// System test layer ST1 (TESTING.md): the full modem stack exercised at
// the REAL 48 kHz audio domain through the radio-chain emulator - rig
// filters, ALC, PTT switching clips, hum/DC, impulse static, soundcard
// clock offsets, Watterson path. Includes stress/soak (CI-scaled; set
// SKW_SOAK=N for the long run) and hostile-audio fuzz.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "dsp/updown.hpp"
#include "link/engine.hpp"
#include "sim/radiochain.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

// Two engines exchanging through 48 kHz audio + the radio chain. Each
// direction has independent chain parameters (like two real rigs).
struct AudioPair {
  LinkEngine a, b;
  RadioChainParams chainAB, chainBA;
  double now = 0.0;
  uint64_t seedCtr = 1;

  explicit AudioPair(double snrDb, uint64_t seed)
      : a(mk("W1AW")), b(mk("K6XYZ")) {
    chainAB.channel.fading = false;
    chainAB.channel.snr3kDb = snrDb;
    chainBA = chainAB;
    chainAB.seed = seed;
    chainBA.seed = seed + 5000;
  }

  static EngineConfig mk(const char* call) {
    EngineConfig c;
    c.policy.ownCall = call;
    c.policy.caps = kCapZstd | kCapResume;
    return c;
  }

  void step() {
    exchange(a, b, chainAB);
    exchange(b, a, chainBA);
  }

  void exchange(LinkEngine& from, LinkEngine& to, RadioChainParams& chain) {
    auto burst = from.tick(now);
    double dt = 0.05;
    if (burst) {
      dt += double(burst->size()) / kFsBaseband;
      // Modem TX: 12 kHz complex -> real 48 kHz audio (the worker path).
      Upconverter up;
      std::vector<float> audio;
      std::vector<cfloat> padded(600, cfloat(0, 0));
      padded.insert(padded.end(), burst->begin(), burst->end());
      padded.insert(padded.end(), 800, cfloat(0, 0));
      up.process(padded, audio);
      // Rig + ionosphere + rig.
      chain.channel.seed = chain.seed + (seedCtr++) * 7919;
      auto rxAudio = radioChain(chain, audio);
      // Peer soundcard capture -> 12 kHz complex -> receiver.
      Downconverter down;
      std::vector<cfloat> bb;
      down.process(rxAudio, bb);
      to.rxBurst(bb, now);
    }
    now += dt;
  }

  bool connect(int maxSteps = 400) {
    a.connectTo("K6XYZ", now);
    for (int i = 0; i < maxSteps; ++i) {
      step();
      if (a.state() == FsmState::Connected &&
          b.state() == FsmState::Connected) {
        return true;
      }
    }
    return false;
  }

  bool transfer(size_t bytes, uint8_t tag, int budget = 4000) {
    // Incompressible payload: keep the ARQ/PHY stress independent of the
    // proto layer's zstd (which would crush a periodic pattern).
    std::vector<uint8_t> blob(bytes);
    uint64_t s = 0x9E3779B97F4A7C15ull ^ (uint64_t(tag) << 32);
    for (auto& v : blob) {
      s ^= s << 13;
      s ^= s >> 7;
      s ^= s << 17;
      v = uint8_t(s >> 32);
    }
    a.sendData(blob);
    std::vector<uint8_t> got;
    for (int i = 0; i < budget && got.size() < blob.size(); ++i) {
      step();
      auto d = b.takeReceivedData();
      got.insert(got.end(), d.begin(), d.end());
      if (a.state() != FsmState::Connected) return false;
    }
    return got == blob;
  }
};

}  // namespace

TEST_CASE(system_clean_audio_path) {
  // Full loop through the real converters and rig filters, no
  // impairments: the audio-domain integration itself must be lossless.
  AudioPair p(25.0, 10);
  CHECK(p.connect());
  CHECK(p.transfer(6 * 1024, 1));
}

TEST_CASE(system_rig_impairments) {
  // Everything at once, at realistic severity: hot ALC, drive error,
  // PTT switching clips, 60 Hz hum + harmonic, DC offset, static
  // crashes, +-25 ppm soundcard offsets, CCIR Good fading at 18 dB.
  AudioPair p(18.0, 20);
  for (RadioChainParams* c : {&p.chainAB, &p.chainBA}) {
    c->alcKnee = 0.4;
    c->driveLevelDb = -6.0;
    c->pttLeadClipMs = 8.0;
    c->pttTailClipMs = 3.0;
    c->humHz = 60.0;
    c->humAmp = 0.02;
    c->dcOffset = 0.01;
    c->impulsePerSec = 2.0;
    c->impulseAmp = 0.3;
    c->channel = ccirGood();
    c->channel.snr3kDb = 18.0;
  }
  p.chainAB.soundcardPpm = 25.0;
  p.chainBA.soundcardPpm = -25.0;
  CHECK(p.connect());
  CHECK(p.transfer(3 * 1024, 2, 6000));
}

TEST_CASE(system_ptt_lead_clip_tolerance) {
  // Rig keying delay eats the head of every burst; the calling preamble
  // (4 chirp reps) must survive substantial clipping.
  for (double clipMs : {10.0, 25.0, 40.0}) {
    AudioPair p(22.0, 30 + uint64_t(clipMs));
    p.chainAB.pttLeadClipMs = clipMs;
    p.chainBA.pttLeadClipMs = clipMs;
    bool ok = p.connect() && p.transfer(1024, 3);
    std::printf("ptt lead clip %.0f ms: %s\n", clipMs, ok ? "ok" : "FAIL");
    CHECK(ok);
  }
}

TEST_CASE(system_hostile_audio_fuzz) {
  // Garbage on the air must never crash the stack or fabricate frames:
  // tones, wideband noise, AM-modulated carriers, silence with hum.
  EngineConfig ec;
  ec.policy.ownCall = "W1AW";
  LinkEngine e(ec);
  uint64_t rng = 777;
  auto nextU = [&]() {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };
  for (int trial = 0; trial < 12; ++trial) {
    std::vector<float> audio(kFsAudio * 2, 0.0f);  // 2 s
    const int kind = trial % 4;
    for (size_t i = 0; i < audio.size(); ++i) {
      const double t = double(i) / kFsAudio;
      switch (kind) {
        case 0:  // steady tone inside the band
          audio[i] = 0.5f * float(std::sin(2 * M_PI * (600 + 90 * trial) * t));
          break;
        case 1:  // white noise
          audio[i] = float((double(nextU() >> 40) / (1 << 24)) - 0.5);
          break;
        case 2:  // AM carrier (SSB voice-ish envelope)
          audio[i] = float(0.4 * (1 + 0.8 * std::sin(2 * M_PI * 3 * t)) *
                           std::sin(2 * M_PI * 1500 * t));
          break;
        default:  // near-silence with hum
          audio[i] = 0.02f * float(std::sin(2 * M_PI * 60 * t));
          break;
      }
    }
    Downconverter down;
    std::vector<cfloat> bb;
    down.process(audio, bb);
    e.rxBurst(bb, double(trial));
    (void)e.tick(double(trial) + 0.5);
  }
  CHECK(e.state() == FsmState::Idle);   // no fabricated session
  CHECK(e.takeReceivedData().empty());  // no fabricated data
}

TEST_CASE(system_soak_cycles) {
  // Connect -> transfer -> disconnect cycles with per-cycle randomized
  // impairments. CI runs a short pass; SKW_SOAK=N runs N cycles for the
  // long-haul stress (leaks, state corruption, throughput drift).
  int cycles = 3;
  if (const char* s = getenv("SKW_SOAK")) cycles = std::max(1, atoi(s));
  int okCount = 0;
  for (int c = 0; c < cycles; ++c) {
    const uint64_t seed = 900 + uint64_t(c) * 613;
    AudioPair p(16.0 + (c % 3) * 6.0, seed);
    for (RadioChainParams* ch : {&p.chainAB, &p.chainBA}) {
      ch->alcKnee = (c % 2) ? 0.5 : 0.0;
      ch->pttLeadClipMs = double(c % 3) * 6.0;
      ch->humHz = 60.0;
      ch->humAmp = 0.01;
      ch->impulsePerSec = double(c % 2);
      ch->impulseAmp = 0.25;
      ch->soundcardPpm = (c % 2) ? 20.0 : -20.0;
    }
    const double t0 = p.now;
    bool ok = p.connect() && p.transfer(2048, uint8_t(c), 5000);
    if (ok) {
      p.a.disconnect(p.now);
      for (int i = 0; i < 60 && (p.a.state() != FsmState::Idle ||
                                 p.b.state() != FsmState::Idle);
           ++i) {
        p.step();
      }
      ok = p.a.state() == FsmState::Idle && p.b.state() == FsmState::Idle;
    }
    std::printf("soak cycle %d: %s (%.0f bps)\n", c, ok ? "ok" : "FAIL",
                ok ? 2048 * 8.0 / (p.now - t0) : 0.0);
    if (ok) ++okCount;
  }
  CHECK_EQ(okCount, cycles);
}

SKYTEST_MAIN
