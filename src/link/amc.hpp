// Adaptive mode controller (LINK_ADAPTATION.md "AMC Controller"): input
// aggregation, promote/demote with hysteresis and dwell, high-mode gates,
// fast-demote paths, failure memory, manual pinning with the fast-demote
// safety rule. Pure logic, injected time; the link layer feeds
// observations and executes the returned decisions via MODE_CHANGE (3.2).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

#include "core/constants.hpp"
#include "link/chanclass.hpp"

namespace skw {

struct AmcConfig {
  // (Channel classes select MEASURED floor tables - constants.hpp
  // kModerateFloors / kPoorFloors, Amc::operatingFloorDb - where
  // kFadingNA marks the modes that never close on that channel. Until
  // v1.2.0 they acted through hard per-class mode caps instead.)
  Family family = Family::Wide;
  uint8_t maxMode = 13;
  // Bottom rung: kFirstMode (M4) normally, 0 when the session
  // negotiated CAP_CSS_TIER (the ladder extends into the CSS deep tier).
  uint8_t minMode = kFirstMode;
  // Margins are relative to the FLOOR tables, which are one-shot
  // 10-15% FER points in the 3 kHz-noise convention. NEGATIVE on
  // purpose: with chase/IR HARQ and selective-repeat ARQ the
  // throughput-optimal operating point of a rung is ~1.5 dB BELOW its
  // one-shot floor (a rung is worth ~1.5x the one under it, so it pays
  // until FER ~ 33%). These values reproduce the operating point the
  // M3 ramp gate was tuned at (0.94 of genie on CCIR Good, 0 drops)
  // once the estimator was calibrated to the table convention
  // (constants.hpp kOfdmSnrCalDb: the old +2.5 / +0.5 margins were read
  // against an estimate ~4 dB optimistic on WIDE, i.e. they WERE
  // -1.5 / -3.5 in set-point terms; MID and NARROW, whose estimates
  // were 8 and 11 dB optimistic, ran far more aggressively than
  // designed and are now on the same operating point as WIDE).
  double promoteMarginDb = -1.5;
  // Promote margin INTO the top rungs (k >= promoteHighFrom). Their FER
  // curves are steep and their frames carry the most bytes, so a failed
  // promotion there costs a whole segment (2026-10-12 ramp trace: M12 at
  // 20-22 dB on CCIR Good where the oracle holds M11, the worst segment
  // 6 KB vs 21 KB) - while the -1.5 dB margin still pays on the lower
  // rungs. Swept -1.5/-0.5/+0.5/+1.5: +0.5 best (with the classifier's
  // spread median: M3 ramp 0.93, churn +5..+8%, coupled churn +17%);
  // +1.5 cost churn 6%. NaN = same as promoteMarginDb.
  double promoteMarginHighDb = 0.5;
  uint8_t promoteHighFrom = 11;
  double demoteMarginDb = -3.5;
  double snrEmaAlpha = 0.25;
  double remoteFreshSec = 60.0;
  double staleRemotePenaltyDb = 1.5;
  double minDwellSec = 6.0;
  int promoteConsecutive = 2;
  double promoteRetryMax = 0.15;
  double demoteRetryMin = 0.35;
  double fastDemoteRetryMin = 0.50;
  int demoteFailStreak = 3;
  // FAILURE MEMORY ("penalty box"). After a demotion driven by frame
  // FAILURES (retry ratio, fail streak, fast demote) rather than by
  // SNR, promotion back into the failed rung or above holds for a
  // window that DOUBLES on every repeat, unless the effective SNR has
  // risen by failRecoverDb since the failure. SNR alone is not evidence
  // that a rung works: the fast tier (M8+) fails 30-60% on a
  // CCIR-Moderate channel at ANY SNR (bertool, by design - its floors
  // are measured on Good), and an SNR-only ladder re-tried it every
  // promote-hold, flapping M6->M7->M8->M7->M6 inside a minute with the
  // MODE_CHANGE exchanges and the failed frames eating the turn.
  // 6 dB, not 3: on CCIR Moderate the estimate swings +-3 dB between
  // frames at the fast tier (motion counted as noise), and a 3 dB
  // override unlocked the box on noise alone - measured as M9 <-> M10
  // every ~40 s on a 23-26 dB Moderate channel, never learning.
  double failRecoverDb = 6.0;
  // First failure holds 30 s, doubling to 240 s: a 15 s first hold was
  // shorter than one promote-fail cycle at the fast tier (~40 s), so
  // the repeat counter never advanced past one.
  int failHoldMaxDoublings = 4;  // 30 s -> 240 s (x airtime cadence)
  // PROMOTION STEP: always one rung. A two-rung step (only into a rung
  // clearing its floor by a further 3 dB, clean retry window) was
  // MEASURED AND REJECTED 2026-09-07: M3 ramp 0.87 -> 0.89, but coupled
  // churn 196,667 -> 122,931 B (-17% of all bytes moved over seeds
  // 11-13). The ramp is a tracking metric on a monotone sweep, where
  // skipping a rung is free; on a fading channel the skipped rung is the
  // evidence. The climb rate stays one MODE_CHANGE round trip per rung.
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
  // 16-frame sliding window. `now` is instrumentation only (window span);
  // decisions are unaffected by it.
  void onFrameResult(bool wasRetransmit, bool decodeOk, double now = -1.0);
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
  uint8_t mode() const { return mode_; }
  double pilotStability() const { return pilotStability_; }
  uint8_t selectivity() const { return selectivity_; }
  // Smoothed local SNR (the EMA the controller steers on) - also what
  // the ACK reports to the peer: a per-frame figure could hold a
  // fade-edge reading for a minute once only converged frames fed it.
  bool haveLocalSnr() const { return haveLocal_; }
  double localSnrDb() const { return localSnr_; }
  int windowFill() const { return windowFill_; }
  // Wall-clock span of the evidence the retry ratio is computed over:
  // the retry window is 16 FRAMES, so at long frames it describes a
  // stretch of channel that may be minutes old. -1 when unknown.
  double windowSpanSec(double now) const {
    return oldestSampleAt_ < 0.0 ? -1.0 : now - oldestSampleAt_;
  }
  // Failure memory (tests / debug): the lowest rung whose last attempt
  // failed on frames, 255 when clear; and the hold still in force.
  uint8_t failedMode() const { return failMode_; }
  double failHoldSec(double now) const;

 private:
  bool modeEligible(uint8_t k, double effSnr,
                    bool assumeProbeClean = false) const;
  double operatingFloorDb(uint8_t k) const;
  // Ladder top: cfg_.maxMode bounded to the mode table. A max_mode of
  // "auto" once arrived here as 255 and every upward walk indexed past
  // the floor tables at M13.
  uint8_t topMode() const {
    return std::min<uint8_t>(cfg_.maxMode, uint8_t(kNumModes - 1));
  }
  uint8_t demoteTarget(int steps) const;  // skips kFadingNA rungs
  double promoteMarginFor(uint8_t k) const;
  void noteFailure(double now, double effSnr);
  double cadence() const { return mode_ <= 3 ? 8.0 : 1.0; }

  AmcConfig cfg_;
  uint8_t mode_;
  double lastDemoteAt_ = -1e9;  // promote damping after demotes
  // Failure memory (see AmcConfig::failRecoverDb).
  uint8_t failMode_ = 255;
  double failSnr_ = -99.0;
  double failAt_ = -1e9;
  int failRepeats_ = 0;
  bool pinned_ = false;
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
  // Arrival times of the samples in the window (instrumentation).
  std::array<double, 16> sampleAt_{};
  int sampleHead_ = 0;
  double oldestSampleAt_ = -1.0;
  int promoteStreak_ = 0;
};

}  // namespace skw
