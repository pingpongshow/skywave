#include "link/mask.hpp"

#include <algorithm>

namespace skw {

void MaskDetector::onSnapshot(const std::vector<float>& snrDb, double now) {
  const int n = plan(family_).carriers();
  if (int(snrDb.size()) != n) return;
  if (!seeded_) {
    ema_.assign(size_t(n), 0.0);
    belowSince_.assign(size_t(n), -1.0);
    for (int c = 0; c < n; ++c) ema_[size_t(c)] = snrDb[size_t(c)];
    seeded_ = true;
    return;
  }
  for (int c = 0; c < n; ++c) {
    ema_[size_t(c)] += cfg_.emaAlpha * (snrDb[size_t(c)] - ema_[size_t(c)]);
  }
  // Band median of the EMA.
  std::vector<double> sorted = ema_;
  std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
  const double median = sorted[size_t(n / 2)];
  for (int c = 0; c < n; ++c) {
    if (ema_[size_t(c)] <= median - effectiveThresholdDb()) {
      if (belowSince_[size_t(c)] < 0) belowSince_[size_t(c)] = now;
    } else {
      belowSince_[size_t(c)] = -1.0;
    }
  }
}

CarrierMask MaskDetector::proposal(double now) const {
  CarrierMask m;
  if (!seeded_) return m;
  const int n = plan(family_).carriers();
  // Qualified candidates, worst (lowest EMA) first.
  std::vector<std::pair<double, int>> cand;
  for (int c = 0; c < n; ++c) {
    if (belowSince_[size_t(c)] >= 0 &&
        now - belowSince_[size_t(c)] >= cfg_.persistSec) {
      cand.push_back({ema_[size_t(c)], c});
    }
  }
  std::sort(cand.begin(), cand.end());
  const int cap = cfg_.maxMasked(n);
  for (int i = 0; i < int(cand.size()) && i < cap; ++i) {
    m.exclude(cand[size_t(i)].second);
  }
  return m;
}

}  // namespace skw
