// Broadcast + CW ID (ROADMAP 4.5): CQ/BEACON/BCAST_DATA fragmenting and
// monitor-side reassembly; Morse renderer timing; CW ID emission after
// disconnect and on the 10-minute in-session cadence.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/morse.hpp"
#include "link/engine.hpp"
#include "link/monitor.hpp"
#include "sim/chansim.hpp"
#include "skytest.hpp"

using namespace skw;

TEST_CASE(morse_timing_and_tone) {
  // PARIS at 20 WPM: 46 units rendered (the trailing word gap is the
  // 47..50th and is not emitted), dit = 60 ms.
  const int wpm = 20;
  const double dit = 1.2 / wpm;
  CHECK_NEAR(morseDurationSec("PARIS", wpm), 46 * dit, 1e-9);
  auto s = renderMorseBaseband("PARIS", wpm);
  CHECK_EQ(int(s.size()), int(46 * dit * kFsBaseband));
  // Keyed-on sample count: PARIS has 22 on-units.
  int on = 0;
  for (auto& v : s) {
    if (std::abs(v) > 0.25) ++on;
  }
  const double onExpected = 22 * dit * kFsBaseband;
  CHECK(std::abs(on - onExpected) < 0.05 * onExpected);
  // Tone frequency: average phase advance over the loudest run should
  // land at audioTone - center = 800 - 1500 = -700 Hz.
  std::complex<double> acc = 0;
  for (size_t i = 1; i < s.size(); ++i) {
    if (std::abs(s[i]) > 0.4f && std::abs(s[i - 1]) > 0.4f) {
      acc += std::complex<double>(s[i]) * std::conj(std::complex<double>(s[i - 1]));
    }
  }
  const double fHz = std::arg(acc) * kFsBaseband / (2 * M_PI);
  std::printf("cw tone: %.1f Hz baseband\n", fHz);
  CHECK(std::abs(fHz - (-700.0)) < 5.0);
}

TEST_CASE(broadcast_monitor_reassembly) {
  // Idle engine transmits CQ, BEACON, and a 3-fragment broadcast; a
  // passive monitor decodes everything and reassembles the chain.
  EngineConfig ec;
  ec.policy.ownCall = "W1AW";
  LinkEngine a(ec);
  Monitor mon(Family::Wide);

  a.sendCq();
  a.sendBeacon(-12.0);
  std::vector<uint8_t> payload(700);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = uint8_t(i * 29 + 1);
  }
  CHECK(a.sendBroadcast(payload));
  CHECK(!a.sendBroadcast({}));  // empty rejected
  std::vector<uint8_t> tooBig(16 * kBcastMaxPayload + 1);
  CHECK(!a.sendBroadcast(tooBig));

  double now = 0.0;
  int monLines = 0, sawCq = 0, sawBeacon = 0, sawFrag = 0;
  uint64_t seed = 1;
  for (int i = 0; i < 40; ++i) {
    auto burst = a.tick(now);
    now += 1.0;
    if (!burst) break;
    std::vector<cfloat> x(1500, cfloat(0, 0));
    x.insert(x.end(), burst->begin(), burst->end());
    x.insert(x.end(), 2000, cfloat(0, 0));
    ChansimParams cp;
    cp.fading = false;
    cp.snr3kDb = 20.0;
    cp.seed = 100 + seed++;
    auto y = Chansim::run(cp, x);
    if (auto ev = mon.rxBurst(y)) {
      ++monLines;
      if (ev->monLine.find(":20:") != std::string::npos) ++sawCq;
      if (ev->monLine.find(":19:") != std::string::npos) ++sawBeacon;
      if (ev->monLine.find(":21:") != std::string::npos) ++sawFrag;
      if (ev->monLine.find("W1AW") != std::string::npos) {
        // src attribution flows from the CQ/BEACON callsign cache
      }
    }
  }
  std::printf("bcast monitor: %d lines cq=%d beacon=%d frags=%d\n",
              monLines, sawCq, sawBeacon, sawFrag);
  CHECK_EQ(sawCq, 1);
  CHECK_EQ(sawBeacon, 1);
  CHECK_EQ(sawFrag, 3);  // 700 B -> 256+256+188
  auto got = mon.takeBroadcasts();
  CHECK_EQ(int(got.size()), 1);
  CHECK(got[0] == payload);
}

namespace {

struct CwPair {
  LinkEngine a, b;
  double now = 0.0;
  uint64_t seedCtr = 1;
  size_t cwLen;

  CwPair()
      : a(mk("W1AW")), b(mk("K6XYZ")),
        cwLen(renderMorseBaseband("W1AW", 20).size()) {}

  static EngineConfig mk(const char* call) {
    EngineConfig c;
    c.policy.ownCall = call;
    c.policy.caps = kCapZstd | kCapResume;
    c.cwId = true;
    return c;
  }

  // Returns true if A emitted its CW ID this step (identified by the
  // exact rendered length - CW is not a PHY frame).
  bool step(int* cwCount = nullptr) {
    auto burstA = a.tick(now);
    auto burstB = b.tick(now);
    double dt = 0.05;
    bool cw = false;
    if (burstA) {
      dt += double(burstA->size()) / kFsBaseband;
      if (burstA->size() == cwLen) {
        cw = true;
        if (cwCount) ++*cwCount;
      } else {
        deliver(*burstA, b);
      }
    }
    if (burstB && burstB->size() != renderMorseBaseband("K6XYZ", 20).size()) {
      dt += double(burstB->size()) / kFsBaseband;
      deliver(*burstB, a);
    }
    now += dt;
    return cw;
  }

  void deliver(std::vector<cfloat>& burst, LinkEngine& to) {
    std::vector<cfloat> x(1500, cfloat(0, 0));
    x.insert(x.end(), burst.begin(), burst.end());
    x.insert(x.end(), 2000, cfloat(0, 0));
    ChansimParams cp;
    cp.fading = false;
    cp.snr3kDb = 25.0;
    cp.seed = 40 + (seedCtr++) * 7919;
    auto y = Chansim::run(cp, x);
    to.rxBurst(y, now);
  }
};

}  // namespace

TEST_CASE(cw_id_after_disconnect_and_periodic) {
  CwPair p;
  p.a.connectTo("K6XYZ", p.now);
  for (int i = 0; i < 400 && (p.a.state() != FsmState::Connected ||
                              p.b.state() != FsmState::Connected);
       ++i) {
    p.step();
  }
  CHECK(p.a.state() == FsmState::Connected);

  // Sparse traffic holds the session for > 10 min: the periodic CW ID
  // must fire in an idle gap, exactly once per 600 s.
  int cwInSession = 0;
  const std::vector<uint8_t> ping(24, 0x33);
  const double t0 = p.now;
  for (int i = 0; i < 20000 && p.now - t0 < 650.0; ++i) {
    if (i % 300 == 0) p.a.sendData(ping);
    p.step(&cwInSession);
    if (p.a.state() != FsmState::Connected) break;
  }
  CHECK(p.a.state() == FsmState::Connected);
  CHECK_EQ(cwInSession, 1);

  // Orderly teardown: both sides identify afterwards.
  p.a.disconnect(p.now);
  int cwAfter = 0;
  for (int i = 0; i < 80; ++i) {
    p.step(&cwAfter);
  }
  CHECK(p.a.state() == FsmState::Idle);
  CHECK_EQ(cwAfter, 1);
  std::printf("cw id: %zu samples (%.2f s), in-session=%d after-disc=%d\n",
              p.cwLen, double(p.cwLen) / kFsBaseband, cwInSession, cwAfter);
}

SKYTEST_MAIN
