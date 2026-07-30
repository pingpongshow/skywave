// VHF-FM band profile (ROADMAP Phase 7): profile activation, connect /
// AMC ramp / transfer / file scenarios under band=vhf-fm, and the M7
// gate - >= 25 kbps net sustained over the flat-channel sim.
#include <cstdio>
#include <vector>

#include "link/engine.hpp"
#include "sim/chansim.hpp"
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

SKYTEST_MAIN
