// Channel-character classifier (LINK_ADAPTATION.md).
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

#include <cstdint>

namespace skw {

enum class ChannelClass : uint8_t {
  Unknown = 0,  // not enough evidence yet - callers use fading floors
  Static,       // AWGN-like: stable pilots, negligible spread, SNR steady
  Fading,       // ordinary ionospheric fading (the design assumption)
  Severe,       // CCIR-Poor-like: fast Doppler and/or deep spread
};

const char* toString(ChannelClass c);

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

  // SEVERE gates - any two of the three.
  double severeSnrVarMin = 12.0;
  double severeSpreadMsMin = 1.6;
  double severeFerMin = 0.35;
  // The sharpest discriminator measured: Poor 1.77-1.83 against
  // Moderate 0.38-0.55. Worth a signal of its own.
  double severePilotStabMin = 1.20;

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
  void onFrame(double snrDb, double spreadMs, double pilotStability,
               bool decodeOk);
  void reset();

  ChannelClass verdict() const { return verdict_; }
  int samples() const { return n_; }
  double snrVariance() const;
  double frameErrorRate() const;

 private:
  ChannelClass classifyNow() const;

  ChanClassConfig cfg_;
  // Welford accumulators over a decaying window, so an old regime does
  // not pin the verdict after conditions change.
  int n_ = 0;
  double snrMean_ = 0.0;
  double snrM2_ = 0.0;
  double spreadEma_ = 0.0;
  double pilotEma_ = 0.0;
  int frames_ = 0;
  int failures_ = 0;

  ChannelClass verdict_ = ChannelClass::Unknown;
  ChannelClass candidate_ = ChannelClass::Unknown;
  int candidateRuns_ = 0;
};

}  // namespace skw
