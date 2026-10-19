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
//  - the channel is a set of taps on a delay grid, each a Gauss-Markov
//    process tracked by a Kalman filter (mode-class-dependent Doppler
//    design); per-carrier H is its projection, so common phase and
//    frequency selectivity are tracked together (no separate CPE loop),
//  - noise variance from pilot innovations,
//  - FLYWHEEL: if a symbol's pilot residual explodes, hold the state and
//    run on prediction only,
//  - optional RTS smoothing over a buffered span (beginHistory).
class ChannelTracker {
 public:
  // robustClass selects slow/deep smoothing (spread modes M4-M7 + robust
  // headers) vs fast tracking (M8+).
  //
  // meanChanPow (> 0): the frame-average channel power, used to scale the
  // process noise instead of the power at ZC time - a frame that starts in
  // a fade otherwise gets Q 10-20 dB too small and cannot follow the
  // recovery (GAP_CLOSURE_PLAN.md R5). <= 0 keeps the ZC-time scaling.
  ChannelTracker(Family family, bool robustClass, std::vector<cfloat> h0,
                 double noiseVar0, double meanChanPow = -1.0);

  // Derotate the symbol's bins by the tracked CPE, then update H and the
  // noise estimate from this symbol's pilots. Call once per symbol in
  // order; bins is the full 320-bin FFT output (modified in place).
  void processSymbol(std::span<cfloat> bins, int symIdx);

  // Session carrier mask (payload symbols): masked pilots are skipped -
  // no measurement is taken; the tap-domain model interpolates.
  void setMask(const CarrierMask* mask) { mask_ = mask; }

  const std::vector<cfloat>& h() const { return h_; }
  bool robustClass() const { return robust_; }

  // Fixed-interval RTS smoothing (GAP_CLOSURE_PLAN.md R6). Between
  // beginHistory() and smoothHistory() every processSymbol() records the
  // filtered tap state; smoothHistory() then runs the backward pass and
  // returns, per recorded symbol, the smoothed per-carrier H and the
  // matching LLR noise (measurement noise + smoothed posterior variance,
  // the noiseVar() analogue). The forward state is left as filtered, so
  // tracking continues from the end of the span.
  void beginHistory();
  void smoothHistory(std::vector<std::vector<cfloat>>& hOut,
                     std::vector<double>& nvOut);
  // Effective noise for LLR scaling: measurement noise plus the filter's
  // mean per-carrier posterior variance. When the estimate is genuinely
  // uncertain (mid-fade, sparse pilots) the demapper must not emit
  // confidently wrong LLRs from a mis-rotated H.
  double noiseVar() const { return noiseVar_ + hVar_; }
  double snrDb() const;             // mean per-carrier SNR estimate
  // Symbols folded into the noise estimate. Below ~8 the figure is still
  // mostly the seed guess (the EMA runs at 0.4 for the first 8, 0.1
  // after): callers steering on snrDb() should not trust shorter frames.
  int noiseUpdates() const { return nvarUpdates_; }
  // Per-carrier pilot-residual EMA: interference-aware quality (a tone
  // INFLATES |H| but explodes the residual; carrier quality must come
  // from residuals, not |H|^2 - LINK_ADAPTATION.md mask detection).
  const std::vector<double>& carrierResid() const { return residEma_; }
  // Delay-spread estimate (ms) from the tap state: extent of taps above
  // 10% of the peak power. Drives CPMODE AUTO.
  double delaySpreadMs() const;

 private:
  Family family_;
  bool robust_;
  std::vector<cfloat> h_;           // per active carrier (= basis * taps)
  cfloat lastGain_{1.0f, 0.0f};     // raw per-symbol common-gain fit (debug)
  double noiseVar_;
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
  int k0_ = 2;  // grid taps ahead of the window (trackerPreTaps())
  // RTS history: filtered state per recorded symbol, whether that symbol
  // ran the predict step (pilot-less symbols do not), and its R.
  struct HistEntry {
    std::vector<std::complex<double>> g, P;
    bool predicted;
    double r;
  };
  bool recording_ = false;
  std::vector<HistEntry> hist_;
  void projectH(const std::vector<std::complex<double>>& g,
                const std::vector<std::complex<double>>& P,
                std::vector<cfloat>& h, double& hv) const;
  double rho_ = 0.999;              // per-symbol tap correlation
  std::vector<std::complex<double>> g_;      // tap state (T)
  std::vector<std::complex<double>> P_;      // state covariance (T x T)
  std::vector<double> qDiag_;                // process noise per tap (T)
  double hVar_ = 0.0;               // mean per-carrier posterior variance
  std::vector<cfloat> basis_;       // (n x T): row c = e^{-j2pi c d_t / N}

  static void solveLinear(std::vector<std::complex<double>>& M,
                          std::vector<std::complex<double>>& B, int T,
                          int nrhs, double tiny);
};

// Max-log LLRs for one despread source symbol.
//   z    : MRC combiner output, sum_k conj(H_k p_k) Y_k
//   a    : sum_k |H_k|^2 (combiner gain; z/a is the symbol estimate)
//   nvar : per-copy noise variance
// LLR convention: positive = bit 0 (matches LdpcCode::decode).
void demapLlr(Constellation c, cfloat z, float a, float nvar,
              std::vector<float>& llrOut);

}  // namespace skw
