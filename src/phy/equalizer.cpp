#include "phy/equalizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace skw {

ChannelTracker::ChannelTracker(Family family, bool robustClass,
                               std::vector<cfloat> h0, double noiseVar0)
    : family_(family), robust_(robustClass), h_(std::move(h0)),
      noiseVar_(std::max(noiseVar0, 1e-6)) {
  shape_ = h_;  // seed the slow structural estimate from the ZC handoff
  // The sliding-window LS already smooths; the blend just suppresses
  // solve-to-solve jitter. Robust class integrates longer windows at low
  // per-pilot SNR; fast class must chase fading.
  alphaH_ = robust_ ? 0.4 : 0.8;
  g1_ = robust_ ? 0.15 : 0.35;
  g2_ = robust_ ? 0.015 : 0.05;

  // Joint pilot LS setup: T taps at delays d_t = (t-1) * N/n, covering one
  // slightly-negative tap through ~CP+8. Pilot positions rotate by one
  // carrier per symbol, so a window of a few symbols supplies measurements
  // at 2-4x T distinct carriers - solvable without any interpolation or
  // edge extrapolation (which is what wrecks dispersive channels: flat
  // extension past the outermost pilot is ~0.24 rad/carrier wrong at 1 ms
  // delay spread).
  // Tap grid at HALF the segment's natural resolution (N/2n): a grid at
  // exactly N/n is an orthogonal DFT over the carrier segment, and any
  // off-grid physical delay then leaks slowly-decaying Dirichlet tails
  // beyond the retained window (the same segment-leakage failure as naive
  // DFT truncation). The 2x-oversampled non-orthogonal basis represents
  // off-grid delays with fast-decaying coefficients; ridge regularization
  // handles the resulting near-collinearity.
  const int n = plan(family_).carriers();
  const double spacing = double(kFftBaseband) / (2.0 * n);
  numTaps_ = 3 + int(56.0 / spacing);
  gPrior_.assign(size_t(numTaps_), {0.0, 0.0});
  // Window length: enough pilot measurements to overdetermine the tap
  // solve by ~3x. NARROW carries ~2 pilots/symbol against 7 taps, so a
  // 4-symbol window is barely determined and 64QAM EVM starves; families
  // with rich pilot budgets keep short windows for fading agility.
  const int np0 = std::max(int(symbolLayout(family_, 0).pilotCarriers.size()), 1);
  const int needed = (3 * numTaps_ + np0 - 1) / np0;
  histSymbols_ = robust_ ? std::max(8, needed) : std::max(4, needed);
  basis_.resize(size_t(n) * numTaps_);
  for (int c = 0; c < n; ++c) {
    for (int t = 0; t < numTaps_; ++t) {
      double d = (t - 2) * spacing;
      double ph = -2.0 * M_PI * double(c) * d / kFftBaseband;
      basis_[size_t(c) * numTaps_ + t] =
          cfloat(float(std::cos(ph)), float(std::sin(ph)));
    }
  }

  // Tap-domain fit of the ZC handoff estimate: the prior MEAN for every
  // subsequent solve. At deep-floor SNR the ZC block (coherent over 3-6
  // known symbols) is a far better estimate than any pilot-window LS (T
  // taps from a handful of noise-dominated pilots), so the solve must
  // shrink toward the seed - shrinking toward zero throws the seed away
  // the moment the window fills.
  {
    const int T = numTaps_;
    std::vector<std::complex<double>> M0(size_t(T) * T, {0.0, 0.0});
    std::vector<std::complex<double>> b0(size_t(T), {0.0, 0.0});
    for (int c = 0; c < n; ++c) {
      const cfloat* row = &basis_[size_t(c) * T];
      std::complex<double> hc(h_[size_t(c)].real(), h_[size_t(c)].imag());
      for (int i = 0; i < T; ++i) {
        std::complex<double> ri(row[i].real(), row[i].imag());
        b0[size_t(i)] += std::conj(ri) * hc;
        for (int j = i; j < T; ++j) {
          std::complex<double> rj(row[j].real(), row[j].imag());
          M0[size_t(i) * T + j] += std::conj(ri) * rj;
        }
      }
    }
    for (int i = 0; i < T; ++i) {
      for (int j = 0; j < i; ++j) {
        M0[size_t(i) * T + j] = std::conj(M0[size_t(j) * T + i]);
      }
      M0[size_t(i) * T + i] += 1e-3 * double(n);
    }
    solveHermitian(M0, b0, T);
    gPrior_ = b0;
  }
}

// In-place Gaussian elimination with partial pivoting; solution in b.
void ChannelTracker::solveHermitian(std::vector<std::complex<double>>& M,
                                    std::vector<std::complex<double>>& b,
                                    int T) {
  for (int col = 0; col < T; ++col) {
    int piv = col;
    for (int r = col + 1; r < T; ++r) {
      if (std::abs(M[size_t(r) * T + col]) >
          std::abs(M[size_t(piv) * T + col])) {
        piv = r;
      }
    }
    for (int c2 = 0; c2 < T; ++c2) {
      std::swap(M[size_t(col) * T + c2], M[size_t(piv) * T + c2]);
    }
    std::swap(b[size_t(col)], b[size_t(piv)]);
    std::complex<double> d = M[size_t(col) * T + col];
    if (std::abs(d) < 1e-12) continue;
    for (int c2 = col; c2 < T; ++c2) M[size_t(col) * T + c2] /= d;
    b[size_t(col)] /= d;
    for (int r = 0; r < T; ++r) {
      if (r == col) continue;
      std::complex<double> f = M[size_t(r) * T + col];
      if (std::abs(f) == 0.0) continue;
      for (int c2 = col; c2 < T; ++c2) {
        M[size_t(r) * T + c2] -= f * M[size_t(col) * T + c2];
      }
      b[size_t(r)] -= f * b[size_t(col)];
    }
  }
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
  const int np = int(lay.pilotCarriers.size());

  // Fast/slow decomposition: fading dynamics are dominated by a COMMON
  // complex gain (flat component - fast, measurable per symbol from this
  // symbol's pilots), while the tap SHAPE decorrelates much more slowly.
  // Fit the scalar per symbol against the current shape, then push
  // gain-normalized measurements into the long shape window so the shape
  // fit stays stationary.
  std::vector<PilotMeas> cur;
  cur.reserve(size_t(np));
  cfloat num(0, 0);
  float den = 0;
  for (int c : lay.pilotCarriers) {
    cfloat m = bins[size_t(carrierToBasebandBin(family_, c))] /
               pilotValue(c, symIdx);
    cur.push_back({c, m});
    num += m * std::conj(shape_[size_t(c)]);
    den += std::norm(shape_[size_t(c)]);
  }
  cfloat gain = den > 1e-12f ? num / den : cfloat(1, 0);

  // Residual and FLYWHEEL check against gain * shape.
  double resid = 0.0;
  double pilotPow = 0.0;
  for (const auto& pm : cur) {
    cfloat expect = gain * shape_[size_t(pm.carrier)];
    resid += std::norm(pm.h - expect);
    pilotPow += std::norm(expect);
  }
  resid /= std::max(np, 1);
  pilotPow /= std::max(np, 1);
  if (resid > 4.0 * (pilotPow + noiseVar_)) return;  // FLYWHEEL hold

  // Fast warm-up, then slow symmetric tracking. A conservatively high
  // ZC-handoff seed otherwise keeps the noise-scaled ridge inflated for
  // tens of symbols (the ridge bias itself elevates residuals, so the
  // loop converges at the EMA rate, not the window rate) - NARROW's first
  // payload codeword spans ~30 symbols and eats the whole transient.
  // Steady state must stay SYMMETRIC: chasing downward excursions tracks
  // the minimum of a noisy chi-square, and the resulting low-biased nvar
  // makes LLRs overconfident and trips false FLYWHEEL holds at floor SNR.
  noiseVar_ += (nvarUpdates_ < 8 ? 0.4 : 0.1) * (resid - noiseVar_);
  ++nvarUpdates_;
  noiseVar_ = std::max(noiseVar_, 1e-9);
  lastGain_ = gain;

  // PLL-smoothed gain: the raw per-symbol fit above is the gauge of
  // everything downstream, and for pilot-poor families at low SNR it is
  // mostly measurement noise (NARROW: 2 pilots at 0 dB -> sigma ~0.5 rad,
  // which alone destroys QPSK). The PHYSICAL common gain moves slowly
  // (residual CFO + fade rotation), so a 2nd-order phase PLL plus an
  // amplitude EMA recovers most of the SNR. The smoothed value - not the
  // raw fit - anchors the gauge: it is stored with the measurements and
  // reused unchanged in every later solve (a stored PHYSICAL trajectory
  // cannot gauge-oscillate the way stale re-fit gains did).
  {
    const double pred = phase_ + freq_;
    double innov = std::atan2(gain.imag(), gain.real()) - pred;
    while (innov > M_PI) innov -= 2.0 * M_PI;
    while (innov < -M_PI) innov += 2.0 * M_PI;
    phase_ = pred + g1_ * innov;
    freq_ += g2_ * innov;
    gainAmp_ += 0.25 * (std::abs(gain) - gainAmp_);
    gainAmp_ = std::max(gainAmp_, 1e-4);
  }
  const cfloat gS(float(gainAmp_ * std::cos(phase_)),
                  float(gainAmp_ * std::sin(phase_)));

  hist_.push_back({std::move(cur), gS});
  if (int(hist_.size()) > histSymbols_) hist_.erase(hist_.begin());

  const int T = numTaps_;
  // Normal equations M g = b over the window, each symbol's measurements
  // normalized by its STORED PLL gain and weighted by age * |gS|^2
  // (deep-faded symbols carry amplified noise after normalization -
  // |gS|^2 weighting is the MRC optimum and keeps them from poisoning
  // the shape). The solved g is the tap-domain SHAPE in the PLL's unit
  // gauge - the same gauge as the ZC prior.
  std::vector<std::complex<double>> M(size_t(T) * T, {0.0, 0.0});
  std::vector<std::complex<double>> b(size_t(T), {0.0, 0.0});
  const double ageDecay = robust_ ? 0.9 : 0.8;
  double w = 1.0;
  double sumW = 0.0;  // total LS weight, for noise-scaled ridge
  const int newest = int(hist_.size()) - 1;
  for (int a = newest; a >= 0; --a) {
    const cfloat ca = hist_[size_t(a)].gain;
    double ca2 = std::norm(ca);
    if (ca2 < 1e-8) {
      w *= ageDecay;
      continue;
    }
    const double wEff = w * ca2;
    const std::complex<double> caInv =
        1.0 / std::complex<double>(ca.real(), ca.imag());
    sumW += wEff * double(hist_[size_t(a)].pilots.size());
    for (const auto& pm : hist_[size_t(a)].pilots) {
      const cfloat* row = &basis_[size_t(pm.carrier) * T];
      std::complex<double> mNorm =
          std::complex<double>(pm.h.real(), pm.h.imag()) * caInv;
      for (int i = 0; i < T; ++i) {
        std::complex<double> ri(row[i].real(), row[i].imag());
        b[size_t(i)] += wEff * std::conj(ri) * mNorm;
        for (int j = i; j < T; ++j) {
          std::complex<double> rj(row[j].real(), row[j].imag());
          M[size_t(i) * T + j] += wEff * std::conj(ri) * rj;
        }
      }
    }
    w *= ageDecay;
  }
  // Noise-scaled ridge TOWARD THE ZC PRIOR (MAP with prior mean gPrior_):
  // lambda = 3*nvar makes the prior dominate exactly when pilots are
  // noise-dominated (deep-floor robust headers, where the coherent ZC
  // estimate is the best information available) and become negligible
  // when pilots are clean (64QAM needs the unbiased pilot solve). A
  // fixed zero-target ridge either biases clean EVM (pilot-poor
  // families) or discards the seed at floor - both were observed.
  const double lambda = 3.0 * noiseVar_ + 1e-3;
  for (int i = 0; i < T; ++i) {
    for (int j = 0; j < i; ++j) {
      M[size_t(i) * T + j] = std::conj(M[size_t(j) * T + i]);
    }
    M[size_t(i) * T + i] += lambda;
    b[size_t(i)] += lambda * gPrior_[size_t(i)];
  }
  solveHermitian(M, b, T);
  const std::vector<std::complex<double>>& g = b;
  (void)sumW;
  // Project back to carriers: shape in the PLL's unit gauge. While the
  // window is still FILLING the solve is ridge-limited, so lean on the
  // previous shape in proportion to the missing fill. The demap estimate
  // for THIS symbol is gS * shape - the smoothed gain restores the gauge.
  const float fill = float(hist_.size()) / float(histSymbols_);
  for (int c = 0; c < n; ++c) {
    const cfloat* row = &basis_[size_t(c) * T];
    std::complex<double> acc(0, 0);
    for (int t = 0; t < T; ++t) {
      acc += std::complex<double>(row[t].real(), row[t].imag()) *
             g[size_t(t)];
    }
    cfloat sNew(float(acc.real()), float(acc.imag()));
    cfloat shapeNew = fill * sNew + (1.0f - fill) * shape_[size_t(c)];
    shape_[size_t(c)] = shapeNew;
    h_[size_t(c)] = gS * shapeNew;
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
