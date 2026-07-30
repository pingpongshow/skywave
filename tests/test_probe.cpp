// Probe-before-promote gate (ROADMAP 3.6): eligibility, probe counts,
// verdicts, cooldown, timeout.
#include "link/probe.hpp"
#include "skytest.hpp"

using namespace skw;

TEST_CASE(eligibility_and_counts) {
  ProbeGate g;
  CHECK(!g.required(10));
  CHECK(g.required(11));
  CHECK(g.required(13));
  auto r = g.begin(11, 0.0);
  CHECK(r && r->count == 1);
  ProbeGate g2;
  auto r2 = g2.begin(12, 0.0);
  CHECK(r2 && r2->count == 2);
  // A pending exchange blocks another begin.
  CHECK(!g2.begin(12, 1.0).has_value());
}

TEST_CASE(clean_path_clears_once) {
  ProbeGate g;
  auto r = g.begin(11, 0.0);
  CHECK(g.pending());
  // Wrong id ignored.
  CHECK(!g.onProbeAck(uint8_t(r->probeId + 1), kProbeClean));
  CHECK(g.onProbeAck(r->probeId, kProbeClean));
  CHECK(g.cleared(11));
  CHECK(!g.cleared(12));
  g.consumeClearance();
  CHECK(!g.cleared(11));
}

TEST_CASE(two_probe_requirement_m12) {
  ProbeGate g;
  auto r = g.begin(12, 0.0);
  CHECK(!g.onProbeAck(r->probeId, kProbeClean));  // 1 of 2
  CHECK(!g.cleared(12));
  CHECK(g.onProbeAck(r->probeId, kProbeClean));   // 2 of 2
  CHECK(g.cleared(12));
}

TEST_CASE(failed_and_marginal_start_cooldown) {
  for (uint8_t verdict : {kProbeFailed, kProbeMarginal}) {
    ProbeGate g;
    auto r = g.begin(11, 0.0);
    CHECK(!g.onProbeAck(r->probeId, verdict));
    CHECK(!g.cleared(11));
    CHECK(g.inCooldown());
    CHECK(!g.begin(11, 1.0).has_value());  // blocked during cooldown
    for (int i = 0; i < 10; ++i) g.onFrameObserved();
    CHECK(!g.inCooldown());
    CHECK(g.begin(11, 2.0).has_value());
  }
}

TEST_CASE(timeout_counts_as_failed) {
  ProbeGate g({11, 10, 20.0});
  auto r = g.begin(11, 100.0);
  g.tick(119.0);
  CHECK(g.pending());
  g.tick(120.5);
  CHECK(!g.pending());
  CHECK(g.inCooldown());
  // A late ack after timeout is ignored.
  CHECK(!g.onProbeAck(r->probeId, kProbeClean));
  CHECK(!g.cleared(11));
}

SKYTEST_MAIN
