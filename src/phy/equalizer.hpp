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

  const std::vector<cfloat>& h() const { return h_; }
  double noiseVar() const { return noiseVar_; }
  double snrDb() const;             // mean per-carrier SNR estimate
  double cpePhase() const { return phase_; }

 private:
  Family family_;
  bool robust_;
  std::vector<cfloat> h_;           // per active carrier
  double noiseVar_;
  double phase_ = 0.0;              // CPE PLL state
  double freq_ = 0.0;               // rad/symbol
  double alphaH_;
  double g1_, g2_;                  // PLL gains
};

// Max-log LLRs for one despread source symbol.
//   z    : MRC combiner output, sum_k conj(H_k p_k) Y_k
//   a    : sum_k |H_k|^2 (combiner gain; z/a is the symbol estimate)
//   nvar : per-copy noise variance
// LLR convention: positive = bit 0 (matches LdpcCode::decode).
void demapLlr(Constellation c, cfloat z, float a, float nvar,
              std::vector<float>& llrOut);

}  // namespace skw
