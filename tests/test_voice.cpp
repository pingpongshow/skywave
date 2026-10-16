// Voice codec + talkspurt streaming (ROADMAP 5.4): frame geometry and the
// AUTO codec mapping, VOICE_DATA body round-trip, encoder unit packing,
// an A -> chansim -> B PCM round-trip through the real PHY, and
// concealment behaviour at 5% / 10% unit loss.
//
// Speech-quality artifacts: SKW_VOICE_ARTIFACTS=<dir> writes the decoded
// PCM (and the reference) as raw 8 kHz mono s16le for listening.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "link/frame.hpp"
#include "phy/framer.hpp"
#include "phy/receiver.hpp"
#include "sim/chansim.hpp"
#include "skytest.hpp"
#include "link/engine.hpp"
#include "voice/codec.hpp"
#include "voice/session.hpp"

using namespace skw;

namespace {

// A voiced-ish test signal: a glottal-pulse-like buzz with formants and an
// amplitude envelope. Codec2 is a vocoder, so white noise would tell us
// nothing about whether the chain works.
std::vector<int16_t> testSpeech(double seconds) {
  const size_t n = size_t(seconds * kVoiceSampleRate);
  std::vector<int16_t> pcm(n);
  const double f0 = 120.0;  // pitch
  for (size_t i = 0; i < n; ++i) {
    const double t = double(i) / kVoiceSampleRate;
    const double pitchPhase = 2 * M_PI * f0 * t;
    // Pulse train shaped by two formants, with syllabic envelope.
    const double src = std::pow(std::max(0.0, std::sin(pitchPhase)), 3.0);
    const double f1 = std::sin(2 * M_PI * 700 * t);
    const double f2 = 0.5 * std::sin(2 * M_PI * 1220 * t);
    const double env = 0.55 + 0.45 * std::sin(2 * M_PI * 2.5 * t);
    pcm[i] = int16_t(std::clamp(12000.0 * env * src * (f1 + f2), -32000.0,
                                32000.0));
  }
  return pcm;
}

double rms(const std::vector<int16_t>& x) {
  if (x.empty()) return 0.0;
  double acc = 0;
  for (int16_t v : x) acc += double(v) * v;
  return std::sqrt(acc / double(x.size()));
}

void writeArtifact(const char* name, const std::vector<int16_t>& pcm) {
  const char* dir = getenv("SKW_VOICE_ARTIFACTS");
  if (!dir) return;
  const std::string path = std::string(dir) + "/" + name;
  if (FILE* f = fopen(path.c_str(), "wb")) {
    fwrite(pcm.data(), sizeof(int16_t), pcm.size(), f);
    fclose(f);
    std::printf("  artifact: %s (%zu samples)\n", path.c_str(), pcm.size());
  }
}

}  // namespace

TEST_CASE(voice_codec_geometry_and_mapping) {
  // Frame geometry comes from the library; check the invariant that
  // matters: bytes/frame must carry the codec's bits for its duration.
  for (VoiceCodec c : {VoiceCodec::C700, VoiceCodec::C1300,
                       VoiceCodec::C2400, VoiceCodec::C3200}) {
    const int spf = voiceCodecSamplesPerFrame(c);
    const int bpf = voiceCodecBytesPerFrame(c);
    CHECK(spf > 0);
    CHECK(bpf > 0);
    const double frameSec = double(spf) / kVoiceSampleRate;
    const double bitsNeeded = voiceCodecBps(c) * frameSec;
    std::printf("codec %d: %d samples (%.0f ms) %d bytes, needs %.0f bits\n",
                int(c), spf, frameSec * 1000, bpf, bitsNeeded);
    CHECK(bpf * 8 >= int(bitsNeeded));        // fits
    CHECK(bpf * 8 < int(bitsNeeded) + 16);    // without gross waste
  }
  // AUTO mapping (VOICE.md): no voice in the spread tier, richer modes
  // get better codecs, and the ladder is monotonic in mode.
  CHECK(!autoVoiceCodec(Family::Wide, 7).has_value());
  auto w8 = autoVoiceCodec(Family::Wide, 8);
  auto w13 = autoVoiceCodec(Family::Wide, 13);
  CHECK(w8.has_value());
  CHECK(w13.has_value());
  CHECK(uint8_t(*w13) >= uint8_t(*w8));
  // NARROW is the tightest family: nothing at M8, something high up.
  CHECK(!autoVoiceCodec(Family::Narrow, 8).has_value());
  CHECK(autoVoiceCodec(Family::Narrow, 13).has_value());
  // Manual selection clamps down to what the mode carries.
  auto clamped = clampVoiceCodec(Family::Wide, 8, VoiceCodec::C3200);
  CHECK(clamped.has_value());
  CHECK(uint8_t(*clamped) <= uint8_t(*w8));
  CHECK(!clampVoiceCodec(Family::Wide, 6, VoiceCodec::C700).has_value());
}

TEST_CASE(voice_body_roundtrip_and_malformed) {
  VoiceEncoder enc(VoiceCodec::C1300);
  enc.pushPcm(testSpeech(2.0));
  // 240-byte budget at C1300 holds 33 frames (13.2 s of speech), so this
  // unit only completes as a flush.
  auto unit = enc.nextUnit(240, true);
  CHECK(unit.has_value());
  CHECK(unit->frameCount > 0);
  auto packed = unit->pack();
  auto back = VoiceDataBody::parse(packed);
  CHECK(back.has_value());
  CHECK(back->codec == unit->codec);
  CHECK_EQ(int(back->frameCount), int(unit->frameCount));
  CHECK_EQ(int(back->timestamp), int(unit->timestamp));
  CHECK(back->frames == unit->frames);
  // Malformed: truncated frame payload, bad codec id, trailing garbage.
  CHECK(!VoiceDataBody::parse({packed.data(), packed.size() - 2}).has_value());
  auto badId = packed;
  badId[0] = 9;
  CHECK(!VoiceDataBody::parse(badId).has_value());
  auto extra = packed;
  extra.push_back(0);
  CHECK(!VoiceDataBody::parse(extra).has_value());
}

TEST_CASE(voice_talkspurt_units_fill_budget) {
  // Streaming discipline: units fill the codeword budget while speech is
  // available, and only the final flush emits a short tail.
  VoiceEncoder enc(VoiceCodec::C3200);
  enc.pushPcm(testSpeech(2.0));
  const size_t budget = 120;
  std::vector<VoiceDataBody> units;
  while (auto u = enc.nextUnit(budget, false)) units.push_back(*u);
  CHECK(units.size() >= 3);
  const int bpf = voiceCodecBytesPerFrame(VoiceCodec::C3200);
  const size_t full = (budget - 6) / size_t(bpf);
  for (auto& u : units) {
    CHECK_EQ(int(u.frameCount), int(full));   // every mid-spurt unit full
    CHECK(u.pack().size() <= budget);
  }
  // Timestamps advance contiguously by the speech carried.
  for (size_t i = 1; i < units.size(); ++i) {
    const uint32_t step =
        units[i].timestamp - units[i - 1].timestamp;
    CHECK_EQ(int(step), int(full * size_t(voiceCodecSamplesPerFrame(
                                     VoiceCodec::C3200))));
  }
  auto tail = enc.nextUnit(budget, true);  // PTT end
  if (tail) CHECK(tail->frameCount <= uint8_t(full));
}

TEST_CASE(voice_pcm_roundtrip_over_channel) {
  // A -> real PHY -> chansim -> B -> PCM. Voice rides VOICE_DATA frames
  // at an unspread mode (M9 WIDE) with the voice sequence space.
  const auto ref = testSpeech(3.0);
  const auto codec = autoVoiceCodec(Family::Wide, 9);
  CHECK(codec.has_value());
  VoiceEncoder enc(*codec);
  VoiceDecoder dec;
  enc.pushPcm(ref);

  int units = 0, decoded = 0;
  uint16_t voiceSeq = 0;
  while (auto u = enc.nextUnit(300, true)) {
    ++units;
    LinkEnvelope env;
    env.type = LinkType::VoiceData;
    env.sessionId = 0x1234;
    env.sequence = voiceSeq++;  // independent voice counter
    auto raw = packFrame(env, u->pack());
    TxFrameParams tp;
    tp.family = Family::Wide;
    tp.sig = ChirpSig::Fast;
    tp.mode = 9;
    tp.frameType = uint8_t(LinkType::VoiceData);
    tp.seqLow = uint8_t(env.sequence & 0xFF);
    tp.cpMode = CpMode::Medium;
    tp.payload = raw;
    auto burst = buildFrameBaseband(tp);
    std::vector<cfloat> x(1200, cfloat(0, 0));
    x.insert(x.end(), burst.begin(), burst.end());
    x.insert(x.end(), 1600, cfloat(0, 0));
    ChansimParams cp;
    cp.fading = false;
    cp.snr3kDb = 22.0;
    cp.seed = 900 + uint64_t(units) * 7919;
    auto y = Chansim::run(cp, x);
    auto res = receiveFrame(Family::Wide, y);
    if (!res || !res->payloadOk) continue;
    auto parsed = parseFrame(res->payload);
    if (!parsed || parsed->env.type != LinkType::VoiceData) continue;
    auto body = VoiceDataBody::parse(parsed->body);
    if (!body) continue;
    dec.onVoiceData(*body);
    ++decoded;
  }
  auto out = dec.takePcm();
  std::printf("voice roundtrip: %d units, %d decoded, %zu PCM samples "
              "(ref %zu), rms out/ref = %.2f\n",
              units, decoded, out.size(), ref.size(),
              rms(ref) > 0 ? rms(out) / rms(ref) : 0.0);
  writeArtifact("voice_ref.s16", ref);
  writeArtifact("voice_out.s16", out);
  CHECK(units >= 2);        // 300-byte units carry ~1.5 s each
  CHECK(decoded == units);  // clean channel: every unit survives
  // Vocoded speech is not sample-accurate, but the duration must match
  // and the energy must be in the same ballpark (not silence, not noise).
  CHECK(out.size() >= ref.size() * 9 / 10);
  const double ratio = rms(out) / rms(ref);
  CHECK(ratio > 0.25);
  CHECK(ratio < 4.0);
}

TEST_CASE(voice_concealment_under_loss) {
  // Dropped units must conceal (repeat + attenuate, then silence) and
  // keep the timeline intact - never stall or NAK. 5% and 10% loss.
  for (int lossPct : {5, 10}) {
    const auto ref = testSpeech(20.0);
    const auto codec = VoiceCodec::C1300;
    VoiceEncoder enc(codec);
    VoiceDecoder dec;
    enc.pushPcm(ref);
    uint64_t rng = 12345 + uint64_t(lossPct);
    int units = 0, dropped = 0, droppedFrames = 0;
    size_t expectSamples = 0;
    const int spf = voiceCodecSamplesPerFrame(codec);
    // Small units (~0.3 s each) so a 5% draw over a 20 s spurt actually
    // loses several of them.
    while (auto u = enc.nextUnit(62, true)) {
      ++units;
      expectSamples += size_t(u->frameCount) * size_t(spf);
      rng ^= rng << 13;
      rng ^= rng >> 7;
      rng ^= rng << 17;
      if (int((rng >> 33) % 100) < lossPct) {
        ++dropped;
        droppedFrames += u->frameCount;
        dec.onGap(u->frameCount);  // gap in the voice counter
      } else {
        dec.onVoiceData(*u);
      }
    }
    auto out = dec.takePcm();
    std::printf("conceal %d%%: %d units, %d dropped, %zu/%zu samples, "
                "concealed=%u\n",
                lossPct, units, dropped, out.size(), expectSamples,
                dec.concealedFrames());
    CHECK(units > 20);
    CHECK(dropped > 0);
    // Timeline preserved exactly: concealment fills every lost frame.
    CHECK_EQ(int(out.size()), int(expectSamples));
    CHECK_EQ(int(dec.concealedFrames()), droppedFrames);
    // Output is still speech-like, not silence.
    CHECK(rms(out) > 0.2 * rms(ref));
    writeArtifact(lossPct == 5 ? "voice_loss5.s16" : "voice_loss10.s16", out);
  }
}

TEST_CASE(voice_concealment_decays_to_silence) {
  // A long outage: 2 repeats at -3 dB each, then silence (VOICE.md).
  VoiceEncoder enc(VoiceCodec::C3200);
  VoiceDecoder dec;
  enc.pushPcm(testSpeech(1.0));
  auto u = enc.nextUnit(300, true);
  CHECK(u.has_value());
  dec.onVoiceData(*u);
  auto good = dec.takePcm();
  CHECK(!good.empty());
  const int spf = voiceCodecSamplesPerFrame(VoiceCodec::C3200);
  dec.onGap(6);
  auto conceal = dec.takePcm();
  CHECK_EQ(int(conceal.size()), 6 * spf);
  // First two concealed frames carry (attenuated) speech; by the fourth
  // the output has gone quiet.
  std::vector<int16_t> f1(conceal.begin(), conceal.begin() + spf);
  std::vector<int16_t> f2(conceal.begin() + spf, conceal.begin() + 2 * spf);
  std::vector<int16_t> f4(conceal.begin() + 3 * spf,
                          conceal.begin() + 4 * spf);
  std::printf("conceal decay: rms %.0f -> %.0f -> %.0f\n", rms(f1), rms(f2),
              rms(f4));
  CHECK(rms(f1) > 0.0);
  CHECK(rms(f2) < rms(f1));   // attenuation applied per repeat
  CHECK_EQ(int(rms(f4)), 0);  // silence after 2 repeats
  CHECK_EQ(int(dec.concealedFrames()), 6);
}

// --- 5.5: session control, jitter buffer, voice port, two-modem loop -------

TEST_CASE(voice_fsm_talkspurt_arbitration) {
  VoiceFsm a, b;
  auto drain = [](VoiceFsm& f) {
    std::vector<VoiceAction> acts;
    while (auto x = f.nextAction()) acts.push_back(*x);
    return acts;
  };
  auto has = [](const std::vector<VoiceAction>& v, VoiceAction a) {
    for (auto x : v) {
      if (x == a) return true;
    }
    return false;
  };
  // Open handshake.
  a.cmdOpen(VoiceCodec::C1300, 0.0);
  CHECK(has(drain(a), VoiceAction::SendOpen));
  b.rxOpen(VoiceCodec::C1300, 0.1);
  auto bActs = drain(b);
  CHECK(has(bActs, VoiceAction::SendOpenAck));
  CHECK(has(bActs, VoiceAction::IndicateOpen));
  a.rxOpenAck(VoiceCodec::C1300, 0.2);
  CHECK(has(drain(a), VoiceAction::IndicateOpen));
  CHECK(a.state() == VoiceState::Open);
  CHECK(b.state() == VoiceState::Open);

  // A takes the talkspurt.
  a.cmdPttOn(1.0);
  CHECK(has(drain(a), VoiceAction::SendPttReq));
  b.rxPttReq(1.1, false);
  CHECK(has(drain(b), VoiceAction::SendPttGrant));
  CHECK(b.state() == VoiceState::Receiving);
  a.rxPttGrant(1.2);
  CHECK(has(drain(a), VoiceAction::IndicateTalking));
  CHECK(a.talking());
  // B asking mid-spurt is ignored until A releases.
  b.rxPttReq(1.5, false);
  a.rxPttReq(1.5, true);
  CHECK(a.talking());
  a.cmdPttOff(2.0);
  CHECK(has(drain(a), VoiceAction::SendPttEnd));
  CHECK(!a.talking());
  b.rxPttEnd(2.1);
  CHECK(has(drain(b), VoiceAction::IndicateStopped));
  CHECK(b.state() == VoiceState::Open);

  // Simultaneous request: the tie-break must leave exactly one talker.
  VoiceFsm x, y;
  x.rxOpen(VoiceCodec::C1300, 0.0);
  y.rxOpen(VoiceCodec::C1300, 0.0);
  (void)drain(x);
  (void)drain(y);
  x.cmdPttOn(3.0);
  y.cmdPttOn(3.0);
  (void)drain(x);
  (void)drain(y);
  x.rxPttReq(3.1, true);   // x wins the tie
  y.rxPttReq(3.1, false);  // y yields
  const bool yGrants = has(drain(y), VoiceAction::SendPttGrant);
  CHECK(yGrants);
  CHECK(x.state() == VoiceState::Requesting);
  CHECK(y.state() == VoiceState::Receiving);
  x.rxPttGrant(3.2);
  CHECK(x.talking());
  CHECK(!y.talking());  // never both

  // Talkspurt hold cap releases a channel whose peer vanished.
  VoiceFsm z;
  z.rxOpen(VoiceCodec::C1300, 0.0);
  (void)drain(z);
  z.rxPttReq(10.0, false);
  (void)drain(z);
  CHECK(z.state() == VoiceState::Receiving);
  z.tick(10.0 + 400.0);
  CHECK(has(drain(z), VoiceAction::IndicateStopped));
  CHECK(z.state() == VoiceState::Open);

  // Unanswered OPEN gives up after the configured attempts.
  VoiceFsm w;
  w.cmdOpen(VoiceCodec::C700, 0.0);
  (void)drain(w);
  double t = 0.0;
  for (int i = 0; i < 4; ++i) {
    t += 9.0;
    w.tick(t);
    (void)drain(w);
  }
  CHECK(w.state() == VoiceState::Closed);
}

TEST_CASE(voice_jitter_buffer) {
  JitterBuffer jb;
  const std::vector<int16_t> chunk(800, 100);  // 100 ms
  // Below the 240 ms target the buffer holds everything back.
  jb.push(chunk);
  CHECK(!jb.primed());
  CHECK(jb.take(160).empty());
  jb.push(chunk);
  jb.push(chunk);  // 300 ms
  CHECK(jb.primed());
  auto got = jb.take(160);
  CHECK_EQ(int(got.size()), 160);
  CHECK_EQ(int(jb.depthMs()), 280);
  // Drain past the end: underrun counted, buffer re-primes.
  size_t guard = 0;
  while (jb.depthSamples() > 0 && guard++ < 100) jb.take(400);
  jb.take(400);
  CHECK(jb.underruns() > 0);
  CHECK(!jb.primed());
}

TEST_CASE(voice_port_records) {
  VoiceRecord rec;
  rec.type = kVoiceRecUplink;
  rec.sequence = 0x11223344;
  rec.sampleCounter = 0xAABBCCDD01020304ull;
  rec.pcm = {1, -2, 3, -32768, 32767};
  auto bytes = rec.pack();
  CHECK_EQ(int(bytes.size()), int(16 + rec.pcm.size() * 2));
  CHECK_EQ(int(bytes[0]), int(kVoiceRecVersion));
  VoiceRecordParser p;
  // Split across two feeds: the parser must reassemble.
  p.feed({bytes.data(), 7});
  CHECK(p.take().empty());
  p.feed({bytes.data() + 7, bytes.size() - 7});
  auto got = p.take();
  CHECK_EQ(int(got.size()), 1);
  CHECK_EQ(int(got[0].type), int(kVoiceRecUplink));
  CHECK_EQ(int(got[0].sequence), int(rec.sequence));
  CHECK(got[0].sampleCounter == rec.sampleCounter);
  CHECK(got[0].pcm == rec.pcm);
  // Back-to-back records in one feed.
  std::vector<uint8_t> two = bytes;
  two.insert(two.end(), bytes.begin(), bytes.end());
  VoiceRecordParser p2;
  p2.feed(two);
  CHECK_EQ(int(p2.take().size()), 2);
  // Garbage version: counted and flushed, never interpreted as PCM.
  VoiceRecordParser p3;
  std::vector<uint8_t> bad(32, 0xEE);
  p3.feed(bad);
  CHECK_EQ(int(p3.badRecords()), 1);
  CHECK(p3.take().empty());
}

namespace {

struct VoicePair {
  LinkEngine a, b;
  ChansimParams chanAB, chanBA;
  double now = 0.0;
  uint64_t seedCtr = 1;
  int lossEvery = 0;
  int passes = 0;

  VoicePair() : a(mk("W1AW")), b(mk("K6XYZ")) {
    chanAB.fading = false;
    chanAB.snr3kDb = 26.0;
    chanBA = chanAB;
    chanAB.seed = 5100;
    chanBA.seed = 9100;
  }

  static EngineConfig mk(const char* call) {
    EngineConfig c;
    c.policy.ownCall = call;
    c.policy.caps = kCapZstd | kCapResume;
    return c;
  }

  void step() {
    auto bA = a.tick(now);
    auto bB = b.tick(now);
    double dt = 0.05;
    auto push = [&](std::vector<cfloat>& burst, LinkEngine& to,
                    ChansimParams& ch, bool drop) {
      dt += double(burst.size()) / kFsBaseband;
      if (drop) return;
      std::vector<cfloat> x(1500, cfloat(0, 0));
      x.insert(x.end(), burst.begin(), burst.end());
      x.insert(x.end(), 2000, cfloat(0, 0));
      ChansimParams cp = ch;
      cp.seed = ch.seed + (seedCtr++) * 7919;
      auto y = Chansim::run(cp, x);
      to.rxBurst(y, now);
    };
    ++passes;
    const bool drop = lossEvery > 0 && (passes % lossEvery) == 0;
    if (bA) push(*bA, b, chanAB, drop);
    if (bB) push(*bB, a, chanBA, false);
    now += dt;
  }

  bool connect() {
    a.connectTo("K6XYZ", now);
    for (int i = 0; i < 600; ++i) {
      step();
      if (a.state() == FsmState::Connected &&
          b.state() == FsmState::Connected) {
        return true;
      }
    }
    return false;
  }

  bool pinMode(int m) {
    if (!a.pinMode(m, now)) return false;
    for (int i = 0; i < 80 && b.sessionMode() != m; ++i) step();
    return a.sessionMode() == m && b.sessionMode() == m;
  }
};

}  // namespace

TEST_CASE(voice_two_modem_loop) {
  // ROADMAP 5.5 gate: open voice over a live session, run a talkspurt of
  // real speech from A to B, verify B plays it out through the jitter
  // buffer, then hand the channel to B and back.
  VoicePair p;
  CHECK(p.connect());
  CHECK(p.pinMode(10));  // unspread mode: voice-capable

  CHECK(p.a.openVoice(std::nullopt, p.now));  // AUTO codec
  for (int i = 0; i < 200 && (p.a.voiceState() != VoiceState::Open ||
                              p.b.voiceState() != VoiceState::Open);
       ++i) {
    p.step();
  }
  CHECK(p.a.voiceState() == VoiceState::Open);
  CHECK(p.b.voiceState() == VoiceState::Open);
  std::printf("voice loop: codec=%d transport=M%d\n",
              int(p.a.voiceCodec()), p.a.sessionMode());

  // A requests the talkspurt and streams 4 s of speech.
  CHECK(p.a.setVoicePtt(true, p.now));
  for (int i = 0; i < 200 && p.a.voiceState() != VoiceState::Talking; ++i) {
    p.step();
  }
  CHECK(p.a.voiceState() == VoiceState::Talking);
  CHECK(p.b.voiceState() == VoiceState::Receiving);

  const auto speech = testSpeech(4.0);
  p.a.pushVoicePcm(speech);
  std::vector<int16_t> heard;
  for (int i = 0; i < 4000 && heard.size() < speech.size() / 2; ++i) {
    p.step();
    auto pcm = p.b.takeVoicePcm(800);
    heard.insert(heard.end(), pcm.begin(), pcm.end());
    if (p.a.state() != FsmState::Connected) break;
  }
  std::printf("voice loop: heard %zu samples of %zu sent, rms ratio %.2f\n",
              heard.size(), speech.size(),
              rms(speech) > 0 ? rms(heard) / rms(speech) : 0.0);
  writeArtifact("voice_loop_rx.s16", heard);
  CHECK(heard.size() >= speech.size() / 2);  // half of it played out
  CHECK(rms(heard) > 0.2 * rms(speech));     // and it is speech

  // Release, then B takes the channel: the arbitration reverses.
  CHECK(p.a.setVoicePtt(false, p.now));
  for (int i = 0; i < 200 && p.b.voiceState() != VoiceState::Open; ++i) {
    p.step();
  }
  CHECK(p.b.voiceState() == VoiceState::Open);
  CHECK(p.b.setVoicePtt(true, p.now));
  for (int i = 0; i < 300 && p.b.voiceState() != VoiceState::Talking; ++i) {
    p.step();
  }
  CHECK(p.b.voiceState() == VoiceState::Talking);
  CHECK(p.a.voiceState() == VoiceState::Receiving);

  // Close from A: both ends return to CLOSED.
  p.a.closeVoice(p.now);
  for (int i = 0; i < 200 && p.b.voiceState() != VoiceState::Closed; ++i) {
    p.step();
  }
  CHECK(p.a.voiceState() == VoiceState::Closed);
  CHECK(p.b.voiceState() == VoiceState::Closed);
  // The data session is untouched by the voice service.
  CHECK(p.a.state() == FsmState::Connected);
}

TEST_CASE(voice_loop_conceals_lost_frames) {
  // Voice frames are never retransmitted: dropped VOICE_DATA must show up
  // as concealment, and the session must not stall or NAK.
  VoicePair p;
  CHECK(p.connect());
  CHECK(p.pinMode(10));
  CHECK(p.a.openVoice(std::nullopt, p.now));
  for (int i = 0; i < 300 && p.b.voiceState() != VoiceState::Open; ++i) {
    p.step();
  }
  CHECK(p.b.voiceState() == VoiceState::Open);
  CHECK(p.a.setVoicePtt(true, p.now));
  for (int i = 0; i < 300 && p.a.voiceState() != VoiceState::Talking; ++i) {
    p.step();
  }
  CHECK(p.a.voiceState() == VoiceState::Talking);
  // Loss starts only now: the subject here is VOICE_DATA loss, and
  // dropping the OPEN/PTT control frames instead would just test the
  // FSM retries (covered above) while stalling the spurt.
  p.lossEvery = 5;  // drop 1 in 5 A->B bursts
  p.a.pushVoicePcm(testSpeech(6.0));
  std::vector<int16_t> heard;
  for (int i = 0; i < 1200 && p.a.voiceTxQueued() > 0; ++i) {
    p.step();
    auto pcm = p.b.takeVoicePcm(800);
    heard.insert(heard.end(), pcm.begin(), pcm.end());
    if (p.a.state() != FsmState::Connected) break;
  }
  // Drain whatever the jitter buffer still holds.
  for (int i = 0; i < 200; ++i) {
    p.step();
    auto pcm = p.b.takeVoicePcm(800);
    heard.insert(heard.end(), pcm.begin(), pcm.end());
  }
  std::printf("voice loss: heard %zu samples, concealed %u frames\n",
              heard.size(), p.b.voiceConcealed());
  CHECK(p.a.state() == FsmState::Connected);  // no teardown from voice loss
  CHECK(p.b.voiceConcealed() > 0);            // gaps were concealed
  CHECK(!heard.empty());
}

// --- review fixes (B4, B5, C14, C15, C16) ------------------------------------

TEST_CASE(voice_unit_frame_count_fits_u8) {
  // B4: a big codeword at C700 holds >255 frames; the u8 count must never
  // wrap (306 -> 50 made every unit fail the receiver's length check).
  VoiceEncoder enc(VoiceCodec::C700);
  enc.pushPcm(std::vector<int16_t>(size_t(400) * 320, 100));
  auto u = enc.nextUnit(1300, false);
  CHECK(u.has_value());
  if (u) {
    CHECK_EQ(int(u->frameCount), 255);
    const auto wire = u->pack();
    auto back = VoiceDataBody::parse(wire);
    CHECK(back.has_value());
    if (back) CHECK_EQ(int(back->frameCount), 255);
  }
}

TEST_CASE(voice_encoder_flush_and_discard) {
  // C16: a flush drains the whole tail, including the sub-frame
  // remainder (padded), and discardPcm drops stale mic audio.
  VoiceEncoder enc(VoiceCodec::C3200);
  const int spf = voiceCodecSamplesPerFrame(VoiceCodec::C3200);
  enc.pushPcm(std::vector<int16_t>(size_t(spf) * 3 + 7, 50));
  CHECK(!enc.nextUnit(300, false).has_value());  // not a full unit yet
  int frames = 0;
  while (auto u = enc.nextUnit(300, true)) frames += u->frameCount;
  CHECK_EQ(frames, 4);
  CHECK_EQ(int(enc.pendingSamples()), 0);
  enc.pushPcm(std::vector<int16_t>(1000, 50));
  enc.discardPcm();
  CHECK_EQ(int(enc.pendingSamples()), 0);
  // Backlog is bounded even if nobody drains it.
  for (int i = 0; i < 40; ++i) enc.pushPcm(std::vector<int16_t>(8000, 1));
  CHECK(enc.pendingSamples() <= VoiceEncoder::kMaxPendingSamples);
}

TEST_CASE(voice_gap_concealment_bounded) {
  // B5: the gap size comes from the air; any value must stay bounded.
  VoiceEncoder enc(VoiceCodec::C1300);
  VoiceDecoder dec;
  enc.pushPcm(testSpeech(1.0));
  auto u = enc.nextUnit(62, true);
  CHECK(u.has_value());
  if (u) dec.onVoiceData(*u);
  (void)dec.takePcm();
  dec.onGap(2000000000);
  const auto pcm = dec.takePcm();
  CHECK(!pcm.empty());
  CHECK(int(pcm.size()) <=
        VoiceDecoder::kMaxConcealMs * kVoiceSampleRate / 1000);
}

TEST_CASE(voice_conceal_after_zero_frame_codec_switch) {
  // C14: a zero-frame unit under a bigger codec must not make concealment
  // decode the old (smaller) frame with the new codec's frame size.
  VoiceEncoder enc(VoiceCodec::C700);
  VoiceDecoder dec;
  enc.pushPcm(testSpeech(1.0));
  auto u = enc.nextUnit(40, true);
  CHECK(u.has_value());
  if (u) dec.onVoiceData(*u);
  (void)dec.takePcm();
  VoiceDataBody empty;
  empty.codec = VoiceCodec::C3200;
  empty.frameCount = 0;
  dec.onVoiceData(empty);
  dec.onGap(2);
  // Still concealing with the codec the stored frame belongs to.
  CHECK_EQ(int(dec.takePcm().size()),
           2 * voiceCodecSamplesPerFrame(VoiceCodec::C700));
}

TEST_CASE(voice_jitter_buffer_bounded) {
  JitterBuffer jb;
  const std::vector<int16_t> chunk(8000, 7);  // 1 s
  for (int i = 0; i < 60; ++i) jb.push(chunk);
  CHECK(jb.depthMs() <= JitterBuffer::kMaxMs);
  CHECK(jb.depthMs() >= JitterBuffer::kMaxMs - 1);
}

TEST_CASE(voice_fsm_recovers_lost_grant_and_end) {
  // C15: neither a lost PTT_GRANT nor a lost PTT_END may lock the voice
  // channel for the 180 s hold cap.
  auto drain = [](VoiceFsm& f) {
    std::vector<VoiceAction> acts;
    while (auto x = f.nextAction()) acts.push_back(*x);
    return acts;
  };
  auto has = [](const std::vector<VoiceAction>& v, VoiceAction a) {
    for (auto x : v) {
      if (x == a) return true;
    }
    return false;
  };
  VoiceFsm a, b;
  a.rxOpen(VoiceCodec::C1300, 0.0);
  b.rxOpen(VoiceCodec::C1300, 0.0);
  (void)drain(a);
  (void)drain(b);

  // Lost grant: A re-requests on timeout, B (already Receiving) re-grants.
  a.cmdPttOn(1.0);
  CHECK(has(drain(a), VoiceAction::SendPttReq));
  b.rxPttReq(1.1, false);
  CHECK(has(drain(b), VoiceAction::SendPttGrant));  // ...lost on the air
  a.tick(1.0 + 8.5);
  CHECK(has(drain(a), VoiceAction::SendPttReq));    // retry
  CHECK(a.state() == VoiceState::Requesting);
  b.rxPttReq(9.6, false);
  CHECK(has(drain(b), VoiceAction::SendPttGrant));  // re-grant
  a.rxPttGrant(9.7);
  CHECK(a.talking());

  // Lost end: B heard speech, then nothing - it frees itself after the
  // idle window, well before the hold cap.
  b.rxVoiceActivity(12.0);
  a.cmdPttOff(13.0);
  (void)drain(a);  // PTT_END lost
  b.tick(12.0 + 19.0);
  CHECK(b.state() == VoiceState::Receiving);
  b.tick(12.0 + 21.0);
  CHECK(has(drain(b), VoiceAction::IndicateStopped));
  CHECK(b.state() == VoiceState::Open);

  // Lost end, but A keys again first: B re-grants instead of ignoring.
  VoiceFsm c;
  c.rxOpen(VoiceCodec::C1300, 0.0);
  (void)drain(c);
  c.rxPttReq(1.0, false);
  (void)drain(c);
  c.rxPttReq(5.0, false);  // peer's next spurt; its END was lost
  CHECK(has(drain(c), VoiceAction::SendPttGrant));
  CHECK(c.state() == VoiceState::Receiving);

  // Capture gating: mic audio only belongs to a spurt while holding PTT.
  CHECK(!c.capturing());  // Receiving
  CHECK(!a.capturing());  // Open after releasing
  a.cmdPttOn(40.0);
  CHECK(a.capturing());   // Requesting: speech before the grant is kept
}

SKYTEST_MAIN
