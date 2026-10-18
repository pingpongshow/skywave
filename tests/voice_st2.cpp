// Voice through the whole modem (ST2 world: two ModemWorkers, real TCP
// control ports, the radio-chain or FM-radio emulator between them).
// Before these, voice had only run between two in-process LinkEngines on
// a clean channel: never through the worker, under fading, encrypted, on
// VHF, or as a broadcast.
//
// Each case talks real-time speech into A (as the voice port would) and
// plays B out in real time, then reports how much arrived, how faithfully
// (correlation against a local Codec2 round trip of the same speech - a
// perfect channel scores ~1.0) and the latency to first audio.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "skytest.hpp"
#include "st2_world.hpp"
#include "voice/codec.hpp"

using namespace skw;
using namespace st2w;

namespace {

// Voiced test signal (as test_voice): pulse train through two formants
// with a syllabic envelope, so the vocoder has something speech-like.
std::vector<int16_t> testSpeech(double seconds) {
  const size_t n = size_t(seconds * kVoiceSampleRate);
  std::vector<int16_t> pcm(n);
  for (size_t i = 0; i < n; ++i) {
    const double t = double(i) / kVoiceSampleRate;
    const double src = std::pow(std::max(0.0, std::sin(2 * M_PI * 120.0 * t)), 3.0);
    const double f = std::sin(2 * M_PI * 700 * t) + 0.5 * std::sin(2 * M_PI * 1220 * t);
    const double env = 0.55 + 0.45 * std::sin(2 * M_PI * 2.5 * t);
    pcm[i] = int16_t(std::clamp(12000.0 * env * src * f, -32000.0, 32000.0));
  }
  return pcm;
}

std::optional<VoiceCodec> codecFromStatus(const std::string& st) {
  for (auto [name, c] : {std::pair{"3200", VoiceCodec::C3200},
                         std::pair{"2400", VoiceCodec::C2400},
                         std::pair{"1300", VoiceCodec::C1300},
                         std::pair{"700C", VoiceCodec::C700}}) {
    if (st.find(std::string("CODEC=") + name) != std::string::npos ||
        st.find(std::string("(") + name + ")") != std::string::npos) {
      return c;
    }
  }
  return std::nullopt;
}

// What a perfect channel delivers: the same speech through Codec2 alone.
std::vector<int16_t> codecReference(VoiceCodec c, const std::vector<int16_t>& in) {
  VoiceEncoder enc(c);
  enc.pushPcm(in);
  VoiceDecoder dec;
  std::vector<int16_t> out;
  while (auto u = enc.nextUnit(4000, /*flush=*/true)) {
    dec.onVoiceData(*u);
    auto p = dec.takePcm();
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

// Best normalised correlation of `heard` against `ref` over a lag search.
double similarity(const std::vector<int16_t>& ref, const std::vector<int16_t>& heard) {
  if (ref.empty() || heard.empty()) return 0.0;
  auto corrAt = [&](long lag) {
    double xy = 0, xx = 0, yy = 0;
    for (size_t i = 0; i < heard.size(); ++i) {
      const long j = long(i) + lag;
      if (j < 0 || j >= long(ref.size())) continue;
      xy += double(heard[i]) * ref[size_t(j)];
      xx += double(heard[i]) * heard[i];
      yy += double(ref[size_t(j)]) * ref[size_t(j)];
    }
    return (xx > 0 && yy > 0) ? xy / std::sqrt(xx * yy) : 0.0;
  };
  long best = 0;
  double bestC = -1.0;
  for (long lag = -1600; lag <= 1600; lag += 8) {
    const double c = corrAt(lag);
    if (c > bestC) { bestC = c; best = lag; }
  }
  for (long lag = best - 8; lag <= best + 8; ++lag) bestC = std::max(bestC, corrAt(lag));
  return bestC;
}

// Wait for an event line that appears after control-trace offset `from`.
bool waitFor(World& w, Node& n, const std::string& line, int rounds, size_t from = 0) {
  for (int i = 0; i < rounds; ++i) {
    if (n.ctrlAcc.find(line, from) != std::string::npos) return true;
    w.round();
  }
  return n.ctrlAcc.find(line, from) != std::string::npos;
}

struct TalkResult {
  size_t sent = 0, heard = 0;
  double similarity = 0.0;
  double firstAudioSec = -1.0;  // PTT ON -> first sample played at B
  std::string codec;
};

// PTT ON at `tx`, feed `seconds` of speech in real time, PTT OFF, keep
// playing `rx` out in real time until it has been quiet for a while.
TalkResult talk(World& w, Node& tx, Node& rx, double seconds) {
  TalkResult r;
  const auto speech = testSpeech(seconds);
  r.sent = speech.size();
  CHECK(w.cmd(tx, "VOICEPTT ON") == "VOICEPTT ON");
  // Session voice waits for the grant; broadcast talks at once.
  for (int i = 0; i < 3000; ++i) {
    if (w.cmd(tx, "VOICESTATUS").find("STATE=TALKING") != std::string::npos) break;
    w.round();
  }
  const double t0 = double(w.clk.nowUs()) * 1e-6;
  const double perRound = kRoundSec * kVoiceSampleRate;
  double feedAcc = 0.0, takeAcc = 0.0;
  size_t fed = 0;
  bool released = false;
  std::vector<int16_t> heard;
  int quietRounds = 0;
  const int maxRounds = int((seconds + 30.0) / kRoundSec);
  for (int i = 0; i < maxRounds; ++i) {
    if (fed < speech.size()) {
      feedAcc += perRound;
      const size_t n = std::min(size_t(feedAcc), speech.size() - fed);
      feedAcc -= double(n);
      tx.w->submitVoicePcm(std::span<const int16_t>(speech.data() + fed, n));
      fed += n;
    } else if (!released) {
      CHECK(w.cmd(tx, "VOICEPTT OFF") == "VOICEPTT OFF");
      released = true;
    }
    w.round();
    takeAcc += perRound;
    const size_t want = size_t(takeAcc);
    takeAcc -= double(want);
    auto pcm = rx.w->takeVoicePcm(want);
    if (!pcm.empty() && r.firstAudioSec < 0.0) {
      r.firstAudioSec = double(w.clk.nowUs()) * 1e-6 - t0;
    }
    heard.insert(heard.end(), pcm.begin(), pcm.end());
    quietRounds = pcm.empty() ? quietRounds + 1 : 0;
    if (released && heard.size() >= speech.size()) break;
    if (released && quietRounds * kRoundSec > 8.0) break;
  }
  r.heard = heard.size();
  const std::string st = w.cmd(tx, "VOICESTATUS");
  r.codec = st;
  if (auto c = codecFromStatus(st)) {
    r.similarity = similarity(codecReference(*c, speech), heard);
  }
  return r;
}

void report(const char* name, const TalkResult& r) {
  std::printf("  %s: heard %.1f of %.1f s (%.0f%%), similarity %.2f, first audio %.1f s\n"
              "    %s\n",
              name, double(r.heard) / kVoiceSampleRate, double(r.sent) / kVoiceSampleRate,
              100.0 * double(r.heard) / double(std::max<size_t>(r.sent, 1)), r.similarity,
              r.firstAudioSec, r.codec.c_str());
}

// Connect, optionally pin the session mode, and open CONNECTED voice.
bool openSessionVoice(World& w, int pinMode) {
  auto fail = [&w](const char* why, const std::string& got) {
    std::printf("  openSessionVoice: %s (got '%s')\n", why, got.c_str());
    if (std::getenv("VOICE_ST2_TRACE")) {
      std::printf("--- A events:\n");
      size_t p = 0;
      while (p < w.a.ctrlAcc.size()) {
        const size_t e = w.a.ctrlAcc.find('\n', p);
        const std::string l = w.a.ctrlAcc.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (l.rfind("EVT:", 0) == 0) std::printf("    %s\n", l.c_str());
        if (e == std::string::npos) break;
        p = e + 1;
      }
    }
    return false;
  };
  std::string r = w.cmd(w.a, "CONNECT K6XYZ");
  if (r != "CONNECT K6XYZ") return fail("CONNECT", r);
  if (!w.waitState(w.a, "CONNECTED", 6000)) return fail("A connect", w.cmd(w.a, "STATE"));
  if (!w.waitState(w.b, "CONNECTED", 1000)) return fail("B connect", w.cmd(w.b, "STATE"));
  if (pinMode > 0) {
    const std::string m = "M" + std::to_string(pinMode);
    r = w.cmd(w.a, "MODE " + m);
    if (r.rfind("MODE", 0) != 0) return fail("MODE", r);
    for (int i = 0; i < 4000; ++i) {
      // The responder reports MODE=AUTO(Mx) or MODE=Mx.
      const std::string st = w.cmd(w.b, "STATUS");
      if (i % 50 == 0 && (st.find("MODE=" + m + " ") != std::string::npos ||
                          st.find("(" + m + ")") != std::string::npos)) {
        break;
      }
      w.round();
    }
  }
  CHECK(w.cmd(w.a, "VOICE CONNECTED") == "VOICE CONNECTED");
  CHECK(w.cmd(w.b, "VOICE CONNECTED") == "VOICE CONNECTED");
  const size_t mark = w.b.ctrlAcc.size();
  r = w.cmd(w.a, "VOICESTART");
  if (r != "VOICESTART") {
    return fail("VOICESTART", r + " / " + w.cmd(w.a, "STATUS"));
  }
  if (!waitFor(w, w.b, "EVT:VOICE OPEN", 3000, mark)) return fail("B open", w.cmd(w.b, "VOICESTATUS"));
  return true;
}

void setHfChannel(World& w, ChansimParams base, double snrDb) {
  for (Shuttle* s : {&w.ab, &w.ba}) {
    const uint64_t seed = s->chain.seed;
    s->chain.channel = base;
    s->chain.channel.snr3kDb = snrDb;
    s->chain.seed = seed;
  }
}

struct EnvGuard {
  const char* name;
  EnvGuard(const char* n, const char* v) : name(n) { ::setenv(n, v, 1); }
  ~EnvGuard() { ::unsetenv(name); }
};

}  // namespace

TEST_CASE(st2_voice_session_ccir_good) {
  World w(20.0, 31);
  setHfChannel(w, ccirGood(), 20.0);
  CHECK(openSessionVoice(w, 9));
  const TalkResult r = talk(w, w.a, w.b, 8.0);
  report("CCIR Good 20 dB, M9", r);
  CHECK(r.heard >= r.sent * 9 / 10);
  CHECK(r.similarity >= 0.8);
  CHECK(w.cmd(w.a, "STATE") == "STATE CONNECTED");
}

TEST_CASE(st2_voice_session_ccir_moderate) {
  World w(20.0, 32);
  setHfChannel(w, ccirModerate(), 20.0);
  CHECK(openSessionVoice(w, 8));
  const TalkResult r = talk(w, w.a, w.b, 8.0);
  report("CCIR Moderate 20 dB, M8", r);
  // Voice is never retransmitted: fades cost concealed audio, not the
  // session. Most of it must still arrive and be recognisable.
  CHECK(r.heard >= r.sent * 7 / 10);
  CHECK(r.similarity >= 0.5);
  CHECK(w.cmd(w.a, "STATE") == "STATE CONNECTED");
}

TEST_CASE(st2_voice_session_both_directions) {
  World w(24.0, 33);
  CHECK(openSessionVoice(w, 10));
  const TalkResult ab = talk(w, w.a, w.b, 5.0);
  report("A->B static 24 dB, M10", ab);
  const TalkResult ba = talk(w, w.b, w.a, 5.0);
  report("B->A static 24 dB, M10", ba);
  CHECK(ab.heard >= ab.sent * 9 / 10);
  CHECK(ba.heard >= ba.sent * 9 / 10);
  CHECK(ab.similarity >= 0.8);
  CHECK(ba.similarity >= 0.8);
  const size_t mark = w.b.ctrlAcc.size();
  CHECK(w.cmd(w.a, "VOICESTOP") == "VOICESTOP");
  CHECK(waitFor(w, w.b, "EVT:VOICE CLOSED", 3000, mark));
}

#ifdef SKW_HAVE_CRYPTO
TEST_CASE(st2_voice_session_encrypted) {
  EnvGuard psk("SKW_ST2_PSK",
               "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
  World w(24.0, 34);
  CHECK(openSessionVoice(w, 10));
  CHECK(w.cmd(w.a, "STATUS").find("CRYPTO=1") != std::string::npos);
  const TalkResult r = talk(w, w.a, w.b, 5.0);
  report("encrypted, static 24 dB, M10", r);
  CHECK(r.heard >= r.sent * 9 / 10);
  CHECK(r.similarity >= 0.8);
}
#endif

TEST_CASE(st2_voice_session_vhf_fm) {
  EnvGuard band("SKW_ST2_BAND", "vhf");
  World w(30.0, 35);  // CNR 30 dB through the FM radio
  for (Shuttle* s : {&w.ab, &w.ba}) s->fmp.flutterHz = 0.7;  // walking
  CHECK(openSessionVoice(w, 0));  // AUTO: whatever the AMC reaches
  const TalkResult r = talk(w, w.a, w.b, 6.0);
  report("VHF-FM CNR 30 dB, walking flutter", r);
  // Walking flutter fades ~1 s deep: a fade takes a whole unit.
  CHECK(r.heard >= r.sent * 7 / 10);
  CHECK(r.similarity >= 0.6);
  // 700C at a high VHF mode (the u8 frame-count bug, review B4), on a
  // steady channel so every unit must arrive.
  for (Shuttle* s : {&w.ab, &w.ba}) s->fmp.flutterHz = 0.0;
  size_t mark = w.b.ctrlAcc.size();
  CHECK(w.cmd(w.a, "VOICESTOP") == "VOICESTOP");
  CHECK(waitFor(w, w.b, "EVT:VOICE CLOSED", 3000, mark));
  CHECK(w.cmd(w.a, "VOICECODEC 700C").rfind("VOICECODEC", 0) == 0);
  mark = w.b.ctrlAcc.size();
  CHECK(w.cmd(w.a, "VOICESTART") == "VOICESTART");
  CHECK(waitFor(w, w.b, "EVT:VOICE OPEN", 3000, mark));
  const TalkResult r7 = talk(w, w.a, w.b, 6.0);
  report("VHF-FM CNR 30 dB static, 700C", r7);
  CHECK(r7.heard >= r7.sent * 9 / 10);
}

TEST_CASE(st2_voice_broadcast) {
  World w(20.0, 36);
  setHfChannel(w, ccirGood(), 20.0);
  // A broadcasts at the configured mode (voice.broadcast_mode, default
  // M9); B is just listening - no session.
  CHECK(w.cmd(w.a, "VOICE BROADCAST") == "VOICE BROADCAST");
  CHECK(w.cmd(w.a, "VOICESTART") == "VOICESTART");
  const TalkResult r = talk(w, w.a, w.b, 8.0);
  report("broadcast, CCIR Good 20 dB", r);
  CHECK(w.b.saw("EVT:VOICE BCAST "));
  // One-way, never retransmitted: a fade costs a whole unit (~2 s).
  CHECK(r.heard >= r.sent * 7 / 10);
  CHECK(r.similarity >= 0.6);
  CHECK(w.cmd(w.a, "STATE") == "STATE LISTENING");  // no session made
  CHECK(w.cmd(w.b, "STATE") == "STATE LISTENING");
  CHECK(waitFor(w, w.b, "EVT:VOICE BCAST END", 1000));
  CHECK(w.cmd(w.a, "VOICESTOP") == "VOICESTOP");
}

TEST_CASE(st2_voice_broadcast_static) {
  World w(24.0, 36);
  CHECK(w.cmd(w.a, "VOICE BROADCAST") == "VOICE BROADCAST");
  CHECK(w.cmd(w.a, "VOICESTART") == "VOICESTART");
  const TalkResult r = talk(w, w.a, w.b, 12.0);
  report("broadcast, static 24 dB", r);
  CHECK(r.heard >= r.sent * 95 / 100);
  CHECK(r.similarity >= 0.9);
}

SKYTEST_MAIN
