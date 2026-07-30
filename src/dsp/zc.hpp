// Zadoff-Chu fine-sync symbols (WAVEFORM.md "Fine Sync Symbols").
//
// A ZC sequence of length = family carrier count is mapped onto the active
// carriers of one OFDM symbol. The sync block transmits identical copies
// back-to-back (fast class: 2, robust class: 4). The receiver refines the
// chirp detector's coarse timing/CFO handoff via matched-filter timing and
// Schmidl-Cox-style pair-phase CFO, and seeds the equalizer's channel
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

// nSymbols identical ZC symbols back-to-back (2 = fast, 4 = robust).
std::vector<cfloat> makeZcBlock(Family family, int nSymbols);

struct ZcSyncResult {
  bool ok = false;
  double blockStart = 0.0;  // refined start of the ZC block within rx span
  double cfoHz = 0.0;       // fused CFO (prior x measurement, by variance)
  double cfoSigmaHz = 0.0;  // estimated std dev of cfoHz
  float quality = 0.0f;     // coherent-combining quality in [0, 1]
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
// large priorCfoSigmaHz to get the ZC-only measurement.
ZcSyncResult zcRefine(Family family, std::span<const cfloat> rx,
                      double coarseStart, double priorCfoHz,
                      double priorCfoSigmaHz, int nSymbols,
                      int searchRadius = 24);

}  // namespace skw
