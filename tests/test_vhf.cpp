// VHF-FM band profile (ROADMAP Phase 7): profile activation, connect /
// AMC ramp / transfer / file scenarios under band=vhf-fm, the M7
// gate - >= 25 kbps net sustained over the flat-channel sim - and the
// same link through the FM radio emulator (sim/fmradio.hpp).
#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp/updown.hpp"
#include "link/engine.hpp"
#include "phy/framer.hpp"
#include "phy/receiver.hpp"
#include "sim/chansim.hpp"
#include "sim/fmradio.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

std::vector<uint8_t> noiseBlob(size_t n, uint64_t seed) {
  std::vector<uint8_t> v(n);
  uint64_t s = seed * 2654435761u + 1;
  for (auto& b : v) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    b = uint8_t(s >> 32);
  }
  return v;
}

// RAII band selection: every case restores HF for the rest of the
// binary (and the suite).
struct VhfBand {
  VhfBand() { setActiveBand(Band::VhfFm); }
  ~VhfBand() { setActiveBand(Band::Hf); }
};

struct Pair {
  LinkEngine a, b;
  ChansimParams chan;
  // FM mode: each burst is upconverted at the worker's TX level, run
  // through the FM radio emulator, and downconverted, instead of the
  // baseband chansim.
  bool fm = false;
  FmRadioParams fmp;
  double now = 0.0;
  uint64_t seedCtr = 1;

  explicit Pair(double snrDb, uint64_t seed)
      : a(makeCfg("W1AW")), b(makeCfg("K6XYZ")) {
    chan = vhfFlat();
    chan.snr3kDb = snrDb;
    chan.seed = seed;
  }

  static EngineConfig makeCfg(const char* call) {
    EngineConfig c;
    c.policy.ownCall = call;
    c.policy.family = Family::Wide;  // resolves to the VHF plan
    c.policy.maxMode = 14;
    c.policy.caps = kCapZstd | kCapResume | kCapHarqIr;
    return c;
  }

  void step() {
    auto burstA = a.tick(now);
    auto burstB = b.tick(now);
    double dt = (burstA || burstB) ? 0.02 : 0.25;
    auto deliver = [&](std::vector<cfloat>& burst, LinkEngine& rx) {
      dt += double(burst.size()) / kFsBaseband;
      std::vector<cfloat> x(600, cfloat(0, 0));
      x.insert(x.end(), burst.begin(), burst.end());
      x.insert(x.end(), 1200, cfloat(0, 0));
      ChansimParams cp = chan;
      cp.seed = chan.seed + (seedCtr++) * 7919;
      if (fm) {
        Upconverter up;
        std::vector<float> audio;
        up.process(x, audio);
        const float g = float(activeProfile().txTargetRms * std::sqrt(2.0));
        for (auto& v : audio) v *= g;
        FmRadioParams rp = fmp;
        rp.seed = cp.seed;
        Downconverter dn;
        std::vector<cfloat> y;
        dn.process(fmRadio(rp, audio), y);
        rx.rxBurst(y, now);
        return;
      }
      auto y = Chansim::run(cp, x);
      rx.rxBurst(y, now);
    };
    if (burstA) deliver(*burstA, b);
    if (burstB) deliver(*burstB, a);
    now += dt;
  }

  bool connect(int maxSteps = 40) {
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
};

}  // namespace

TEST_CASE(vhf_profile_activation) {
  VhfBand band;
  CHECK_EQ(plan(Family::Wide).carriers(), 176);
  CHECK_NEAR(centerHz(), 3600.0, 1e-9);
  CHECK(!activeProfile().cssTier);
  CHECK(!activeProfile().masking);
  CHECK_EQ(chirpRepSamples(), 480);
  CHECK_EQ(cpSamplesAudio(CpMode::Short), 32);
  CHECK_NEAR(fadingFloorDb(Family::Wide, 14), 33.0, 1e-9);
  setActiveBand(Band::Hf);
  CHECK_EQ(plan(Family::Wide).carriers(), 64);
  CHECK_NEAR(centerHz(), 1500.0, 1e-9);
  setActiveBand(Band::VhfFm);  // dtor restores HF again
}

TEST_CASE(vhf_connect_bulk_disconnect) {
  VhfBand band;
  Pair p(20.0, 7);
  CHECK(p.connect());
  if (p.a.state() != FsmState::Connected) return;
  CHECK(p.a.sessionMode() >= 4);
  auto blob = noiseBlob(4000, 3);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  for (int i = 0; i < 200 && got.size() < blob.size(); ++i) {
    p.step();
    auto r = p.b.takeReceivedData();
    got.insert(got.end(), r.begin(), r.end());
  }
  CHECK(got == blob);
  p.a.disconnect(p.now);
  for (int i = 0;
       i < 80 && (p.a.state() != FsmState::Idle ||
                  p.b.state() != FsmState::Idle);
       ++i) {
    p.step();
  }
  CHECK(p.a.state() == FsmState::Idle);
  CHECK(p.b.state() == FsmState::Idle);
}

TEST_CASE(vhf_amc_ramps_to_top) {
  VhfBand band;
  Pair p(40.0, 21);
  CHECK(p.connect());
  if (p.a.state() != FsmState::Connected) return;
  auto blob = noiseBlob(1500, 9);
  int last = p.a.sessionMode();
  for (int i = 0; i < 900 && p.a.sessionMode() < 14; ++i) {
    if (i % 6 == 0) p.a.sendData(blob);
    p.step();
    (void)p.b.takeReceivedData();
    last = p.a.sessionMode();
  }
  std::printf("  vhf ramp reached M%d at t=%.0f s\n", last, p.now);
  CHECK(p.a.sessionMode() >= 13);  // M14 preferred; >= M13 required
}

TEST_CASE(vhf_m7_throughput_gate) {
  // Milestone M7: >= 25 kbps NET sustained over the flat-channel sim.
  // Mode pinned to M14 (operator pin is a first-class control); the
  // measurement covers everything - preamble/sync/header overhead, MAC
  // container packing, batched ACK airtime, turnarounds, ARQ.
  VhfBand band;
  Pair p(40.0, 33);
  CHECK(p.connect());
  if (p.a.state() != FsmState::Connected) return;
  CHECK(p.a.pinMode(14, p.now));
  for (int i = 0; i < 40 && p.a.sessionMode() != 14; ++i) p.step();
  CHECK_EQ(p.a.sessionMode(), 14);

  const size_t kBytes = 50000;
  auto blob = noiseBlob(kBytes, 42);
  const double t0 = p.now;
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  int steps = 0;
  for (; steps < 2000 && got.size() < blob.size(); ++steps) {
    p.step();
    auto r = p.b.takeReceivedData();
    got.insert(got.end(), r.begin(), r.end());
  }
  const double dt = p.now - t0;
  const double kbps = double(got.size()) * 8.0 / dt / 1000.0;
  std::printf("  vhf M14 sustained: %zu bytes in %.1f s = %.1f kbps\n",
              got.size(), dt, kbps);
  CHECK(got == blob);
  CHECK(kbps >= 25.0);
}

TEST_CASE(vhf_file_transfer) {
  VhfBand band;
  Pair p(35.0, 51);
  CHECK(p.connect());
  if (p.a.state() != FsmState::Connected) return;
  CHECK(p.a.pinMode(13, p.now));
  auto data = noiseBlob(8000, 77);
  CHECK(p.a.sendFile("vhf.bin", data, 12345));
  std::optional<ReceivedFile> rf;
  for (int i = 0; i < 600 && !rf; ++i) {
    p.step();
    rf = p.b.takeReceivedFile();
  }
  CHECK(rf.has_value());
  if (rf) {
    CHECK(rf->data == data);
    CHECK(rf->crcOk);
  }
}

// FM radio sweep 2026-10-17: the chirp detector's 4-sample correlation
// grid was coarser than VHF's ~1.8-sample correlation peak, so a frame
// whose start fell between grid points was never detected (3-5 of 8
// audio-sample phases failed in every mode). Every sample phase must
// decode, at baseband and through the 48 kHz converters.
TEST_CASE(vhf_detects_at_every_sample_phase) {
  VhfBand band;
  for (int mode : {4, 8, 13, 14}) {
    std::vector<uint8_t> payload(mode <= 7 ? 60 : 300);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 7 + mode);
    TxFrameParams tp;
    tp.family = Family::Wide;
    tp.mode = mode;
    tp.sig = mode <= 7 ? ChirpSig::Robust : ChirpSig::Fast;
    tp.cpMode = activeProfile().cpDefault;
    tp.payload = payload;
    auto bb = buildFrameBaseband(tp);
    std::vector<cfloat> padded(2000, cfloat(0, 0));
    padded.insert(padded.end(), bb.begin(), bb.end());
    padded.insert(padded.end(), 2500, cfloat(0, 0));
    Upconverter up;
    std::vector<float> audio;
    up.process(padded, audio);
    int ok = 0;
    for (int k = 0; k < 8; ++k) {  // audio samples = quarter baseband samples
      std::vector<float> d(size_t(k), 0.0f);
      d.insert(d.end(), audio.begin(), audio.end());
      Downconverter dn;
      std::vector<cfloat> r;
      dn.process(d, r);
      auto res = receiveFrame(Family::Wide, r);
      if (res && res->payloadOk && res->payload == payload) ++ok;
    }
    std::printf("  vhf M%d: %d/8 sample phases decode\n", mode, ok);
    CHECK_EQ(ok, 8);
  }
}

// The whole link through the FM radio: modulator, 15 Hz mobile flutter,
// AWGN in the IF, limiter-discriminator (threshold clicks), data-port
// filters. Connect, AMC, and a byte-exact bulk transfer.
TEST_CASE(vhf_link_through_fm_radio_with_flutter) {
  VhfBand band;
  Pair p(99.0, 61);
  p.fm = true;
  p.fmp = fmDataPort();
  p.fmp.cnrDb = 30.0;
  p.fmp.flutterHz = 15.0;
  CHECK(p.connect());
  if (p.a.state() != FsmState::Connected) return;
  auto blob = noiseBlob(20000, 62);
  p.a.sendData(blob);
  std::vector<uint8_t> got;
  for (int i = 0; i < 3000 && got.size() < blob.size(); ++i) {
    p.step();
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
  }
  std::printf("  fm link: %zu/%zu bytes, mode M%d, t=%.0f s\n", got.size(),
              blob.size(), p.a.sessionMode(), p.now);
  CHECK(got == blob);
}

SKYTEST_MAIN
