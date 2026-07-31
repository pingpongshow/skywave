#include "link/amc.hpp"

#include <algorithm>

namespace skw {

Amc::Amc(AmcConfig cfg, uint8_t initialMode, double now)
    : cfg_(cfg), mode_(initialMode), lastChange_(now) {}

void Amc::onLocalSnr(double snrDb) {
  if (!haveLocal_) {
    localSnr_ = snrDb;
    haveLocal_ = true;
  } else {
    localSnr_ += cfg_.snrEmaAlpha * (snrDb - localSnr_);
  }
}

void Amc::onRemoteSnr(double snrDb, double now) {
  remoteSnr_ = snrDb;
  remoteTime_ = now;
}

void Amc::onFrameResult(bool wasRetransmit, bool decodeOk) {
  retryBits_ = uint32_t((retryBits_ << 1) | (wasRetransmit ? 1u : 0u)) &
               0xFFFFu;
  windowFill_ = std::min(windowFill_ + 1, 16);
  failStreak_ = decodeOk ? 0 : failStreak_ + 1;
}

void Amc::onQualityIndicators(uint8_t selectivity, double pilotStability) {
  selectivity_ = selectivity;
  pilotStability_ = pilotStability;
}

void Amc::onProbeResult(bool clean, double /*now*/) { probeClean_ = clean; }

void Amc::onModeApplied(uint8_t mode, double now) {
  mode_ = mode;
  lastChange_ = now;
  promoteStreak_ = 0;
  failStreak_ = 0;
  probeClean_ = false;  // a probe clears exactly one promotion
}

void Amc::pin(uint8_t mode, double now) {
  pinned_ = true;
  onModeApplied(mode, now);
}

void Amc::unpin() { pinned_ = false; }

double Amc::retryRatio() const {
  if (windowFill_ == 0) return 0.0;
  int set = 0;
  for (int i = 0; i < windowFill_; ++i) {
    if (retryBits_ & (1u << i)) ++set;
  }
  return double(set) / windowFill_;
}

double Amc::effectiveSnrDb(double now) const {
  if (!haveLocal_) return -99.0;
  if (now - remoteTime_ <= cfg_.remoteFreshSec) {
    return std::min(localSnr_, remoteSnr_);
  }
  return localSnr_ - cfg_.staleRemotePenaltyDb;
}

double Amc::operatingFloorDb(uint8_t k) const {
  return fadingFloorDb(cfg_.family, int(k));
}

// Next rung DOWN that can actually operate on a fading channel:
// kFadingNA rungs (static-only, probe-gated on the way up) are useless
// as a refuge when the channel is collapsing - demoting INTO one
// parked the session on a dead mode (observed: WIDE M4 -> M3(NA) at
// floor SNR, zero goodput forever).
uint8_t Amc::demoteTarget(int steps) const {
  int m = int(mode_);
  int landed = m;
  while (steps > 0 && m > int(cfg_.minMode)) {
    --m;
    if (operatingFloorDb(uint8_t(m)) < 90.0) {
      landed = m;
      --steps;
    }
  }
  return uint8_t(landed);
}

bool Amc::modeEligible(uint8_t k, double effSnr,
                       bool assumeProbeClean) const {
  const bool probeOk = probeClean_ || assumeProbeClean;
  const double floor = operatingFloorDb(k);
  if (floor >= 90.0) {
    // Near-static-only mode (kFadingNA): clean probe + AWGN-floor margin.
    if (!probeOk) return false;
    if (effSnr < floorDb(cfg_.family, int(k)) + cfg_.naModeAwgnMarginDb) {
      return false;
    }
  } else if (effSnr < floor + cfg_.promoteMarginDb) {
    return false;
  }
  const double retry = retryRatio();
  if (k >= 11 &&
      (retry > cfg_.m11RetryMax || pilotStability_ > cfg_.pilotStabilityMax)) {
    return false;
  }
  if (k >= 12 && selectivity_ > cfg_.selectivityMax) return false;
  if (k >= 13 && !probeOk) return false;
  return true;
}

std::optional<uint8_t> Amc::probeWanted(double now) const {
  if (pinned_ || mode_ >= cfg_.maxMode) return std::nullopt;
  if (now - lastChange_ < cfg_.minDwellSec) return std::nullopt;
  if (retryRatio() > cfg_.promoteRetryMax) return std::nullopt;
  uint8_t next = uint8_t(mode_ + 1);
  const double eff = effectiveSnrDb(now);
  // Skip rungs a probe cannot fix (kFadingNA on a channel where even a
  // clean probe leaves them ineligible) - mirror the promote ladder.
  while (next < cfg_.maxMode &&
         !modeEligible(next, eff, /*assumeProbeClean=*/true)) {
    ++next;
  }
  if (modeEligible(next, eff)) return std::nullopt;  // no probe needed
  if (!modeEligible(next, eff, /*assumeProbeClean=*/true)) {
    return std::nullopt;  // blocked on something a probe cannot fix
  }
  return next;
}

AmcDecision Amc::evaluate(double now) {
  const int bias = advisorBias_;  // one-shot advisor influence
  advisorBias_ = 0;
  const double eff = effectiveSnrDb(now);
  const double retry = retryRatio();
  degraded_ = false;
  // Adaptation cadence scales with the mode's AIRTIME class: at CSS
  // modes a single frame runs tens of seconds, so wall-clock dwells
  // tuned for OFDM (6 s / 15 s) re-decide every frame and the
  // MODE_CHANGE exchanges themselves consume the channel (measured:
  // deep-tier goodput 0.83 B/s with a mode change every ~20 s).
  const double cadence = mode_ <= 3 ? 8.0 : 1.0;
  const double dwellSec = cfg_.minDwellSec * cadence;
  const double promoteHoldSec = 15.0 * cadence;

  // Fast demote: applies even when pinned (safety), min dwell waived.
  if (windowFill_ >= 8 && retry > cfg_.fastDemoteRetryMin) {
    uint8_t target = demoteTarget(2);
    if (target != mode_) {
      promoteStreak_ = 0;
      return {AmcDecision::Kind::FastDemote, target};
    }
    degraded_ = true;
    return {};
  }
  if (pinned_) return {};

  // Demote: SNR under the demote margin, retry ratio, or failure streak.
  const bool floorBreak =
      operatingFloorDb(mode_) < 90.0 &&
      eff < operatingFloorDb(mode_) + cfg_.demoteMarginDb +
                (bias < 0 ? 0.5 : 0.0);
  const bool retryBreak = windowFill_ >= 8 && retry > cfg_.demoteRetryMin;
  const bool failBreak = failStreak_ >= cfg_.demoteFailStreak;
  if (floorBreak || retryBreak || failBreak) {
    promoteStreak_ = 0;
    if (uint8_t t = demoteTarget(1); t != mode_) {
      lastDemoteAt_ = now;
      return {AmcDecision::Kind::Demote, t};
    }
    degraded_ = true;  // at the bottom usable rung and still failing
    return {};
  }

  // Promote: single step, dwell, retry gate, consecutive evaluations.
  // After any demote, promotion holds for a 15 s damping window: a
  // fade hovering at a floor boundary otherwise ping-pongs
  // demote/promote every dwell, and the MODE_CHANGE/MODE_ACK exchanges
  // themselves consume the channel (observed under bidirectional
  // fading: a mode flap every ~6 s with data starved behind it).
  // 15 s ~ 2.5 dwells: enough to break the flap loop while keeping the
  // M3 ramp-tracking gate (>= 0.75 of genie throughput) intact.
  if (mode_ >= cfg_.maxMode || now - lastChange_ < dwellSec ||
      now - lastDemoteAt_ < promoteHoldSec ||
      retry > cfg_.promoteRetryMax) {
    promoteStreak_ = 0;
    return {};
  }
  // The next rung UP skips kFadingNA (static-only) rungs, mirroring
  // demoteTarget: from CSS M1 the literal next rung (M2, NA on the
  // wide-chip families) is permanently ineligible on a fading channel,
  // and a single-step-only ladder could never climb back out of the
  // deep tier after a collapse - even when conditions fully recovered.
  uint8_t next = uint8_t(mode_ + 1);
  while (next < cfg_.maxMode && operatingFloorDb(next) >= 90.0 &&
         !modeEligible(next, eff)) {
    ++next;
  }
  if (!modeEligible(next, eff)) {
    promoteStreak_ = 0;
    return {};
  }
  if (promoteStreak_ + (bias > 0 ? 1 : 0) + 1 < cfg_.promoteConsecutive) {
    ++promoteStreak_;
    return {};
  }
  promoteStreak_ = 0;
  return {AmcDecision::Kind::Promote, next};
}

}  // namespace skw
