// Zadoff-Chu fine-sync symbols (WAVEFORM.md "Fine Sync Symbols").
//
// A ZC sequence of length = family carrier count is mapped onto the active
// carriers of one OFDM symbol. The sync block transmits identical copies
// back-to-back (band profile zcSymbolsFast / zcSymbolsRobust: 3 / 6 on HF,
// 2 / 4 on VHF-FM). The receiver refines the chirp detector's coarse
// timing/CFO handoff via matched-filter timing and an ML frequency fit over
// the per-symbol matched-filter outputs, and seeds the equalizer's channel
// estimate.
//
// Sync symbols always use MEDIUM CP: they precede the PHY header, so the
// payload CP mode is not yet known to the receiver.
#pragma once

#include <complex>
#include <span>
#include <vector>

#include "core/constants.hpp"

namespace skw {

using cfloat = std::complex<float>;

// ZC symbols carry the profile's MEDIUM CP (HF: 48 baseband -> 368
// samples/symbol; VHF-FM: 24 -> 344).
inline int zcCpBaseband() {
  return cpSamplesAudio(CpMode::Medium) / kDecimation;
}
inline int zcSymbolSamples() { return kFftBaseband + zcCpBaseband(); }

// Frequency-domain ZC sequence, length = family carrier count.
std::vector<cfloat> zcFreqSequence(Family family);

// One time-domain ZC sync symbol at 12 kHz baseband (CP + core), unit RMS.
std::vector<cfloat> makeZcSymbol(Family family);

// nSymbols identical ZC symbols back-to-back (zcSymbols(robust)).
std::vector<cfloat> makeZcBlock(Family family, int nSymbols);

// Re-reference a per-carrier channel estimate (index 0 = the family's
// first carrier) to an FFT window `samples` later: the linear phase a
// timing shift puts across the carriers.
void applyTimingShift(Family family, std::span<cfloat> h, double samples);

struct ZcSyncResult {
  bool ok = false;
  double blockStart = 0.0;  // refined start of the ZC block within rx span
  double cfoHz = 0.0;       // fused CFO (prior x measurement, by variance)
  float quality = 0.0f;     // coherent-combining quality in [0, 1]
  // Coherent ML-fit power |sum_s m_s e^{-jws}|^2 at the fitted residual:
  // the metric receiveFrame compares across CFO branch hypotheses.
  double fitPower = 0.0;
  // Per-carrier noise variance estimated from the spread of the block's
  // per-symbol channel estimates (same normalization as `channel`).
  // Seeds the tracker so SHORT frames (ACKs, keepalives) report honest
  // SNR instead of the tracker's crude initial guess.
  double noiseVar = 0.0;
  // Per-active-carrier complex channel gain, averaged over the block's
  // symbols (index 0 = family's first active carrier).
  std::vector<cfloat> channel;
};

// Refine sync around a coarse block-start estimate (from the chirp stage,
// accurate to ~+-12 samples) with a prior CFO and its accuracy.
//
// CFO fusion: the ZC measurement's precision is energy-limited (NARROW's 13
// carriers cannot deliver < 0.5 Hz alone at deep SNR), while the chirp
// stage integrates its full band over 0.64 s and is often the better
// estimator. The result is the variance-weighted fusion of both; pass a
// large priorCfoSigmaHz to get the ZC-only measurement. A residual far
// outside the joint prior/measurement uncertainty (> 3 sigma) means the
// prior is on a wrong branch, and the prior is then dropped rather than
// fused (GAP_CLOSURE_PLAN.md R1).
//
// fitRangeHz bounds the residual frequency search around the prior.
ZcSyncResult zcRefine(Family family, std::span<const cfloat> rx,
                      double coarseStart, double priorCfoHz,
                      double priorCfoSigmaHz, int nSymbols,
                      int searchRadius = 24, double fitRangeHz = 3.0);

}  // namespace skw
