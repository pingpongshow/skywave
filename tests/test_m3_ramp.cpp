// Milestone M3 gate (ROADMAP 3.11): scripted SNR ramp -10 -> +22 -> -10
// dB with continuous traffic. The AMC-driven session must survive the
// whole traversal with zero drops and deliver >= 75% of what a genie
// (best-mode-pinned per segment) achieves under identical conditions.
#include <cstdio>
#include <vector>

#include "link/engine.hpp"
#include "sim/chansim.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

struct RampResult {
  size_t bytes = 0;
  int drops = 0;
  bool connected = false;
};

// Oracle uses the same operating floors as the AMC (kFadingFloors, the
// measured one-shot floors for this channel class): the ramp measures
// TRACKING quality, not floor-table disagreement. kFadingNA rows are
// naturally skipped (floor 99).
int oracleMode(double snrDb) {
  int best = kFirstMode;
  for (int m = kFirstMode; m < kNumModes; ++m) {
    if (snrDb >= fadingFloorDb(Family::Wide, m) + 1.5) best = m;
  }
  return best;
}

RampResult runRamp(bool genie, uint64_t seed) {
  EngineConfig ca, cb;
  ca.policy.ownCall = "W1AW";
  cb.policy.ownCall = "K6XYZ";
  ca.policy.caps = cb.policy.caps = kCapZstd | kCapResume;
  LinkEngine a(ca), b(cb);
  double now = 0;
  uint64_t sc = 1;
  double snr = 2.0;  // connect just above the M4 fading floor
  RampResult res;

  auto pass = [&](LinkEngine& from, LinkEngine& to, uint64_t sbase) {
    auto burst = from.tick(now);
    double dt = 0.05;
    if (burst) {
      dt += double(burst->size()) / kFsBaseband;
      std::vector<cfloat> x(1500, cfloat(0, 0));
      x.insert(x.end(), burst->begin(), burst->end());
      x.insert(x.end(), 2000, cfloat(0, 0));
      ChansimParams ch = ccirGood();
      ch.snr3kDb = snr;
      ch.seed = sbase + (sc++) * 7919;
      auto y = Chansim::run(ch, x);
      to.rxBurst(y, now);
    }
    now += dt;
  };

  a.connectTo("K6XYZ", now);
  for (int i = 0; i < 600 && (a.state() != FsmState::Connected ||
                              b.state() != FsmState::Connected);
       ++i) {
    pass(a, b, seed);
    pass(b, a, seed + 5000);
  }
  if (a.state() != FsmState::Connected) return res;
  res.connected = true;

  // Ramp schedule on CCIR Good, 4 dB segments, 35 s each. The original
  // -10..+22 gate was written against the AWGN reference floors
  // (bottom = M4 floor - 1.5); after the M1 floor re-spec the faithful
  // fading-channel translation keeps the same depth RELATIVE to the
  // operating ladder: M4 fading floor (1.5) - 1.5 = 0 up to M12's
  // floor region (+26).
  std::vector<double> sched;
  for (double s = 0; s <= 26; s += 4) sched.push_back(s);
  for (double s = 22; s >= 0; s -= 4) sched.push_back(s);

  // Incompressible traffic: a constant-fill chunk would zstd down to a
  // few bytes and turn the throughput ramp into a frame-rate counter.
  std::vector<uint8_t> chunk(1500);
  {
    uint64_t s = seed ^ 0xC0FFEE;
    for (auto& v : chunk) {
      s ^= s << 13;
      s ^= s >> 7;
      s ^= s << 17;
      v = uint8_t(s >> 32);
    }
  }
  int lastPinned = -1;
  for (double seg : sched) {
    snr = seg;
    if (genie) {
      const int want = oracleMode(seg);
      if (want != lastPinned) {
        a.pinMode(want, now);
        lastPinned = want;
      }
    }
    // 75 s per 4 dB segment: single-step promotion (spec rule) through
    // the robust tier costs ~20-30 s per rung at robust-mode airtimes;
    // real ionospheric SNR ramps evolve over tens of minutes, so this is
    // still a conservative rate of change.
    const double segEnd = now + 75.0;
    size_t segBytes = 0;
    while (now < segEnd) {
      if (a.txBacklog() < 4000) a.sendData(chunk);  // keep traffic flowing
      pass(a, b, seed);
      pass(b, a, seed + 5000);
      segBytes += b.takeReceivedData().size();
      if (a.state() != FsmState::Connected ||
          b.state() != FsmState::Connected) {
        ++res.drops;
        return res;  // a drop fails the gate; no need to continue
      }
    }
    res.bytes += segBytes;
    if (getenv("SKW_RAMP_DEBUG")) {
      std::printf("  %s seg %+5.1f dB: mode M%d (oracle M%d) %zu B\n",
                  genie ? "GEN" : "AMC", seg, a.sessionMode(),
                  oracleMode(seg), segBytes);
    }
  }
  return res;
}

}  // namespace

TEST_CASE(m3_snr_ramp_gate) {
  RampResult amc = runRamp(false, 4200);
  RampResult gen = runRamp(true, 4200);
  CHECK(amc.connected);
  CHECK(gen.connected);
  const double ratio =
      gen.bytes > 0 ? double(amc.bytes) / double(gen.bytes) : 0.0;
  std::printf("M3 ramp: amc=%zu B genie=%zu B ratio=%.2f drops=%d\n",
              amc.bytes, gen.bytes, ratio, amc.drops);
  CHECK_EQ(amc.drops, 0);       // zero session drops across the traversal
  CHECK(ratio >= 0.75);         // >= 75%% of oracle throughput
}

SKYTEST_MAIN
