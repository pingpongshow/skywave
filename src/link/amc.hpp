// Adaptive mode controller (LINK_ADAPTATION.md "AMC Controller"): input
// aggregation, promote/demote with hysteresis and dwell, high-mode gates,
// fast-demote paths, DEGRADED flag, manual pinning with the fast-demote
// safety rule. Pure logic, injected time; the link layer feeds
// observations and executes the returned decisions via MODE_CHANGE (3.2).
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

#include "core/constants.hpp"
#include "link/chanclass.hpp"

namespace skw {

struct AmcConfig {
  // Highest mode allowed once the channel is classified SEVERE
  // (CCIR-Poor-like). Measured 2026-08-17: on Poor, MID closes only
  // M4-M6 and NARROW only M4-M7, while WIDE never closes at any mode.
  // The robust tier is the only thing that works, so promoting past it
  // is guaranteed waste.
  uint8_t severeMaxMode = 7;
  Family family = Family::Wide;
  uint8_t maxMode = 13;
  // Bottom rung: kFirstMode (M4) normally, 0 when the session
  // negotiated CAP_CSS_TIER (the ladder extends into the CSS deep tier).
  uint8_t minMode = kFirstMode;
  double promoteMarginDb = 2.5;
  double demoteMarginDb = 0.5;
  double snrEmaAlpha = 0.25;
  double remoteFreshSec = 60.0;
  double staleRemotePenaltyDb = 1.5;
  double minDwellSec = 6.0;
  int promoteConsecutive = 2;
  double promoteRetryMax = 0.15;
  double demoteRetryMin = 0.35;
  double fastDemoteRetryMin = 0.50;
  int demoteFailStreak = 3;
  // High-mode gates.
  double m11RetryMax = 0.10;
  // Pilot-residual energy relative to channel power (phy/receiver.hpp
  // RxFrameResult::pilotStability). CALIBRATED 2026-08-17 at M10 WIDE,
  // 12 frames per point:
  //     AWGN 25 dB 0.003   AWGN 15 dB 0.025
  //     Good 25 dB 0.010   Good 15 dB 0.057 (worst frame 0.156)
  //     Mod  25 dB 0.381   Mod  15 dB 0.545
  //     Poor 25 dB 1.766   Poor 15 dB 1.830
  // M11+ wants a well-tracked channel: admit Good, refuse Moderate. The
  // old 0.15 predated any implementation of this measurement (nothing
  // fed the gate, so it evaluated a constant 0) and sits INSIDE Good's
  // worst-frame spread, which cost the M3 ramp 3 points when the gate
  // first went live.
  double pilotStabilityMax = 0.25;
  uint8_t selectivityMax = 96;       // M12+ gate (0..255)
  // kFadingNA rows (near-static-only modes): require a clean probe and
  // this margin above the AWGN reference floor.
  double naModeAwgnMarginDb = 6.0;
};

struct AmcDecision {
  enum class Kind : uint8_t { None, Promote, Demote, FastDemote };
  Kind kind = Kind::None;
  uint8_t targetMode = 4;
};

class Amc {
 public:
  Amc(AmcConfig cfg, uint8_t initialMode, double now);

  // --- observations ----------------------------------------------------
  void onLocalSnr(double snrDb);            // post-equalizer, per frame
  void onRemoteSnr(double snrDb, double now);
  void onFrameResult(bool wasRetransmit, bool decodeOk);  // 16-frame window
  void onQualityIndicators(uint8_t selectivity, double pilotStability);
  // Channel character (link/chanclass.hpp). STATIC switches the floor
  // table to the AWGN reference - the fading floors leave large
  // throughput unused on a static path; SEVERE caps the ladder at the
  // robust tier. UNKNOWN/FADING keep the fading floors, which is the
  // long-standing behaviour.
  void setChannelClass(ChannelClass c) { chanClass_ = c; }
  ChannelClass channelClass() const { return chanClass_; }
  void onProbeResult(bool clean, double now);  // 3.6 feeds this
  void onModeApplied(uint8_t mode, double now);  // MODE_CHANGE committed

  // --- control ---------------------------------------------------------
  void pin(uint8_t mode, double now);  // manual override (fast demote stays)
  void unpin();
  bool pinned() const { return pinned_; }

  // --- evaluation ------------------------------------------------------
  AmcDecision evaluate(double now);
  // The next rung is promotable EXCEPT for the clean-probe requirement
  // (kFadingNA / fragile modes): the engine should run a probe exchange
  // toward the returned mode. nullopt otherwise. Without this hook the
  // NA-mode gate is a chicken-and-egg: evaluate() can never return
  // Promote to trigger the probe that would make it eligible.
  std::optional<uint8_t> probeWanted(double now) const;
  // Advisor assist hook (LINK_ADAPTATION.md): a single-step bias
  // consumed by the NEXT evaluate() only. +1 credits one promote
  // streak evaluation; -1 widens the demote margin by 0.5 dB for that
  // evaluation. Never moves more than the ladder's normal single step.
  void applyAdvisorBias(int bias) { advisorBias_ = std::clamp(bias, -1, 1); }
  double effectiveSnrDb(double now) const;
  double retryRatio() const;
  bool degraded() const { return degraded_; }
  uint8_t mode() const { return mode_; }
  const AmcConfig& config() const { return cfg_; }

 private:
  bool modeEligible(uint8_t k, double effSnr,
                    bool assumeProbeClean = false) const;
  double operatingFloorDb(uint8_t k) const;
  uint8_t demoteTarget(int steps) const;  // skips kFadingNA rungs

  AmcConfig cfg_;
  uint8_t mode_;
  double lastDemoteAt_ = -1e9;  // promote damping after demotes
  bool pinned_ = false;
  bool degraded_ = false;
  double lastChange_;
  double localSnr_ = 0.0;
  bool haveLocal_ = false;
  double remoteSnr_ = 0.0;
  double remoteTime_ = -1e9;
  uint8_t selectivity_ = 0;
  ChannelClass chanClass_ = ChannelClass::Unknown;
  double pilotStability_ = 0.0;
  bool probeClean_ = false;
  int advisorBias_ = 0;  // one-shot, consumed by evaluate()
  int failStreak_ = 0;
  // Sliding 16-frame result window (bit i: retransmit flag).
  uint32_t retryBits_ = 0;
  int windowFill_ = 0;
  int promoteStreak_ = 0;
};

}  // namespace skw
