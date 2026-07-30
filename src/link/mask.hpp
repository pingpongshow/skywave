// Carrier-mask detection (LINK_ADAPTATION.md "Carrier Masking"):
// per-carrier SNR quality weights; carriers persistently >= 12 dB below
// the band median for >= 5 s become mask candidates. Pure logic; the
// engine runs the MASK_UPDATE/ACK exchange.
#pragma once

#include <vector>

#include "phy/layout.hpp"

namespace skw {

struct MaskDetectorConfig {
  double belowMedianDb = 12.0;
  double persistSec = 5.0;
  double emaAlpha = 0.3;
  double maxMaskedRatio = 0.30;  // never propose beyond this
};

class MaskDetector {
 public:
  explicit MaskDetector(Family f, MaskDetectorConfig cfg = {})
      : family_(f), cfg_(cfg) {}

  // Per-frame per-carrier SNR snapshot (dB, tracker output).
  void onSnapshot(const std::vector<float>& carrierSnrDb, double now);

  // Current proposal: carriers excluded, capped at maxMaskedRatio
  // (worst offenders first). All-usable when nothing qualifies.
  CarrierMask proposal(double now) const;
  bool ratioExceeded(double now) const;  // > 30% qualify: AMC should demote
  void reset();

 private:
  Family family_;
  MaskDetectorConfig cfg_;
  std::vector<double> ema_;        // per-carrier SNR EMA (dB)
  std::vector<double> belowSince_; // -1 = currently healthy
  bool seeded_ = false;
};

}  // namespace skw
