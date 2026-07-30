// Channel tracking, equalization support, and soft demapping (DSP.md RX
// chain: pilot processor -> channel estimator -> MMSE equalizer ->
// despreader -> soft demapper).
#pragma once

#include <complex>
#include <span>
#include <vector>

#include "core/constants.hpp"
#include "phy/layout.hpp"
#include "phy/mapper.hpp"

namespace skw {

// Per-carrier channel tracker for one frame. Seeded by the ZC estimate,
// updated every symbol from scattered pilots:
//  - common phase error (CPE) tracked by a small PLL (pilot innovations),
//  - per-carrier H: pilot measurements interpolated in frequency, blended
//    in time with a mode-class-dependent IIR (DSP.md),
//  - noise variance from pilot residuals,
//  - FLYWHEEL: if a symbol's pilot residual explodes, hold H and keep the
//    PLL running on prediction only.
class ChannelTracker {
 public:
  // robustClass selects slow/deep smoothing (spread modes M4-M7 + robust
  // headers) vs fast tracking (M8+).
  ChannelTracker(Family family, bool robustClass, std::vector<cfloat> h0,
                 double noiseVar0);

  // Derotate the symbol's bins by the tracked CPE, then update H and the
  // noise estimate from this symbol's pilots. Call once per symbol in
  // order; bins is the full 320-bin FFT output (modified in place).
  void processSymbol(std::span<cfloat> bins, int symIdx);

  // Session carrier mask (payload symbols): masked pilots are skipped -
  // no measurement is taken; the tap-domain model interpolates.
  void setMask(const CarrierMask* mask) { mask_ = mask; }

  const std::vector<cfloat>& h() const { return h_; }
  // Effective noise for LLR scaling: measurement noise plus the filter's
  // mean per-carrier posterior variance. When the estimate is genuinely
  // uncertain (mid-fade, sparse pilots) the demapper must not emit
  // confidently wrong LLRs from a mis-rotated H.
  double noiseVar() const { return noiseVar_ + hVar_; }
  double snrDb() const;             // mean per-carrier SNR estimate
  // Per-carrier pilot-residual EMA: interference-aware quality (a tone
  // INFLATES |H| but explodes the residual; carrier quality must come
  // from residuals, not |H|^2 - LINK_ADAPTATION.md mask detection).
  const std::vector<double>& carrierResid() const { return residEma_; }
  double cpePhase() const {
    return std::atan2(lastGain_.imag(), lastGain_.real());
  }

 private:
  Family family_;
  bool robust_;
  std::vector<cfloat> h_;           // per active carrier (= basis * taps)
  cfloat lastGain_{1.0f, 0.0f};     // raw per-symbol common-gain fit (diag)
  double noiseVar_;
  double phase_ = 0.0;              // retained for the cpePhase() accessor
  int nvarUpdates_ = 0;             // symbols folded into noiseVar_

  // Per-tap Gauss-Markov Kalman filter: the channel is T taps on a
  // physical delay grid, each tap an independent Rayleigh process with
  // known Doppler (g[k+1] = rho g[k] + w). Each symbol's pilots form the
  // measurement update. This handles fading DYNAMICS correctly - the
  // former sliding-window LS assumed a quasi-static shape and collapsed
  // mid-fade when tap-relative phases rotated within the window.
  const CarrierMask* mask_ = nullptr;
  std::vector<double> residEma_;    // per carrier; <0 = never visited
  int numTaps_ = 0;
  double rho_ = 0.999;              // per-symbol tap correlation
  std::vector<std::complex<double>> g_;      // tap state (T)
  std::vector<std::complex<double>> P_;      // state covariance (T x T)
  std::vector<double> qDiag_;                // process noise per tap (T)
  double hVar_ = 0.0;               // mean per-carrier posterior variance
  std::vector<cfloat> basis_;       // (n x T): row c = e^{-j2pi c d_t / N}

  static void solveHermitian(std::vector<std::complex<double>>& M,
                             std::vector<std::complex<double>>& b, int T);
};

// Max-log LLRs for one despread source symbol.
//   z    : MRC combiner output, sum_k conj(H_k p_k) Y_k
//   a    : sum_k |H_k|^2 (combiner gain; z/a is the symbol estimate)
//   nvar : per-copy noise variance
// LLR convention: positive = bit 0 (matches LdpcCode::decode).
void demapLlr(Constellation c, cfloat z, float a, float nvar,
              std::vector<float>& llrOut);

}  // namespace skw
