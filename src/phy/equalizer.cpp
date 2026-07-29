#include "phy/equalizer.hpp"

#include <algorithm>
#include <cmath>

namespace skw {

ChannelTracker::ChannelTracker(Family family, bool robustClass,
                               std::vector<cfloat> h0, double noiseVar0)
    : family_(family), robust_(robustClass), h_(std::move(h0)),
      noiseVar_(std::max(noiseVar0, 1e-6)) {
  // DSP.md: alpha = 0.4 for unspread modes, 0.15 for the robust class
  // (longer integration at low per-pilot SNR). PLL gains likewise.
  alphaH_ = robust_ ? 0.15 : 0.4;
  g1_ = robust_ ? 0.15 : 0.35;
  g2_ = robust_ ? 0.015 : 0.05;
}

double ChannelTracker::snrDb() const {
  double p = 0;
  for (const auto& v : h_) p += std::norm(v);
  p /= double(h_.size());
  return 10.0 * std::log10(std::max(p / noiseVar_, 1e-9));
}

void ChannelTracker::processSymbol(std::span<cfloat> bins, int symIdx) {
  const int n = plan(family_).carriers();
  auto lay = symbolLayout(family_, symIdx);

  // Predicted CPE for this symbol.
  phase_ += freq_;
  cfloat derot(float(std::cos(-phase_)), float(std::sin(-phase_)));
  for (auto& b : bins) b *= derot;

  // Pilot innovations against the current H.
  cfloat rot(0, 0);
  for (int c : lay.pilotCarriers) {
    cfloat y = bins[size_t(carrierToBasebandBin(family_, c))];
    cfloat expect = h_[size_t(c)] * pilotValue(c, symIdx);
    rot += y * std::conj(expect);
  }
  const double innov = std::atan2(rot.imag(), rot.real());

  // Pilot residual power (after innovation removal) for noise tracking and
  // the FLYWHEEL decision.
  cfloat innovRot(float(std::cos(-innov)), float(std::sin(-innov)));
  double resid = 0.0;
  double pilotPow = 0.0;
  for (int c : lay.pilotCarriers) {
    cfloat y = bins[size_t(carrierToBasebandBin(family_, c))] * innovRot;
    cfloat expect = h_[size_t(c)] * pilotValue(c, symIdx);
    resid += std::norm(y - expect);
    pilotPow += std::norm(expect);
  }
  const int np = int(lay.pilotCarriers.size());
  resid /= std::max(np, 1);
  pilotPow /= std::max(np, 1);

  // FLYWHEEL: pilots look broken - hold the channel, coast the PLL.
  if (resid > 4.0 * (pilotPow + noiseVar_)) return;

  // PLL update.
  phase_ += g1_ * innov;
  freq_ += g2_ * innov;
  cfloat post(float(std::cos(-g1_ * innov)), float(std::sin(-g1_ * innov)));
  for (auto& b : bins) b *= post;

  // Noise estimate (pilot residual after the applied correction).
  noiseVar_ += 0.1 * (resid - noiseVar_);
  noiseVar_ = std::max(noiseVar_, 1e-9);

  // Per-carrier H update: pilot measurements interpolated linearly in
  // carrier index, IIR-blended in time.
  std::vector<cfloat> meas(static_cast<size_t>(np));
  for (int i = 0; i < np; ++i) {
    int c = lay.pilotCarriers[size_t(i)];
    cfloat y = bins[size_t(carrierToBasebandBin(family_, c))];
    meas[size_t(i)] = y / pilotValue(c, symIdx);
  }
  for (int c = 0; c < n; ++c) {
    // Find surrounding pilots.
    int lo = -1, hi = np;
    for (int i = 0; i < np; ++i) {
      if (lay.pilotCarriers[size_t(i)] <= c) lo = i;
      if (lay.pilotCarriers[size_t(i)] >= c) {
        hi = i;
        break;
      }
    }
    cfloat hInterp;
    if (lo < 0) hInterp = meas[size_t(hi)];
    else if (hi >= np) hInterp = meas[size_t(lo)];
    else if (lo == hi) hInterp = meas[size_t(lo)];
    else {
      int cLo = lay.pilotCarriers[size_t(lo)];
      int cHi = lay.pilotCarriers[size_t(hi)];
      float t = float(c - cLo) / float(cHi - cLo);
      hInterp = meas[size_t(lo)] + t * (meas[size_t(hi)] - meas[size_t(lo)]);
    }
    h_[size_t(c)] += float(alphaH_) * (hInterp - h_[size_t(c)]);
  }
}

void demapLlr(Constellation c, cfloat z, float a, float nvar,
              std::vector<float>& llrOut) {
  const int bps = bitsPerSymbol(c);
  const int m = 1 << bps;
  if (a < 1e-9f) {
    for (int b = 0; b < bps; ++b) llrOut.push_back(0.0f);
    return;
  }
  // z = a*s + n', var(n') = a*nvar  =>  |z/a - s|^2 * (a/nvar) is the
  // normalized metric.
  const cfloat est = z / a;
  const float scale = a / std::max(nvar, 1e-9f);
  float d[64];
  for (int v = 0; v < m; ++v) {
    d[v] = std::norm(est - mapSymbol(c, uint32_t(v))) * scale;
  }
  for (int b = bps - 1; b >= 0; --b) {
    float m0 = 1e30f, m1 = 1e30f;  // min metric with bit==0 / bit==1
    for (int v = 0; v < m; ++v) {
      if ((v >> b) & 1) m1 = std::min(m1, d[v]);
      else m0 = std::min(m0, d[v]);
    }
    float llr = std::clamp(m1 - m0, -30.0f, 30.0f);
    llrOut.push_back(llr);
  }
}

}  // namespace skw
