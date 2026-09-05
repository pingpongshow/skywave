// Channel-character classifier (link/chanclass.hpp).
//
// Thresholds here are pinned against the calibration measured 2026-08-17
// at M10 WIDE (see AmcConfig::pilotStabilityMax for the table). If the
// tracker's residual scale ever changes, these fail first.

#include "link/amc.hpp"
#include "link/chanclass.hpp"
#include "skytest.hpp"

#include <cstdio>

using namespace skw;

namespace {

// Measured means: AWGN 0.003-0.025, Good 0.010-0.057, Moderate
// 0.38-0.55, Poor 1.77-1.83.
void feed(ChannelClassifier& c, int n, double snr, double spreadMs,
          double pilot, bool ok = true) {
  for (int i = 0; i < n; ++i) c.onFrame(snr, spreadMs, pilot, ok);
}

}  // namespace

TEST_CASE(starts_unknown_until_enough_evidence) {
  ChannelClassifier c;
  CHECK(c.verdict() == ChannelClass::Unknown);
  feed(c, 4, 20.0, 0.05, 0.003);
  // Below minSamples: still no verdict, so callers keep fading floors.
  CHECK(c.verdict() == ChannelClass::Unknown);
}

TEST_CASE(awgn_like_channel_reads_static) {
  ChannelClassifier c;
  feed(c, 30, 20.0, 0.05, 0.003);
  CHECK(c.verdict() == ChannelClass::Static);
}

TEST_CASE(ccir_good_does_not_read_static) {
  // Good fades: pilot residual an order of magnitude above AWGN, and a
  // real delay spread. Calling this Static would promote against
  // optimistic floors and cost the session.
  ChannelClassifier c;
  for (int i = 0; i < 30; ++i) {
    c.onFrame(18.0 + (i % 5) * 1.2, 0.6, 0.045, true);
  }
  CHECK(c.verdict() != ChannelClass::Static);
  CHECK(c.verdict() == ChannelClass::Fading);
}

TEST_CASE(ccir_poor_like_channel_reads_severe) {
  // Poor: 2 ms spread and pilot residual ~1.8 - two severe signals.
  ChannelClassifier c;
  feed(c, 30, 12.0, 2.0, 1.8);
  CHECK(c.verdict() == ChannelClass::Severe);
}

TEST_CASE(high_fer_at_good_snr_reads_severe) {
  // The signature of a channel that will not close no matter the SNR:
  // measured on CCIR Poor where WIDE never beat 28% FER at any mode.
  ChannelClassifier c;
  for (int i = 0; i < 40; ++i) {
    const bool ok = (i % 5) >= 2;  // 40% failures
    c.onFrame(22.0, 1.8, 0.9, ok);
  }
  CHECK(c.verdict() == ChannelClass::Severe);
}

TEST_CASE(verdict_needs_confirmation_runs) {
  // A single divergent sample must not flip the floor table: switching
  // mid-session is disruptive and flapping is worse than being wrong.
  ChannelClassifier c;
  feed(c, 30, 20.0, 0.05, 0.003);
  CHECK(c.verdict() == ChannelClass::Static);
  c.onFrame(2.0, 2.5, 2.0, false);   // one bad frame
  CHECK(c.verdict() == ChannelClass::Static);
}

TEST_CASE(reset_clears_the_verdict) {
  ChannelClassifier c;
  feed(c, 30, 20.0, 0.05, 0.003);
  CHECK(c.verdict() == ChannelClass::Static);
  c.reset();
  CHECK(c.verdict() == ChannelClass::Unknown);
  CHECK_EQ(c.samples(), 0);
}

// --- the AMC's consumption of the verdict ---------------------------------

// The AMC consumes the verdict through its PUBLIC promote path: a
// SEVERE channel must never climb past the robust tier no matter how
// good the SNR looks, and a STATIC one must be free to climb on the
// AWGN floors.
TEST_CASE(severe_verdict_caps_the_ladder_at_the_robust_tier) {
  AmcConfig cfg;
  cfg.family = Family::Mid;
  Amc amc(cfg, 4, 0.0);
  amc.setChannelClass(ChannelClass::Severe);
  CHECK(amc.channelClass() == ChannelClass::Severe);

  // Feed a channel that looks excellent by SNR alone, then let the AMC
  // climb for a long time. Measured on CCIR Poor: MID closes only
  // M4-M6, so the ladder must stop at severeMaxMode regardless.
  double now = 0.0;
  for (int i = 0; i < 4000; ++i) {
    amc.onLocalSnr(40.0);
    amc.onRemoteSnr(40.0, now);
    amc.onFrameResult(false, true);
    amc.onQualityIndicators(0, 0.001);
    auto d = amc.evaluate(now);
    if (d.kind != AmcDecision::Kind::None) {
      amc.onModeApplied(d.targetMode, now);
    }
    now += 1.0;
  }
  CHECK(amc.mode() <= cfg.severeMaxMode);
}

TEST_CASE(static_verdict_climbs_higher_than_fading_on_the_same_snr) {
  // The AWGN reference floors sit below the measured fading floors, so
  // the same SNR must reach a HIGHER rung once the channel reads Static.
  auto climb = [](ChannelClass cls) {
    AmcConfig cfg;
    cfg.family = Family::Wide;
    Amc amc(cfg, 4, 0.0);
    amc.setChannelClass(cls);
    double now = 0.0;
    for (int i = 0; i < 6000; ++i) {
      amc.onLocalSnr(14.0);
      amc.onRemoteSnr(14.0, now);
      amc.onFrameResult(false, true);
      amc.onQualityIndicators(0, 0.001);
      amc.onProbeResult(true, now);
      auto d = amc.evaluate(now);
      if (d.kind != AmcDecision::Kind::None) {
      amc.onModeApplied(d.targetMode, now);
    }
      now += 1.0;
    }
    return amc.mode();
  };
  const uint8_t fading = climb(ChannelClass::Fading);
  const uint8_t stat = climb(ChannelClass::Static);
  std::printf("same 14 dB: fading reached M%d, static reached M%d\n",
              int(fading), int(stat));
  CHECK(stat > fading);
}

SKYTEST_MAIN
