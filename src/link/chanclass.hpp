// Channel-character classifier (LINK_ADAPTATION.md).
//
// (v1.2.0: the history below describes the v1.1.0 receiver. The classes
// now select per-class floor tables - AMC operatingFloorDb - and the
// pilot thresholds were re-measured on the new tracker.)
//
// The AMC has always operated against ONE floor table - the measured
// fading floors - because that is the safe assumption. Two costs were
// measured and logged against that choice:
//
//  * on a genuinely STATIC channel the fading floors leave large
//    throughput unused: an AWGN ramp delivered 39% of an AWGN-floor
//    oracle (ROADMAP 3.11 finding);
//  * on a SEVERE channel (CCIR Poor: 1 Hz Doppler, 2 ms spread) the
//    fading floors are far too optimistic. Measured 2026-08-17: WIDE
//    never closes at ANY mode (best 28% FER at M4 regardless of SNR),
//    MID closes only M4-M6, NARROW only M4-M7. Promoting into M8+ on
//    such a channel is guaranteed waste.
//
// Both are the same missing input: what KIND of channel is this? The
// classifier answers from evidence the receiver already produces - no
// new measurements on air, no wire format, no negotiation.
#pragma once

#include <array>

#include "core/constants.hpp"
#include <cstdint>

namespace skw {

enum class ChannelClass : uint8_t {
  Unknown = 0,  // not enough evidence yet - callers use fading floors
  Static,       // AWGN-like: stable pilots, negligible spread, SNR steady
  Fading,       // ordinary ionospheric fading (the design assumption)
  Severe,       // CCIR-Poor-like: fast Doppler and/or deep spread
  Moderate,     // CCIR-Moderate-like Doppler: the fast tier (M8+) fails
                // 25-75% at ANY SNR (WAVEFORM fading annex - its floors
                // are measured on Good), so the ladder is capped at M8
};

struct ChanClassConfig {
  // Evidence needed before leaving Unknown. Frames are seconds apart at
  // robust modes, so this is tens of seconds of observation.
  int minSamples = 12;

  // STATIC gates - all must hold. Deliberately strict: a wrong Static
  // verdict promotes against optimistic floors and costs a session,
  // whereas a wrong Fading verdict only costs throughput.
  double staticSnrVarMax = 1.5;      // dB^2 over the window
  double staticSpreadMsMax = 0.35;   // delay spread
  // Pilot residual alone must rule out ordinary fading: measured Good
  // sits at 0.010-0.057 while AWGN is 0.003-0.025 (see AmcConfig).
  double staticPilotStabMax = 0.03;
  double staticFerMax = 0.02;

  // SEVERE gates - any two of the four (with severePilotStabMin below).
  double severeSnrVarMin = 12.0;
  double severeSpreadMsMin = 1.6;
  double severeFerMin = 0.35;
  // Pilot excess on CCIR Poor vs Moderate, per family (index = Family).
  // RE-MEASURED 2026-10 on the v1.2.0 tracker (smoothed, multipath
  // margin) - bertool `pilot` column, decoded frames 3-10 dB above each
  // mode's floor on that channel, median over M4-M13:
  //            WIDE   MID    NARROW ULTRA
  //   Moderate 0.018  0.021  0.059  0.017
  //   Poor     0.095  0.142  0.263  0.097
  // Threshold = geometric mean of the two. (The former 1.20 came from
  // the old tracker's Poor 1.77-1.83 and can no longer fire.)
  std::array<double, kNumFamilies> severePilotStabMin = {
      {0.040, 0.055, 0.125, 0.040}};

  // MODERATE gate: pilot-residual EXCESS over its noise share (the
  // figure the receiver reports since 2026-09-06; LINK_ADAPTATION.md
  // "pilot stability"). Measured with bertool's `pilot` column at the
  // fast tier: CCIR Good 0.001-0.005, CCIR Moderate 0.008-0.014
  // (0.02-0.12 per frame in the two-modem churn at M9/M10), AWGN
  // 0.001-0.007. The verdict caps the AMC ladder at M8 - the fast tier
  // fails 25-75% on Moderate at any SNR (bertool) and an SNR-steered
  // ladder sat at M9/M10 on a 23-26 dB Moderate channel moving almost
  // nothing. Applied only once the EMA is seeded from decoded payload
  // frames.
  // Applied to the MEDIAN of the last kPilotWindow decoded-frame samples,
  // not an EMA: single spikes (0.1-0.45 on Moderate, 0.12 once on Good)
  // drove the EMA verdict. Measured medians per 120 s window in the
  // two-modem churn (SKW_CHANCLASS_DEBUG): static 0.003, CCIR Good at
  // M6-M8 0.004-0.0045 (p75 <= 0.006), CCIR Moderate at M8-M11
  // 0.008-0.021 (p25 0.003-0.011).
  // RE-MEASURED 2026-10 (same campaign): the larger of AWGN/Good vs
  // Moderate medians, per family - WIDE 0.005/0.018, MID 0.011/0.021,
  // NARROW 0.017/0.059, ULTRA 0.005/0.017; geometric mean. A single
  // 0.007 read NARROW's own AWGN median (0.017) as Moderate.
  std::array<double, kNumFamilies> moderatePilotMin = {
      {0.010, 0.015, 0.030, 0.009}};
  int moderatePilotMinSamples = 5;

  // Hysteresis: a verdict must repeat before it replaces the last one.
  // Floor-table switching mid-session is disruptive; flapping is worse
  // than being wrong in one direction.
  int confirmRuns = 3;
};

// Fed one sample per DECODED frame (plus failures, which carry FER
// evidence even when nothing else is measurable).
class ChannelClassifier {
 public:
  explicit ChannelClassifier(ChanClassConfig cfg = {}) : cfg_(cfg) {}

  // snrDb: post-equalizer estimate. spreadMs: tracker delay spread.
  // pilotStability: tracker residual ratio (lower is steadier).
  // family: the session's current family (the pilot thresholds are
  // per family - narrow bands read a larger excess on the same channel).
  void onFrame(double snrDb, double spreadMs, double pilotStability,
               bool decodeOk, Family family = Family::Wide);
  void reset();

  ChannelClass verdict() const { return verdict_; }
  int samples() const { return n_; }
  double snrVariance() const;
  double frameErrorRate() const;

 private:
  ChannelClass classifyNow() const;

  ChanClassConfig cfg_;
  Family family_ = Family::Wide;
  // Welford accumulators over a decaying window, so an old regime does
  // not pin the verdict after conditions change.
  int n_ = 0;
  double snrMean_ = 0.0;
  double snrM2_ = 0.0;
  double spreadEma_ = 0.0;
  // Pilot-excess samples from decoded frames (negative = no sample),
  // judged by their median so one spike cannot flip a verdict.
  static constexpr int kPilotWindow = 9;
  std::array<double, kPilotWindow> pilotWin_{};
  int pilotN_ = 0;     // samples held (<= kPilotWindow)
  int pilotHead_ = 0;  // next slot
  double pilotMedian() const;
  // Delay-spread MEDIAN over the last kSpreadWindow decoded frames, for
  // the Severe gate. The EMA (kept for the Static gate) is pulled over
  // the 1.6 ms Severe threshold by estimator outliers: on CCIR Good
  // (0.5 ms, 2 paths) 26% of decoded frames read >= 1.6 ms, most ~5 ms,
  // while the median read 0.62 ms (2026-10-12, M3 ramp seed 4200). With
  // the SNR variance of Good's deep slow fades as the second signal, the
  // verdict went SEVERE and the AMC steered by the CCIR Poor table -
  // M11 -> M9 -> M8 -> M7 at 18 dB with no retries.
  static constexpr int kSpreadWindow = 9;
  std::array<double, kSpreadWindow> spreadWin_{};
  int spreadN_ = 0;
  int spreadHead_ = 0;
  double spreadMedian() const;
  int frames_ = 0;
  int failures_ = 0;

  ChannelClass verdict_ = ChannelClass::Unknown;
  ChannelClass candidate_ = ChannelClass::Unknown;
  int candidateRuns_ = 0;
};

}  // namespace skw
