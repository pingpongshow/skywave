// Advisor layer (ROADMAP 6.3): env config, deterministic advice,
// bounded single-step influence (the assist property gates), and
// off-vs-shadow equivalence. Note the suite-wide non-degradation
// evidence: SKW_ADVISOR defaults to `shadow`, so every engine-level
// test in the suite (including the Phase-3 ramp) already runs with the
// Advisor observing.
#include <cstdlib>
#include <vector>

#include "link/advisor.hpp"
#include "link/amc.hpp"
#include "link/engine.hpp"
#include "sim/chansim.hpp"
#include "core/constants.hpp"
#include <cstdio>
#include "skytest.hpp"

using namespace skw;

namespace {

AdvisorConfig assistCfg() {
  AdvisorConfig c;
  c.mode = AdvisorMode::Assist;
  return c;
}

AmcConfig amcCfg() {
  AmcConfig c;
  c.family = Family::Wide;
  return c;
}

}  // namespace

TEST_CASE(advisor_env_config) {
  setenv("SKW_ADVISOR", "off", 1);
  setenv("SKW_ADVISOR_AMC_CONF", "0.8", 1);
  auto c = AdvisorConfig::fromEnv();
  CHECK(c.mode == AdvisorMode::Off);
  CHECK_NEAR(c.amcConf, 0.8, 1e-9);
  setenv("SKW_ADVISOR", "assist", 1);
  CHECK(AdvisorConfig::fromEnv().mode == AdvisorMode::Assist);
  setenv("SKW_ADVISOR", "banana", 1);  // unknown -> shadow
  CHECK(AdvisorConfig::fromEnv().mode == AdvisorMode::Shadow);
  unsetenv("SKW_ADVISOR");
  unsetenv("SKW_ADVISOR_AMC_CONF");
  CHECK(AdvisorConfig::fromEnv().mode == AdvisorMode::Shadow);  // default
}

TEST_CASE(advisor_amc_trend_and_veto) {
  Advisor a(assistCfg());
  // Rising SNR ~3 dB/min over 8 samples: promote bias, confident.
  for (int i = 0; i < 8; ++i) a.onSnr(2.0 + 0.5 * i, i * 10.0);
  auto up = a.adviseAmc(0.0);
  CHECK_EQ(up.stepBias, 1);
  CHECK(up.confidence >= 0.6);
  // Retry evidence vetoes promote bias regardless of the trend.
  auto veto = a.adviseAmc(0.3);
  CHECK_EQ(veto.stepBias, 0);

  Advisor b(assistCfg());
  for (int i = 0; i < 8; ++i) b.onSnr(10.0 - 0.5 * i, i * 10.0);
  auto down = b.adviseAmc(0.0);
  CHECK_EQ(down.stepBias, -1);
  CHECK(down.confidence >= 0.6);

  // Flat/noisy: no advice.
  Advisor f(assistCfg());
  const double noisy[] = {8.0, 8.3, 7.9, 8.1, 8.0, 8.2, 7.8, 8.1};
  for (int i = 0; i < 8; ++i) f.onSnr(noisy[i], i * 10.0);
  CHECK_EQ(f.adviseAmc(0.0).stepBias, 0);
}

TEST_CASE(advisor_determinism) {
  auto run = [] {
    Advisor a(assistCfg());
    for (int i = 0; i < 6; ++i) {
      a.onSnr(3.0 + 0.7 * i, i * 12.0);
      a.onCfo(1.0 + 0.05 * i, i * 12.0);
    }
    a.onMaskChange(10.0);
    a.onMaskChange(30.0);
    a.onMaskChange(50.0);
    auto m = a.adviseMask(60.0);
    auto s = a.adviseAmc(0.0);
    auto c = a.adviseCfo(80.0);
    return std::make_tuple(s.stepBias, s.confidence, c.driftHzPerMin,
                           c.predictedHz, c.confidence, m.thresholdDeltaDb,
                           m.confidence);
  };
  CHECK(run() == run());
}

TEST_CASE(advisor_cfo_drift_prediction) {
  Advisor a(assistCfg());
  // 0.9 Hz/min linear drift, clean.
  for (int i = 0; i < 10; ++i) a.onCfo(2.0 + 0.9 * (i * 15.0) / 60.0,
                                       i * 15.0);
  auto adv = a.adviseCfo(150.0);
  CHECK_NEAR(adv.driftHzPerMin, 0.9, 0.1);
  CHECK(adv.confidence > 0.9);
  // Prediction extrapolates from the last sample, bounded to 2 min.
  CHECK_NEAR(adv.predictedHz, 2.0 + 0.9 * 135.0 / 60.0 + 0.9 * 0.25, 0.15);
  // Drift clamp: absurd slope saturates at +-6 Hz/min.
  Advisor b(assistCfg());
  for (int i = 0; i < 6; ++i) b.onCfo(i * 20.0, i * 10.0);
  CHECK(std::abs(b.adviseCfo(60.0).driftHzPerMin) <= 6.0 + 1e-9);
}

TEST_CASE(advisor_mask_churn_bounded) {
  Advisor a(assistCfg());
  for (int i = 0; i < 12; ++i) a.onMaskChange(i * 5.0);
  auto adv = a.adviseMask(60.0);
  CHECK(adv.thresholdDeltaDb > 0.0);
  CHECK(adv.thresholdDeltaDb <= 1.5 + 1e-9);
  CHECK(adv.confidence >= 0.55);
  // Quiet history: no advice.
  Advisor q(assistCfg());
  CHECK_NEAR(q.adviseMask(100.0).thresholdDeltaDb, 0.0, 1e-9);
}

TEST_CASE(advisor_assist_gating) {
  AdvisorConfig cfg = assistCfg();
  cfg.amcConf = 1.01;  // floor above any producible confidence
  Advisor a(cfg);
  for (int i = 0; i < 8; ++i) a.onSnr(2.0 + 0.5 * i, i * 10.0);
  auto adv = a.adviseAmc(0.0);
  CHECK_EQ(adv.stepBias, 1);
  CHECK(!a.assistAmc(adv));  // confident, but below the raised floor
  // Shadow mode never applies, whatever the confidence.
  AdvisorConfig sh;
  sh.mode = AdvisorMode::Shadow;
  Advisor s(sh);
  for (int i = 0; i < 8; ++i) s.onSnr(2.0 + 0.5 * i, i * 10.0);
  CHECK(!s.assistAmc(s.adviseAmc(0.0)));
}

TEST_CASE(advisor_amc_bias_single_step_property) {
  // The assist gate property (ROADMAP 6.3): with a promote bias applied
  // before EVERY evaluation, AMC still never moves more than one mode
  // per decision and still honors dwell, maxMode, and eligibility.
  Amc amc(amcCfg(), 8, 0.0);
  amc.onLocalSnr(40.0);
  amc.onQualityIndicators(0, 0.0);
  for (int i = 0; i < 20; ++i) amc.onFrameResult(false, true);
  int prev = 8;
  double t = 0.0;
  int changes = 0;
  for (int i = 0; i < 60; ++i) {
    t += 4.0;
    amc.applyAdvisorBias(+1);
    auto d = amc.evaluate(t);
    if (d.kind == AmcDecision::Kind::Promote) {
      CHECK_EQ(int(d.targetMode), prev + 1);  // single step, always
      amc.onModeApplied(d.targetMode, t);
      prev = d.targetMode;
      ++changes;
    }
    CHECK(prev <= 13);
  }
  CHECK(changes > 0);       // the bias did lean promotions forward
  CHECK(prev <= 13);
  // Demote bias: never more than one step down per decision either.
  Amc down(amcCfg(), 8, 1000.0);
  down.onLocalSnr(2.6);  // just above the M8 fading demote margin
  down.onQualityIndicators(0, 0.0);
  for (int i = 0; i < 20; ++i) down.onFrameResult(false, true);
  down.applyAdvisorBias(-1);
  auto d = down.evaluate(1010.0);
  if (d.kind == AmcDecision::Kind::Demote) {
    CHECK_EQ(int(d.targetMode), 7);
  }
}

TEST_CASE(advisor_off_vs_shadow_equivalence) {
  // Shadow computes advice but by construction cannot change an AMC
  // decision sequence: identical input streams yield identical
  // decisions whether the advisor is off or shadowing.
  auto runSeq = [](AdvisorMode mode) {
    AdvisorConfig cfg;
    cfg.mode = mode;
    Advisor adv(cfg);
    Amc amc(amcCfg(), 8, 0.0);
    amc.onQualityIndicators(0, 0.0);
    std::vector<int> modes;
    double t = 0.0;
    for (int i = 0; i < 40; ++i) {
      t += 4.0;
      const double snr = 6.0 + 0.25 * i;
      amc.onLocalSnr(snr);
      amc.onFrameResult(false, true);
      adv.onSnr(snr, t);
      auto advice = adv.adviseAmc(amc.retryRatio());
      if (adv.assistAmc(advice) && advice.stepBias != 0) {
        amc.applyAdvisorBias(advice.stepBias);  // never true here
      }
      auto d = amc.evaluate(t);
      if (d.kind == AmcDecision::Kind::Promote ||
          d.kind == AmcDecision::Kind::Demote) {
        amc.onModeApplied(d.targetMode, t);
      }
      modes.push_back(amc.mode());
    }
    return modes;
  };
  CHECK(runSeq(AdvisorMode::Off) == runSeq(AdvisorMode::Shadow));
}

// Engine wiring (2026-10, pre-on-air log review): the engine fed the
// advisor's SNR history on EVERY tick (~7 ms) and logged an evaluation
// each time - 147 debug lines/s, 112 MB per 2 h soak - while the 8-sample
// history spanned ~50 ms of identical values, so the AMC trend could
// never form. It is now fed once per new trusted measurement: over two
// minutes of traffic the history must span real time.
TEST_CASE(advisor_engine_feed_spans_real_time) {
  EngineConfig ca, cb;
  ca.policy.ownCall = "W1AW";
  cb.policy.ownCall = "K6XYZ";
  LinkEngine a(ca), b(cb);
  double now = 0.0;
  uint64_t sc = 1;
  auto pass = [&](LinkEngine& from, LinkEngine& to) {
    auto burst = from.tick(now);
    double dt = 0.05;
    if (burst) {
      dt += double(burst->size()) / kFsBaseband;
      std::vector<cfloat> x(1500, cfloat(0, 0));
      x.insert(x.end(), burst->begin(), burst->end());
      x.insert(x.end(), 2000, cfloat(0, 0));
      ChansimParams ch;
      ch.fading = false;
      ch.snr3kDb = 20.0;
      ch.seed = 77 + (sc++) * 7919;
      to.rxBurst(Chansim::run(ch, x), now);
    }
    now += dt;
  };
  a.connectTo("K6XYZ", now);
  for (int i = 0; i < 600 && a.state() != FsmState::Connected; ++i) {
    pass(a, b);
    pass(b, a);
  }
  CHECK(a.state() == FsmState::Connected);
  std::vector<uint8_t> chunk(1500, 0x5A);
  const double end = now + 120.0;
  while (now < end) {
    if (a.txBacklog() < 4000) a.sendData(chunk);
    pass(a, b);
    pass(b, a);
    b.takeReceivedData();
  }
  const double span = a.advisor().snrHistorySpanSec();
  std::printf("advisor SNR history span after 120 s: %.1f s\n", span);
  CHECK(span >= 10.0);
}

SKYTEST_MAIN
