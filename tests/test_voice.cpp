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
#include "voice/codec.hpp"

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

SKYTEST_MAIN
