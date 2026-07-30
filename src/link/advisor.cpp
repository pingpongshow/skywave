#include "link/advisor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace skw {
namespace {

constexpr size_t kSnrCap = 8;
constexpr double kSnrHorizonSec = 120.0;
constexpr size_t kCfoCap = 10;
constexpr double kCfoHorizonSec = 180.0;
constexpr double kMaskHorizonSec = 90.0;

double envConf(const char* name, double dflt) {
  const char* v = getenv(name);
  if (!v || !*v) return dflt;
  char* end = nullptr;
  const double x = strtod(v, &end);
  if (end == v) return dflt;
  return std::clamp(x, 0.0, 1.0);
}

}  // namespace

AdvisorConfig AdvisorConfig::fromEnv() {
  AdvisorConfig c;
  if (const char* m = getenv("SKW_ADVISOR"); m && *m) {
    if (!strcmp(m, "off")) c.mode = AdvisorMode::Off;
    else if (!strcmp(m, "assist")) c.mode = AdvisorMode::Assist;
    else c.mode = AdvisorMode::Shadow;  // default / unknown values
  }
  c.amcConf = envConf("SKW_ADVISOR_AMC_CONF", c.amcConf);
  c.cfoConf = envConf("SKW_ADVISOR_CFO_CONF", c.cfoConf);
  c.maskConf = envConf("SKW_ADVISOR_MASK_CONF", c.maskConf);
  return c;
}

void Advisor::push(std::deque<Sample>& q, double t, double v, size_t cap,
                   double horizonSec) {
  q.push_back({t, v});
  while (q.size() > cap || (!q.empty() && t - q.front().t > horizonSec)) {
    q.pop_front();
  }
}

bool Advisor::fit(const std::deque<Sample>& q, double* slopePerMin,
                  double* rr, double* last) {
  if (q.size() < 3) return false;
  const double t0 = q.front().t;
  double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
  const double n = double(q.size());
  for (const auto& s : q) {
    const double x = (s.t - t0) / 60.0;  // minutes
    sx += x;
    sy += s.v;
    sxx += x * x;
    sxy += x * s.v;
    syy += s.v * s.v;
  }
  const double denom = n * sxx - sx * sx;
  if (denom < 1e-9) return false;  // no time spread
  const double slope = (n * sxy - sx * sy) / denom;
  const double varY = syy / n - (sy / n) * (sy / n);
  double r2 = 0.0;
  if (varY > 1e-9) {
    const double covXY = sxy / n - (sx / n) * (sy / n);
    const double varX = sxx / n - (sx / n) * (sx / n);
    r2 = varX > 1e-12 ? (covXY * covXY) / (varX * varY) : 0.0;
  }
  *slopePerMin = slope;
  *rr = std::clamp(r2, 0.0, 1.0);
  *last = q.back().v;
  return true;
}

void Advisor::onSnr(double effSnrDb, double now) {
  if (cfg_.mode == AdvisorMode::Off) return;
  if (effSnrDb <= -90.0) return;  // no-measurement sentinel
  push(snr_, now, effSnrDb, kSnrCap, kSnrHorizonSec);
}

Advisor::AmcAdvice Advisor::adviseAmc(double retryRatio) const {
  AmcAdvice a;
  double slope, r2, last;
  if (cfg_.mode == AdvisorMode::Off || !fit(snr_, &slope, &r2, &last)) {
    return a;
  }
  // A confident sustained trend leans AMC ONE evaluation earlier in the
  // direction the channel is already moving. Retries veto promote bias
  // (a lossy link must not be nudged upward, whatever the SNR says).
  constexpr double kSlopeGateDbPerMin = 1.5;
  if (slope >= kSlopeGateDbPerMin && retryRatio <= 0.10) {
    a.stepBias = 1;
  } else if (slope <= -kSlopeGateDbPerMin) {
    a.stepBias = -1;
  } else {
    return a;
  }
  a.confidence =
      std::clamp(std::abs(slope) / (2.0 * kSlopeGateDbPerMin), 0.0, 1.0) *
      r2;
  if (a.confidence <= 0.0) a.stepBias = 0;
  return a;
}

void Advisor::onCfo(double cfoHz, double now) {
  if (cfg_.mode == AdvisorMode::Off) return;
  push(cfo_, now, cfoHz, kCfoCap, kCfoHorizonSec);
}

Advisor::CfoAdvice Advisor::adviseCfo(double now) const {
  CfoAdvice a;
  double slope, r2, last;
  if (cfg_.mode == AdvisorMode::Off || !fit(cfo_, &slope, &r2, &last)) {
    return a;
  }
  a.driftHzPerMin = std::clamp(slope, -6.0, 6.0);  // sane oscillator drift
  const double dtMin = std::max(0.0, (now - cfo_.back().t) / 60.0);
  a.predictedHz = last + a.driftHzPerMin * std::min(dtMin, 2.0);
  a.confidence = r2;
  return a;
}

void Advisor::onMaskChange(double now) {
  if (cfg_.mode == AdvisorMode::Off) return;
  maskChanges_.push_back(now);
  while (!maskChanges_.empty() &&
         now - maskChanges_.front() > kMaskHorizonSec) {
    maskChanges_.pop_front();
  }
}

Advisor::MaskAdvice Advisor::adviseMask(double now) const {
  MaskAdvice a;
  if (cfg_.mode == AdvisorMode::Off) return a;
  int recent = 0;
  for (double t : maskChanges_) {
    if (now - t <= kMaskHorizonSec) ++recent;
  }
  // Mask churn (proposals flapping) means the threshold sits on the
  // noise: harden it, +0.5 dB per churn event beyond the first pair,
  // clamped. Quiet history advises nothing (the static default rules).
  if (recent >= 3) {
    a.thresholdDeltaDb = std::min(1.5, 0.5 * (recent - 2));
    a.confidence = std::clamp(0.4 + 0.15 * recent, 0.0, 1.0);
  }
  return a;
}

void Advisor::reset() {
  snr_.clear();
  cfo_.clear();
  maskChanges_.clear();
}

}  // namespace skw
