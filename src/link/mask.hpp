// Carrier-mask detection (LINK_ADAPTATION.md "Carrier Masking"):
// per-carrier SNR quality weights; carriers persistently >= 12 dB below
// the band median for >= 5 s become mask candidates. Pure logic; the
// engine runs the MASK_UPDATE/ACK exchange.
#pragma once

#include <algorithm>
#include <vector>

#include "phy/layout.hpp"

namespace skw {

struct MaskDetectorConfig {
  double belowMedianDb = 12.0;
  double persistSec = 5.0;
  double emaAlpha = 0.3;
  double maxMaskedRatio = 0.30;  // never propose beyond this
  // Most carriers a mask may exclude: what the detector proposes at most,
  // and what a peer's MASK_UPDATE is refused above.
  int maxMasked(int carriers) const {
    return int(maxMaskedRatio * carriers);
  }
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

  // Advisor assist hook (LINK_ADAPTATION.md): bounded threshold bias on
  // top of the configured belowMedianDb; clamped to +-1.5 dB.
  void setThresholdBiasDb(double d) {
    thresholdBiasDb_ = std::clamp(d, -1.5, 1.5);
  }
  double effectiveThresholdDb() const {
    return cfg_.belowMedianDb + thresholdBiasDb_;
  }

 private:
  Family family_;
  MaskDetectorConfig cfg_;
  double thresholdBiasDb_ = 0.0;   // advisor assist, clamped
  std::vector<double> ema_;        // per-carrier SNR EMA (dB)
  std::vector<double> belowSince_; // -1 = currently healthy
  bool seeded_ = false;
};

}  // namespace skw
