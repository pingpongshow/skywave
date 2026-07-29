#include "phy/equalizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace skw {

ChannelTracker::ChannelTracker(Family family, bool robustClass,
                               std::vector<cfloat> h0, double noiseVar0)
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
  numTaps_ = 3 + int(56.0 / spacing);
  const int T = numTaps_;
  basis_.resize(size_t(n) * T);
  for (int c = 0; c < n; ++c) {
    for (int t = 0; t < T; ++t) {
      double d = (t - 2) * spacing;
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

  // Gauss-Markov dynamics. rho from the worst-case Doppler the class must
  // survive (CCIR Poor 1 Hz for robust, Moderate 0.5 Hz for fast at the
  // shorter symbol dwell): rho = exp(-2 (pi fd Ts)^2). Process noise is
  // distributed per tap proportional to the SEED's tap powers (delays
  // where the ZC saw energy are where fading happens), with a small floor
  // so a tap the seed missed can still be acquired.
  const double fd = robust_ ? 1.0 : 1.0;
  const double Ts =
      double(kFftBaseband + kCpSamplesAudio[1] / kDecimation) / kFsBaseband;
  const double arg = 2.0 * std::pow(M_PI * fd * Ts, 2.0);
  rho_ = std::exp(-arg);
  double seedPow = 0.0;
  for (const auto& v : g_) seedPow += std::norm(v);
  seedPow = std::max(seedPow, 1e-6);
  qDiag_.assign(size_t(T), 0.0);
  const double qScale = 1.0 - rho_ * rho_;
  for (int t = 0; t < T; ++t) {
    qDiag_[size_t(t)] =
        qScale * (std::norm(g_[size_t(t)]) + 0.02 * seedPow / T);
  }

  // Initial covariance: the ZC estimate is good but not perfect; start
  // with a modest fraction of the per-tap steady-state power.
  P_.assign(size_t(T) * T, {0.0, 0.0});
  for (int t = 0; t < T; ++t) {
    P_[size_t(t) * T + t] = 0.05 * (std::norm(g_[size_t(t)]) +
                                    0.02 * seedPow / T);
  }
  hVar_ = 0.0;
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
  auto lay = symbolLayout(family_, symIdx);
  const int np = int(lay.pilotCarriers.size());
  if (np == 0) return;

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
  for (int k = 0; k < np; ++k) {
    std::complex<double> pred(0, 0);
    const cfloat* row = rows[size_t(k)];
    for (int t = 0; t < T; ++t) {
      pred += std::complex<double>(row[t].real(), row[t].imag()) *
              g_[size_t(t)];
    }
    resid += std::norm(m[size_t(k)] - pred);
    pilotPow += std::norm(pred);
    gnum += m[size_t(k)] * std::conj(pred);
    gden += std::norm(pred);
  }
  resid /= np;
  pilotPow /= np;
  lastGain_ = gden > 1e-12
                  ? cfloat(float((gnum / gden).real()),
                           float((gnum / gden).imag()))
                  : cfloat(1, 0);
  phase_ = std::atan2(lastGain_.imag(), lastGain_.real());

  static const bool eqDbg = getenv("SKW_EQ_DEBUG") != nullptr;
  // FLYWHEEL: a residual explosion means interference or a collision -
  // skip the measurement update (prediction already ran, so uncertainty
  // grows and later good pilots re-acquire). The gate must be generous:
  // mid-fade phase slew produces honest innovations.
  const bool hold = resid > 25.0 * (pilotPow + noiseVar_);
  if (eqDbg) {
    fprintf(stderr,
            "eq: sym=%d |g|=%.3f resid=%.3g pp=%.3g nv=%.3g hv=%.3g%s\n",
            symIdx, double(std::abs(lastGain_)), resid, pilotPow, noiseVar_,
            hVar_, hold ? " HOLD" : "");
  }
  if (!hold) {
    // Noise tracking from residuals. The residual contains estimation
    // error too; the posterior term reported by noiseVar() covers that
    // separately, so subtract the predicted-H uncertainty share crudely
    // by tracking toward the smaller of resid and its EMA fast/slow.
    noiseVar_ += (nvarUpdates_ < 8 ? 0.4 : 0.1) * (resid - noiseVar_);
    ++nvarUpdates_;
    noiseVar_ = std::max(noiseVar_, 1e-9);

    // Measurement update: S = A P A^H + R, K = P A^H S^-1,
    // g += K (m - A g), P -= K A P.  (np x np solve, np <= ~10.)
    std::vector<std::complex<double>> PA(size_t(T) * np);  // P A^H
    for (int t = 0; t < T; ++t) {
      for (int k = 0; k < np; ++k) {
        std::complex<double> acc(0, 0);
        const cfloat* row = rows[size_t(k)];
        for (int u = 0; u < T; ++u) {
          acc += P_[size_t(t) * T + u] *
                 std::conj(std::complex<double>(row[u].real(), row[u].imag()));
        }
        PA[size_t(t) * np + k] = acc;
      }
    }
    std::vector<std::complex<double>> S(size_t(np) * np, {0.0, 0.0});
    for (int k = 0; k < np; ++k) {
      const cfloat* rk = rows[size_t(k)];
      for (int l = 0; l < np; ++l) {
        std::complex<double> acc(0, 0);
        for (int t = 0; t < T; ++t) {
          acc += std::complex<double>(rk[t].real(), rk[t].imag()) *
                 PA[size_t(t) * np + l];
        }
        S[size_t(k) * np + l] = std::conj(acc);
      }
    }
    for (int k = 0; k < np; ++k) S[size_t(k) * np + k] += noiseVar_;
    // Innovation vector.
    std::vector<std::complex<double>> innov(static_cast<size_t>(np));
    for (int k = 0; k < np; ++k) {
      std::complex<double> pred(0, 0);
      const cfloat* row = rows[size_t(k)];
      for (int t = 0; t < T; ++t) {
        pred += std::complex<double>(row[t].real(), row[t].imag()) *
                g_[size_t(t)];
      }
      innov[size_t(k)] = m[size_t(k)] - pred;
    }
    // Solve S x = innov and S Y = (A P) for the gain application:
    // K = PA S^-1; g += PA (S^-1 innov); P -= PA S^-1 (PA)^H.
    // Factor S once via Gaussian elimination on augmented columns.
    std::vector<std::complex<double>> rhs(size_t(np) * (np + 1));
    for (int k = 0; k < np; ++k) {
      rhs[size_t(k) * (np + 1)] = innov[size_t(k)];
      // Identity for S^-1 (needed for the covariance update).
      for (int l = 0; l < np; ++l) {
        rhs[size_t(k) * (np + 1) + 1 + l] =
            (k == l) ? std::complex<double>(1, 0) : std::complex<double>(0, 0);
      }
    }
    // In-place solve of S * X = rhs (multi-RHS Gaussian elimination).
    {
      const int R = np, C = np + 1;
      for (int col = 0; col < R; ++col) {
        int piv = col;
        for (int r = col + 1; r < R; ++r) {
          if (std::abs(S[size_t(r) * R + col]) >
              std::abs(S[size_t(piv) * R + col])) {
            piv = r;
          }
        }
        for (int c2 = 0; c2 < R; ++c2) {
          std::swap(S[size_t(col) * R + c2], S[size_t(piv) * R + c2]);
        }
        for (int c2 = 0; c2 < C; ++c2) {
          std::swap(rhs[size_t(col) * C + c2], rhs[size_t(piv) * C + c2]);
        }
        std::complex<double> d = S[size_t(col) * R + col];
        if (std::abs(d) < 1e-15) continue;
        for (int c2 = col; c2 < R; ++c2) S[size_t(col) * R + c2] /= d;
        for (int c2 = 0; c2 < C; ++c2) rhs[size_t(col) * C + c2] /= d;
        for (int r = 0; r < R; ++r) {
          if (r == col) continue;
          std::complex<double> f = S[size_t(r) * R + col];
          if (std::abs(f) == 0.0) continue;
          for (int c2 = col; c2 < R; ++c2) {
            S[size_t(r) * R + c2] -= f * S[size_t(col) * R + c2];
          }
          for (int c2 = 0; c2 < C; ++c2) {
            rhs[size_t(r) * C + c2] -= f * rhs[size_t(col) * C + c2];
          }
        }
      }
    }
    // State update: g += PA * x  (x = column 0 of rhs).
    for (int t = 0; t < T; ++t) {
      std::complex<double> acc(0, 0);
      for (int k = 0; k < np; ++k) {
        acc += PA[size_t(t) * np + k] * rhs[size_t(k) * (np + 1)];
      }
      g_[size_t(t)] += acc;
    }
    // Covariance update: P -= PA * S^-1 * PA^H (Joseph-lite; adequate
    // with the diagonal floor below).
    std::vector<std::complex<double>> SinvPAH(size_t(np) * T);
    for (int k = 0; k < np; ++k) {
      for (int t = 0; t < T; ++t) {
        std::complex<double> acc(0, 0);
        for (int l = 0; l < np; ++l) {
          acc += rhs[size_t(k) * (np + 1) + 1 + l] *
                 std::conj(PA[size_t(t) * np + l]);
        }
        SinvPAH[size_t(k) * T + t] = acc;
      }
    }
    for (int t = 0; t < T; ++t) {
      for (int u = 0; u < T; ++u) {
        std::complex<double> acc(0, 0);
        for (int k = 0; k < np; ++k) {
          acc += PA[size_t(t) * np + k] * SinvPAH[size_t(k) * T + u];
        }
        P_[size_t(t) * T + u] -= acc;
      }
    }
    // Numerical hygiene: keep P Hermitian with a tiny diagonal floor.
    for (int t = 0; t < T; ++t) {
      for (int u = 0; u < t; ++u) {
        std::complex<double> avg =
            0.5 * (P_[size_t(t) * T + u] + std::conj(P_[size_t(u) * T + t]));
        P_[size_t(t) * T + u] = avg;
        P_[size_t(u) * T + t] = std::conj(avg);
      }
      double dg = P_[size_t(t) * T + t].real();
      P_[size_t(t) * T + t] = std::max(dg, 1e-9);
    }
  }

  // Project the state to per-carrier H and the mean posterior variance
  // (row P row^H averaged over carriers - the demapper's uncertainty).
  double hv = 0.0;
  for (int c = 0; c < n; ++c) {
    const cfloat* row = &basis_[size_t(c) * T];
    std::complex<double> acc(0, 0);
    for (int t = 0; t < T; ++t) {
      acc += std::complex<double>(row[t].real(), row[t].imag()) *
             g_[size_t(t)];
    }
    h_[size_t(c)] = cfloat(float(acc.real()), float(acc.imag()));
    // Diagonal-only projection of P (full row P row^H is O(T^2) per
    // carrier; the diagonal captures the bulk and the cost matters here).
    double v = 0.0;
    for (int t = 0; t < T; ++t) v += P_[size_t(t) * T + t].real();
    hv += v;
  }
  hVar_ = hv / std::max(n, 1);
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
