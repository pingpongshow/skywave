// AMC controller (ROADMAP 3.1): table-driven rule coverage - promote/
// demote margins and hysteresis, dwell, consecutive-eval requirement,
// high-mode gates, fast demote, DEGRADED, pinning safety, effective-SNR
// staleness.
#include "link/amc.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

AmcConfig cfg() {
  AmcConfig c;
  c.family = Family::Wide;
  return c;
}

// Feed n clean frames so the retry window fills.
void cleanFrames(Amc& a, int n = 16) {
  for (int i = 0; i < n; ++i) a.onFrameResult(false, true);
}

double floorOf(int m) { return fadingFloorDb(Family::Wide, m); }

}  // namespace

TEST_CASE(effective_snr_rules) {
  Amc a(cfg(), 8, 0.0);
  a.onLocalSnr(20.0);
  // No remote report: local minus staleness penalty.
  CHECK_NEAR(a.effectiveSnrDb(0.0), 18.5, 1e-9);
  a.onRemoteSnr(14.0, 0.0);
  CHECK_NEAR(a.effectiveSnrDb(10.0), 14.0, 1e-9);   // min(local, remote)
  CHECK_NEAR(a.effectiveSnrDb(100.0), 18.5, 1e-9);  // remote stale again
  // Local EMA moves at alpha 0.25.
  a.onLocalSnr(28.0);
  CHECK_NEAR(a.effectiveSnrDb(100.0), 20.0 + 0.25 * 8.0 - 1.5, 1e-9);
}

TEST_CASE(promote_requires_margin_dwell_consecutive) {
  Amc a(cfg(), 8, 0.0);
  cleanFrames(a);
  // Abundant SNR for M9 (floor 15.0): eff = snr - 1.5 (no remote).
  a.onLocalSnr(25.0);
  // Dwell not met at t=3.
  CHECK(a.evaluate(3.0).kind == AmcDecision::Kind::None);
  // First eligible evaluation after dwell: streak 1 of 2 - still None.
  CHECK(a.evaluate(7.0).kind == AmcDecision::Kind::None);
  auto d = a.evaluate(8.0);
  CHECK(d.kind == AmcDecision::Kind::Promote);
  CHECK_EQ(int(d.targetMode), 9);  // single step only
  a.onModeApplied(9, 8.0);
  CHECK_EQ(int(a.mode()), 9);
  // SNR just below the next promote margin: no promotion.
  Amc b(cfg(), 8, 0.0);
  cleanFrames(b);
  // eff lands 0.1 dB under the promote margin
  b.onLocalSnr(floorOf(9) + cfg().promoteMarginDb - 0.1 + 1.5);
  b.evaluate(7.0);
  CHECK(b.evaluate(8.0).kind == AmcDecision::Kind::None);
}

TEST_CASE(demote_rules) {
  // SNR under floor + 0.5.
  {
    Amc a(cfg(), 9, 0.0);
    cleanFrames(a);
    // eff lands 0.1 dB under the demote margin
    a.onLocalSnr(floorOf(9) + cfg().demoteMarginDb - 0.1 + 1.5);
    auto d = a.evaluate(1.0);
    CHECK(d.kind == AmcDecision::Kind::Demote);
    CHECK_EQ(int(d.targetMode), 8);
  }
  // Retry ratio > 0.35.
  {
    Amc a(cfg(), 9, 0.0);
    a.onLocalSnr(30.0);
    for (int i = 0; i < 16; ++i) a.onFrameResult(i % 2 == 0, true);  // 0.5
    auto d = a.evaluate(1.0);
    // 0.5 crosses fast-demote too; accept either demotion kind but a
    // plain >0.35 <0.5 case must be a single-step Demote:
    CHECK(d.kind != AmcDecision::Kind::None);
    Amc b(cfg(), 9, 0.0);
    b.onLocalSnr(30.0);
    for (int i = 0; i < 16; ++i) b.onFrameResult(i % 5 < 2, true);  // 0.4
    auto db = b.evaluate(1.0);
    CHECK(db.kind == AmcDecision::Kind::Demote);
    CHECK_EQ(int(db.targetMode), 8);
  }
  // 3 consecutive decode failures.
  {
    Amc a(cfg(), 7, 0.0);
    cleanFrames(a);
    a.onLocalSnr(30.0);
    a.onFrameResult(false, false);
    a.onFrameResult(false, false);
    CHECK(a.evaluate(1.0).kind == AmcDecision::Kind::None);
    a.onFrameResult(false, false);
    auto d = a.evaluate(1.1);
    CHECK(d.kind == AmcDecision::Kind::Demote);
  }
}

TEST_CASE(fast_demote_and_degraded) {
  Amc a(cfg(), 10, 0.0);
  a.onLocalSnr(30.0);
  for (int i = 0; i < 16; ++i) a.onFrameResult(i % 3 != 0, true);  // ~0.66
  auto d = a.evaluate(0.5);  // dwell waived
  CHECK(d.kind == AmcDecision::Kind::FastDemote);
  CHECK_EQ(int(d.targetMode), 8);  // two steps
  // At the bottom rung: DEGRADED instead of demotion.
  Amc b(cfg(), 4, 0.0);
  b.onLocalSnr(-20.0);
  for (int i = 0; i < 16; ++i) b.onFrameResult(true, false);
  auto db = b.evaluate(1.0);
  CHECK(db.kind == AmcDecision::Kind::None);
  CHECK(b.degraded());
}

TEST_CASE(high_mode_gates) {
  // M11 blocked by retry ratio > 0.10 even with SNR.
  {
    Amc a(cfg(), 10, 0.0);
    a.onLocalSnr(40.0);
    for (int i = 0; i < 16; ++i) a.onFrameResult(i % 8 == 0, true);  // 0.125
    a.onQualityIndicators(0, 0.0);
    a.evaluate(7.0);
    CHECK(a.evaluate(8.0).kind == AmcDecision::Kind::None);
  }
  // M11 blocked by pilot instability; allowed when stable.
  {
    Amc a(cfg(), 10, 0.0);
    cleanFrames(a);
    a.onLocalSnr(40.0);
    a.onQualityIndicators(0, 0.5);
    a.evaluate(7.0);
    CHECK(a.evaluate(8.0).kind == AmcDecision::Kind::None);
    a.onQualityIndicators(0, 0.05);
    a.evaluate(9.0);
    auto d = a.evaluate(10.0);
    CHECK(d.kind == AmcDecision::Kind::Promote);
    CHECK_EQ(int(d.targetMode), 11);
  }
  // M12 additionally needs low selectivity.
  {
    Amc a(cfg(), 11, 0.0);
    cleanFrames(a);
    a.onLocalSnr(40.0);
    a.onQualityIndicators(200, 0.05);
    a.evaluate(7.0);
    CHECK(a.evaluate(8.0).kind == AmcDecision::Kind::None);
    a.onQualityIndicators(10, 0.05);
    a.evaluate(9.0);
    CHECK(a.evaluate(10.0).kind == AmcDecision::Kind::Promote);
  }
  // M13 (WIDE: kFadingNA) needs a clean probe + AWGN-floor margin.
  {
    Amc a(cfg(), 12, 0.0);
    cleanFrames(a);
    a.onLocalSnr(40.0);
    a.onQualityIndicators(10, 0.05);
    a.evaluate(7.0);
    CHECK(a.evaluate(8.0).kind == AmcDecision::Kind::None);  // no probe
    a.onProbeResult(true, 8.5);
    a.evaluate(9.0);
    auto d = a.evaluate(10.0);
    CHECK(d.kind == AmcDecision::Kind::Promote);
    CHECK_EQ(int(d.targetMode), 13);
    // Applying the mode consumes the probe clearance.
    a.onModeApplied(13, 10.0);
    Amc fresh = a;
    (void)fresh;
  }
}

TEST_CASE(pinning_with_fast_demote_safety) {
  Amc a(cfg(), 6, 0.0);
  a.pin(10, 0.0);
  cleanFrames(a);
  a.onLocalSnr(40.0);
  a.onQualityIndicators(0, 0.0);
  // Pinned: no promotion, no ordinary demotion.
  a.evaluate(7.0);
  CHECK(a.evaluate(8.0).kind == AmcDecision::Kind::None);
  a.onLocalSnr(-10.0);
  CHECK(a.evaluate(9.0).kind == AmcDecision::Kind::None);
  // But fast demote still fires.
  for (int i = 0; i < 16; ++i) a.onFrameResult(true, false);
  auto d = a.evaluate(9.5);
  CHECK(d.kind == AmcDecision::Kind::FastDemote);
  CHECK_EQ(int(d.targetMode), 8);
  // Unpin restores normal control.
  a.unpin();
  CHECK(!a.pinned());
}

TEST_CASE(failure_memory_holds_promotion_and_doubles) {
  // A retry-ratio demotion at abundant SNR remembers the failed rung:
  // SNR looking fine is not evidence that the rung works (the fast tier
  // fails 30-60% on CCIR Moderate at ANY SNR).
  Amc a(cfg(), 9, 0.0);
  a.onLocalSnr(30.0);  // eff 28.5, far above every floor here
  for (int i = 0; i < 16; ++i) a.onFrameResult(i % 5 < 2, true);  // 0.4
  auto d = a.evaluate(1.0);
  CHECK(d.kind == AmcDecision::Kind::Demote);
  CHECK_EQ(int(d.targetMode), 8);
  a.onModeApplied(8, 1.0);
  CHECK_EQ(int(a.failedMode()), 9);
  CHECK_NEAR(a.failHoldSec(1.0), 30.0, 1e-9);  // first failure: 30 s
  cleanFrames(a);
  // Ordinary post-demote damping (15 s) has expired at t=17, but the
  // box holds until t=31.
  CHECK(a.evaluate(17.0).kind == AmcDecision::Kind::None);
  CHECK(a.evaluate(18.0).kind == AmcDecision::Kind::None);
  CHECK(a.evaluate(30.0).kind == AmcDecision::Kind::None);
  CHECK(a.evaluate(32.0).kind == AmcDecision::Kind::None);  // streak 1
  auto p = a.evaluate(33.0);
  CHECK(p.kind == AmcDecision::Kind::Promote);
  CHECK_EQ(int(p.targetMode), 9);
  a.onModeApplied(9, 33.0);
  // Fail AGAIN at the same rung, same SNR: the hold doubles to 60 s.
  for (int i = 0; i < 16; ++i) a.onFrameResult(i % 5 < 2, true);
  auto d2 = a.evaluate(34.0);
  CHECK(d2.kind == AmcDecision::Kind::Demote);
  a.onModeApplied(8, 34.0);
  CHECK_EQ(int(a.failedMode()), 9);
  CHECK_NEAR(a.failHoldSec(34.0), 60.0, 1e-9);
  cleanFrames(a);
  CHECK(a.evaluate(50.0).kind == AmcDecision::Kind::None);
  CHECK(a.evaluate(93.0).kind == AmcDecision::Kind::None);
  CHECK(a.evaluate(95.0).kind == AmcDecision::Kind::None);  // streak 1
  CHECK(a.evaluate(96.0).kind == AmcDecision::Kind::Promote);
  // SNR recovery overrides the box: +6 dB over the SNR at failure is
  // new evidence, so a third failure's 120 s hold does not apply.
  a.onModeApplied(9, 96.0);
  for (int i = 0; i < 16; ++i) a.onFrameResult(i % 5 < 2, true);
  CHECK(a.evaluate(97.0).kind == AmcDecision::Kind::Demote);
  a.onModeApplied(8, 97.0);
  CHECK_NEAR(a.failHoldSec(97.0), 120.0, 1e-9);
  cleanFrames(a);
  a.onLocalSnr(45.0);
  a.onLocalSnr(45.0);
  a.onLocalSnr(45.0);  // EMA: 30 -> 33.75 -> 36.6 -> 38.7, eff 37.2 > 34.5
  CHECK(a.evaluate(113.0).kind == AmcDecision::Kind::None);  // streak 1
  CHECK(a.evaluate(114.0).kind == AmcDecision::Kind::Promote);
}

TEST_CASE(moderate_class_caps_ladder_at_m8) {
  // MODERATE with the cap enabled (off by default): no promotion above
  // M8 however rich the SNR (eligibility only - a ladder already above
  // the cap is left to the failure rules, never force-walked down); the
  // cap lifts with the verdict.
  AmcConfig capped = cfg();
  capped.moderateMaxMode = 8;
  Amc a(capped, 8, 0.0);
  cleanFrames(a);
  a.onLocalSnr(35.0);
  a.setChannelClass(ChannelClass::Moderate);
  a.evaluate(7.0);
  CHECK(a.evaluate(8.0).kind == AmcDecision::Kind::None);  // M9 ineligible
  Amc b(capped, 10, 0.0);
  cleanFrames(b);
  b.onLocalSnr(35.0);
  b.setChannelClass(ChannelClass::Moderate);
  CHECK(b.evaluate(7.0).kind == AmcDecision::Kind::None);   // no walk-down
  CHECK(b.evaluate(14.0).kind == AmcDecision::Kind::None);  // no promote
  a.setChannelClass(ChannelClass::Fading);  // Good again: cap lifts
  a.evaluate(20.0);
  auto p = a.evaluate(21.0);
  CHECK(p.kind == AmcDecision::Kind::Promote);
  CHECK_EQ(int(p.targetMode), 9);
}

TEST_CASE(failure_memory_ignores_snr_explained_demotions) {
  // A floor-break demotion is explained by the SNR; the SNR coming back
  // is exactly what should let the ladder climb again - no box.
  Amc a(cfg(), 9, 0.0);
  cleanFrames(a);
  a.onLocalSnr(floorOf(9) + cfg().demoteMarginDb - 0.1 + 1.5);
  auto d = a.evaluate(1.0);
  CHECK(d.kind == AmcDecision::Kind::Demote);
  a.onModeApplied(8, 1.0);
  CHECK_EQ(int(a.failedMode()), 255);
  CHECK_NEAR(a.failHoldSec(1.0), 0.0, 1e-9);
}

TEST_CASE(max_mode_cap) {
  AmcConfig c = cfg();
  c.maxMode = 8;
  Amc a(c, 8, 0.0);
  cleanFrames(a);
  a.onLocalSnr(45.0);
  a.evaluate(7.0);
  CHECK(a.evaluate(8.0).kind == AmcDecision::Kind::None);
}

SKYTEST_MAIN
