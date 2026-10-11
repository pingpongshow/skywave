#include "phy/equalizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace skw {

ChannelTracker::ChannelTracker(Family family, bool robustClass,
                               std::vector<cfloat> h0, double noiseVar0,
                               double meanChanPow)
    : family_(family), robust_(robustClass), h_(std::move(h0)),
      noiseVar_(std::max(noiseVar0, 1e-6)) {
  // Tap grid at HALF the segment's natural resolution (N/2n): a grid at
  // exactly N/n is an orthogonal DFT over the carrier segment, and any
  // off-grid physical delay then leaks slowly-decaying Dirichlet tails
  // beyond the retained window. The 2x-oversampled non-orthogonal basis
  // represents off-grid delays with fast-decaying coefficients; the
  // Kalman prior handles the resulting near-collinearity.
  const int n = plan(family_).carriers();
  const double spacing = double(kFftBaseband) / (2.0 * n);
  k0_ = trackerPreTaps();
  numTaps_ = 1 + k0_ + int(56.0 / spacing);
  const int T = numTaps_;
  basis_.resize(size_t(n) * T);
  for (int c = 0; c < n; ++c) {
    for (int t = 0; t < T; ++t) {
      double d = (t - k0_) * spacing;
      double ph = -2.0 * M_PI * double(c) * d / kFftBaseband;
      basis_[size_t(c) * T + t] =
          cfloat(float(std::cos(ph)), float(std::sin(ph)));
    }
  }

  // Initial tap state: LS fit of the ZC handoff estimate onto the grid.
  g_.assign(size_t(T), {0.0, 0.0});
  {
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
    g_ = b0;
  }

  // Gauss-Markov dynamics: rho = exp(-2 (pi fd Ts)^2), designed at the
  // worst-case Doppler each class must survive (robust: CCIR Poor 1 Hz;
  // fast: AMC-gated to <= 0.5 Hz channels). NOTE (empirical, see ROADMAP
  // 1.13): the residual tracking floor under fading (~ -8 dB vs signal
  // at high SNR) is NOT process-noise-limited - varying fd/Q trades
  // per-family wins and losses without moving the floor; it is the
  // moving off-grid tap representation itself. Model refinement (e.g.
  // tracked physical-delay taps, adaptive Q) is Phase-3 work.
  // Process noise follows a SMOOTHED seed delay profile: the seed's
  // instantaneous tap powers say where the channel's energy lives, but a
  // tap that happened to be mid-fade at ZC time must not be starved
  // forever (that starved taps was the failure of the per-tap-
  // proportional first cut), so take the neighborhood max plus a floor.
  const double fd = robust_ ? 1.0 : 0.5;
  const double Ts =
      double(kFftBaseband + cpSamplesAudio(CpMode(1)) / kDecimation) / kFsBaseband;
  const double arg = 2.0 * std::pow(M_PI * fd * Ts, 2.0);
  rho_ = std::exp(-arg);
  double seedPow = 0.0;
  for (const auto& v : g_) seedPow += std::norm(v);
  seedPow = std::max(seedPow, 1e-6);
  // Total process-noise power: the frame-average channel power when the
  // caller measured it (R5), else the ZC-time seed power. The profile
  // SHAPE below still comes from the seed taps.
  const double qPow = meanChanPow > 0.0 ? meanChanPow : seedPow;
  qDiag_.assign(size_t(T), 0.0);
  const double qScale = 1.0 - rho_ * rho_;
  {
    // Profile shape: neighborhood max spreads each seed tap's power to
    // adjacent grid delays; the floor keeps starved taps acquirable.
    // Then NORMALIZE the total to the physical steady-state power
    // (seedPow) - the un-normalized neighborhood sum is ~5x too big, and
    // total process noise beyond the channel's actual power inflates the
    // posterior without bound, crushing the LLR scale via noiseVar().
    double sum = 0.0;
    for (int t = 0; t < T; ++t) {
      double prof = 0.0;
      for (int u = std::max(0, t - 2); u <= std::min(T - 1, t + 2); ++u) {
        prof = std::max(prof, std::norm(g_[size_t(u)]));
      }
      qDiag_[size_t(t)] = prof + 0.1 * seedPow / T;
      sum += qDiag_[size_t(t)];
    }
    const double norm = qScale * 1.2 * qPow / std::max(sum, 1e-12);
    for (auto& q : qDiag_) q *= norm;
  }

  // Initial covariance: the ZC estimate is good but not perfect; start
  // with a modest fraction of the per-tap steady-state power.
  P_.assign(size_t(T) * T, {0.0, 0.0});
  for (int t = 0; t < T; ++t) {
    P_[size_t(t) * T + t] = 0.05 * qDiag_[size_t(t)] / qScale;
  }
  hVar_ = 0.0;
  residEma_.assign(size_t(n), -1.0);
}

double ChannelTracker::delaySpreadMs() const {
  const int T = numTaps_;
  const int n = plan(family_).carriers();
  const double spacing = double(kFftBaseband) / (2.0 * n);
  double peak = 0;
  for (const auto& v : g_) peak = std::max(peak, std::norm(v));
  if (peak <= 0) return 0.0;
  int first = -1, last = -1;
  for (int t = 0; t < T; ++t) {
    if (std::norm(g_[size_t(t)]) >= 0.1 * peak) {
      if (first < 0) first = t;
      last = t;
    }
  }
  if (first < 0) return 0.0;
  return double(last - first) * spacing / kFsBaseband * 1000.0;
}

double ChannelTracker::snrDb() const {
  double p = 0;
  for (const auto& v : h_) p += std::norm(v);
  p /= double(h_.size());
  return 10.0 * std::log10(std::max(p / noiseVar_, 1e-9));
}

void ChannelTracker::processSymbol(std::span<cfloat> bins, int symIdx) {
  const int n = plan(family_).carriers();
  const int T = numTaps_;
  auto lay = symbolLayout(family_, symIdx, mask_);
  const int np = int(lay.pilotCarriers.size());
  if (np == 0) {
    if (recording_) hist_.push_back({g_, P_, false, noiseVar_});
    return;
  }

  // Predict: g <- rho g, P <- rho^2 P + Q.
  for (auto& v : g_) v *= rho_;
  const double r2 = rho_ * rho_;
  for (auto& v : P_) v *= r2;
  for (int t = 0; t < T; ++t) P_[size_t(t) * T + t] += qDiag_[size_t(t)];

  // Pilot measurements m = A g + noise, A rows = basis rows of the pilot
  // carriers (divided by the known pilot value happens on capture).
  std::vector<std::complex<double>> m(static_cast<size_t>(np));
  std::vector<const cfloat*> rows(static_cast<size_t>(np));
  {
    int k = 0;
    for (int c : lay.pilotCarriers) {
      cfloat meas = bins[size_t(carrierToBasebandBin(family_, c))] /
                    pilotValue(c, symIdx);
      m[size_t(k)] = std::complex<double>(meas.real(), meas.imag());
      rows[size_t(k)] = &basis_[size_t(c) * T];
      ++k;
    }
  }

  // Innovation and residual gating (FLYWHEEL): compare against predicted
  // H at the pilots. Track the raw common-gain fit for telemetry.
  double resid = 0.0;
  double pilotPow = 0.0;
  std::complex<double> gnum(0, 0);
  double gden = 0;
  {
    int k = 0;
    for (int c : lay.pilotCarriers) {
      std::complex<double> pred(0, 0);
      const cfloat* row = rows[size_t(k)];
      for (int t = 0; t < T; ++t) {
        pred += std::complex<double>(row[t].real(), row[t].imag()) *
                g_[size_t(t)];
      }
      const double r2c = std::norm(m[size_t(k)] - pred);
      resid += r2c;
      double& e = residEma_[size_t(c)];
      e = e < 0 ? r2c : e + 0.3 * (r2c - e);
      pilotPow += std::norm(pred);
      gnum += m[size_t(k)] * std::conj(pred);
      gden += std::norm(pred);
      ++k;
    }
  }
  resid /= np;
  pilotPow /= np;
  lastGain_ = gden > 1e-12
                  ? cfloat(float((gnum / gden).real()),
                           float((gnum / gden).imag()))
                  : cfloat(1, 0);
  phase_ = std::atan2(lastGain_.imag(), lastGain_.real());

  // Bootstrap R: the ZC handoff's noiseVar0 is an H-error figure, not
  // per-pilot measurement noise - at floor SNR it can be 15x low, and an
  // underestimated R makes the very first Kalman gains enormous (observed
  // divergence: state jumps onto noise, covariance loses definiteness).
  // The first innovation against the good ZC seed is essentially pure
  // measurement noise, so it calibrates R directly.
  if (nvarUpdates_ == 0) noiseVar_ = std::max(noiseVar_, 0.8 * resid);

  // Expected innovation power from filter uncertainty alone: mean of the
  // per-pilot prior variances s_k = row P row^H (pre-update pass).
  double meanPrior = 0.0;
  for (int k = 0; k < np; ++k) {
    const cfloat* row = rows[size_t(k)];
    double s = 0.0;
    for (int t = 0; t < T; ++t) {
      std::complex<double> acc(0, 0);
      for (int u = 0; u < T; ++u) {
        acc += P_[size_t(t) * T + u] *
               std::conj(
                   std::complex<double>(row[u].real(), row[u].imag()));
      }
      s += (std::complex<double>(row[t].real(), row[t].imag()) * acc).real();
    }
    meanPrior += std::max(s, 0.0);
  }
  meanPrior /= np;
  const double meanS = meanPrior + noiseVar_;

  static const bool eqDbg = getenv("SKW_EQ_DEBUG") != nullptr;
  // FLYWHEEL: hold only on a residual far beyond what the filter itself
  // expects (interference, collision) - meanS already accounts for honest
  // mid-fade innovation, so the gate is S-normalized.
  const bool hold = resid > 10.0 * meanS;
  if (eqDbg) {
    fprintf(stderr,
            "eq: sym=%d |g|=%.3f resid=%.3g pp=%.3g nv=%.3g hv=%.3g "
            "S=%.3g%s\n",
            symIdx, double(std::abs(lastGain_)), resid, pilotPow, noiseVar_,
            hVar_, meanS, hold ? " HOLD" : "");
  }
  if (!hold) {
    // Noise tracking: the innovation power resid contains the estimation
    // -error share meanPrior; subtract it so nvar tracks the MEASUREMENT
    // noise, clamped to stay positive under fluctuation.
    const double nvarTarget = std::max(resid - meanPrior, 0.2 * resid);
    noiseVar_ += (nvarUpdates_ < 8 ? 0.4 : 0.1) * (nvarTarget - noiseVar_);
    ++nvarUpdates_;
    noiseVar_ = std::max(noiseVar_, 1e-9);

    // SEQUENTIAL scalar measurement updates (one pilot at a time). With
    // diagonal R this is algebraically the joint update, but each
    // innovation variance s = row P row^H + R is a positive scalar - no
    // matrix inversion, and none of the joint update's indefiniteness
    // failure modes.
    std::vector<std::complex<double>> u(static_cast<size_t>(T));
    for (int k = 0; k < np; ++k) {
      const cfloat* row = rows[size_t(k)];
      // u = P row^H ; s = row u + R ; pred = row g.
      double s = noiseVar_;
      std::complex<double> pred(0, 0);
      for (int t = 0; t < T; ++t) {
        std::complex<double> acc(0, 0);
        for (int w = 0; w < T; ++w) {
          acc += P_[size_t(t) * T + w] *
                 std::conj(
                     std::complex<double>(row[w].real(), row[w].imag()));
        }
        u[size_t(t)] = acc;
        std::complex<double> rt(row[t].real(), row[t].imag());
        s += (rt * acc).real();
        pred += rt * g_[size_t(t)];
      }
      s = std::max(s, 0.1 * noiseVar_ + 1e-12);
      const std::complex<double> e = m[size_t(k)] - pred;
      const double sInv = 1.0 / s;
      for (int t = 0; t < T; ++t) {
        g_[size_t(t)] += u[size_t(t)] * sInv * e;
      }
      for (int t = 0; t < T; ++t) {
        for (int w = 0; w < T; ++w) {
          P_[size_t(t) * T + w] -=
              u[size_t(t)] * std::conj(u[size_t(w)]) * sInv;
        }
      }
    }
    // Numerical hygiene: keep P Hermitian with a tiny diagonal floor.
    for (int t = 0; t < T; ++t) {
      for (int w = 0; w < t; ++w) {
        std::complex<double> avg =
            0.5 * (P_[size_t(t) * T + w] + std::conj(P_[size_t(w) * T + t]));
        P_[size_t(t) * T + w] = avg;
        P_[size_t(w) * T + t] = std::conj(avg);
      }
      double dg = P_[size_t(t) * T + t].real();
      P_[size_t(t) * T + t] = std::max(dg, 1e-9);
    }
  }

  // Project the state to per-carrier H and the mean posterior variance
  // (row P row^H averaged over carriers - the demapper's uncertainty).
  // The projection must be EXACT: P's off-diagonal structure (updates
  // concentrate along the observed subspace) makes the trace a large
  // overcount, and the demapper's LLR scale rides on this number.
  double hv = 0.0;
  std::vector<std::complex<double>> Pr(static_cast<size_t>(T));
  for (int c = 0; c < n; ++c) {
    const cfloat* row = &basis_[size_t(c) * T];
    std::complex<double> acc(0, 0);
    double v = 0.0;
    for (int t = 0; t < T; ++t) {
      std::complex<double> rt(row[t].real(), row[t].imag());
      acc += rt * g_[size_t(t)];
      std::complex<double> pa(0, 0);
      for (int u = 0; u < T; ++u) {
        std::complex<double> ru(row[u].real(), row[u].imag());
        pa += P_[size_t(t) * T + u] * std::conj(ru);
      }
      v += (rt * pa).real();
    }
    h_[size_t(c)] = cfloat(float(acc.real()), float(acc.imag()));
    hv += std::max(v, 0.0);
  }
  hVar_ = hv / std::max(n, 1);
  if (recording_) hist_.push_back({g_, P_, true, noiseVar_});
}

void ChannelTracker::projectH(const std::vector<std::complex<double>>& g,
                              const std::vector<std::complex<double>>& P,
                              std::vector<cfloat>& h, double& hv) const {
  const int T = numTaps_;
  const int n = plan(family_).carriers();
  h.resize(size_t(n));
  double sum = 0.0;
  for (int c = 0; c < n; ++c) {
    const cfloat* row = &basis_[size_t(c) * T];
    std::complex<double> acc(0, 0);
    double v = 0.0;
    for (int t = 0; t < T; ++t) {
      std::complex<double> rt(row[t].real(), row[t].imag());
      acc += rt * g[size_t(t)];
      std::complex<double> pa(0, 0);
      for (int u = 0; u < T; ++u) {
        pa += P[size_t(t) * T + u] *
              std::conj(std::complex<double>(row[u].real(), row[u].imag()));
      }
      v += (rt * pa).real();
    }
    h[size_t(c)] = cfloat(float(acc.real()), float(acc.imag()));
    sum += std::max(v, 0.0);
  }
  hv = sum / std::max(n, 1);
}

void ChannelTracker::beginHistory() {
  recording_ = true;
  hist_.clear();
}

// Rauch-Tung-Striebel backward pass over the recorded span. With
// transition g' = rho g + w (w ~ Q), for a step that ran the predict:
//   Pp_{k+1} = rho^2 Pf_k + Q,  gp_{k+1} = rho gf_k
//   G_k = rho Pf_k Pp_{k+1}^-1
//   gs_k = gf_k + G_k (gs_{k+1} - gp_{k+1})
//   Ps_k = Pf_k + G_k (Ps_{k+1} - Pp_{k+1}) G_k^H
// A pilot-less symbol ran no predict (state unchanged), so across it the
// smoothed state simply carries over.
void ChannelTracker::smoothHistory(std::vector<std::vector<cfloat>>& hOut,
                                   std::vector<double>& nvOut) {
  using cd = std::complex<double>;
  recording_ = false;
  const int T = numTaps_;
  const size_t K = hist_.size();
  hOut.assign(K, {});
  nvOut.assign(K, 0.0);
  if (K == 0) return;
  std::vector<cd> gs = hist_[K - 1].g, Ps = hist_[K - 1].P;
  double hv = 0.0;
  projectH(gs, Ps, hOut[K - 1], hv);
  nvOut[K - 1] = hist_[K - 1].r + hv;
  const size_t TT = size_t(T) * size_t(T);
  std::vector<cd> Pp(TT), X(TT), G(TT), GD(TT), dg(static_cast<size_t>(T));
  for (size_t k = K - 1; k-- > 0;) {
    const HistEntry& f = hist_[k];
    if (!hist_[k + 1].predicted) {
      // No predict between k and k+1: smoothed state carries over.
      projectH(gs, Ps, hOut[k], hv);
      nvOut[k] = f.r + hv;
      continue;
    }
    const double r2 = rho_ * rho_;
    for (size_t i = 0; i < Pp.size(); ++i) Pp[i] = r2 * f.P[i];
    for (int t = 0; t < T; ++t) Pp[size_t(t) * T + t] += qDiag_[size_t(t)];
    // Solve Pp X = Pf (Gauss-Jordan, partial pivoting); G = rho X^H since
    // Pf and Pp are Hermitian: G = rho Pf Pp^-1 = rho (Pp^-1 Pf)^H.
    std::vector<cd> M = Pp;
    X = f.P;
    for (int col = 0; col < T; ++col) {
      int piv = col;
      for (int r = col + 1; r < T; ++r) {
        if (std::abs(M[size_t(r) * T + col]) >
            std::abs(M[size_t(piv) * T + col])) {
          piv = r;
        }
      }
      if (piv != col) {
        for (int c2 = 0; c2 < T; ++c2) {
          std::swap(M[size_t(col) * T + c2], M[size_t(piv) * T + c2]);
          std::swap(X[size_t(col) * T + c2], X[size_t(piv) * T + c2]);
        }
      }
      const cd d = M[size_t(col) * T + col];
      if (std::abs(d) < 1e-15) continue;
      for (int c2 = 0; c2 < T; ++c2) {
        M[size_t(col) * T + c2] /= d;
        X[size_t(col) * T + c2] /= d;
      }
      for (int r = 0; r < T; ++r) {
        if (r == col) continue;
        const cd fr = M[size_t(r) * T + col];
        if (fr == cd(0, 0)) continue;
        for (int c2 = 0; c2 < T; ++c2) {
          M[size_t(r) * T + c2] -= fr * M[size_t(col) * T + c2];
          X[size_t(r) * T + c2] -= fr * X[size_t(col) * T + c2];
        }
      }
    }
    for (int i = 0; i < T; ++i) {
      for (int j = 0; j < T; ++j) {
        G[size_t(i) * T + j] = rho_ * std::conj(X[size_t(j) * T + i]);
      }
    }
    for (int t = 0; t < T; ++t) dg[size_t(t)] = gs[size_t(t)] - rho_ * f.g[size_t(t)];
    std::vector<cd> gNew = f.g;
    for (int i = 0; i < T; ++i) {
      cd a(0, 0);
      for (int j = 0; j < T; ++j) a += G[size_t(i) * T + j] * dg[size_t(j)];
      gNew[size_t(i)] += a;
    }
    // GD = G (Ps - Pp);  Ps_k = Pf + GD G^H.
    for (int i = 0; i < T; ++i) {
      for (int j = 0; j < T; ++j) {
        cd a(0, 0);
        for (int u = 0; u < T; ++u) {
          a += G[size_t(i) * T + u] *
               (Ps[size_t(u) * T + j] - Pp[size_t(u) * T + j]);
        }
        GD[size_t(i) * T + j] = a;
      }
    }
    std::vector<cd> PNew = f.P;
    for (int i = 0; i < T; ++i) {
      for (int j = 0; j < T; ++j) {
        cd a(0, 0);
        for (int u = 0; u < T; ++u) {
          a += GD[size_t(i) * T + u] * std::conj(G[size_t(j) * T + u]);
        }
        PNew[size_t(i) * T + j] += a;
      }
    }
    gs = std::move(gNew);
    Ps = std::move(PNew);
    projectH(gs, Ps, hOut[k], hv);
    nvOut[k] = f.r + hv;
  }
  hist_.clear();
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
  float d[256];
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
