// Chirp preamble generation and matched-filter detection (WAVEFORM.md
// acquisition, DSP.md sync engine SEARCH/COARSE stages).
//
// Everything operates in the 12 kHz complex baseband. A family's chirp
// sweeps its audio band shifted by -kCenterHz.
//
// Detection strategy: per-slope normalized correlation (NCC) on a decimated
// grid, then JOINT pattern scoring - for each candidate start, the score of
// a signature is the sum of per-rep-slot NCC window maxima. Thresholding
// the sum (relative to a tracked noise floor) integrates all reps into one
// statistic, which is what makes the deep-floor families detectable;
// per-rep thresholding loses ~6 dB of ROC at the NARROW floor.
#pragma once

#include <complex>
#include <span>
#include <vector>

#include "core/constants.hpp"
#include "dsp/fir.hpp"

namespace skw {

using cfloat = std::complex<float>;

enum class ChirpSig : uint8_t { Fast = 0, Robust = 1, Calling = 2, Css = 3 };

// Slope pattern for a signature (true = up). Index = rep number.
// fast: U U   robust: U D U D   calling: U U D D   css: U D D U
// (patterns and their spectrum-inverted mirrors are pairwise distinct).
std::vector<bool> chirpPattern(ChirpSig sig);
// The pattern a family actually transmits: chirpPattern(sig), except the
// CSS signature on families with a LONG CSS preamble (deep-tier frames on
// narrow bands, where the 4-rep signature limited acquisition - see
// chirp.cpp). SKW_CSS_PREAMBLE_<FAMILY>=<U/D string> overrides (A/B).
std::vector<bool> chirpPattern(Family family, ChirpSig sig);

inline int chirpRepSamples() {
  // Profile-dependent: 1920 (HF, 160 ms) or 480 (VHF-FM, 40 ms).
  return int(activeProfile().chirpRepSec * kFsBaseband);
}

// CFO capture range of the detector's matched filter (+-Hz). Every
// signature's ridge integrates this whole range.
double chirpCfoCaptureMaxHz();

// Ambiguity spacing of the detector's FINE CFO estimate for a signature:
// same-slope inter-rep phase is ambiguous modulo 1/(gap*T) for the
// smallest same-slope rep gap (fast/calling/css: 1/T = 6.25 Hz on HF,
// robust: 1/(2T) = 3.125 Hz). The detector unwraps it against a COARSE
// estimate from up/down peak timing, which a slope-less pattern (fast)
// does not have and which two-path multipath can bias by several
// branches (GAP_CLOSURE_PLAN.md R1) - receiveFrame resolves the branch.
double chirpCfoAmbiguityHz(ChirpSig sig);
double chirpCfoAmbiguityHz(Family family, ChirpSig sig);

// Delay-Doppler bias of the detector's start estimate, in samples per Hz
// of CFO: under +cfo an up-rep's peak arrives early by cfo*T/B*fs and a
// down-rep's late, so the slot-mean start is biased by that times
// (nDown - nUp)/n. Zero for balanced patterns; the returned value is the
// correction to ADD per Hz (positive for fast U-U).
double chirpStartCfoSensitivity(Family family, ChirpSig sig);

// Baseband preamble for a family/signature: reps back-to-back, raised-cosine
// edges (2 ms) per rep, unit RMS per rep.
std::vector<cfloat> makeChirpPreamble(Family family, ChirpSig sig);

struct ChirpDetection {
  Family family;
  ChirpSig sig;
  bool inverted;       // spectrum-inverted signal detected
  double startSample;  // preamble start in the detector's sample count
  double cfoHz;        // estimated carrier frequency offset
  float quality;       // mean per-rep NCC of the winning pattern
  // Runner-up 4-rep signatures (the best reading of each OTHER pattern
  // that scores >= kAltScoreFraction of the winner): near the
  // floor single slope slots flip and a CSS U-D-D-U read as robust U-D-U-D
  // or calling U-U-D-D (NARROW M0 at -17 dB: 10 of 40 frames). The
  // receiver tries it only when the winner's path fails.
  struct Alt {
    ChirpSig sig = ChirpSig::Robust;
    bool inverted = false;
    double startSample = 0.0;
    double cfoHz = 0.0;
    float score = 0.0f;
  };
  int nAlt = 0;    // best reading per other signature, best first
  Alt alts[2]{};
};

// Streaming matched-filter detector for one family (both slopes).
class ChirpDetector {
 public:
  static constexpr int kGridStep = 4;  // 0.33 ms timing quantization

  explicit ChirpDetector(Family family);

  // Feed baseband samples; completed detections are appended to out.
  // Decision latency: a signature is classified roughly one rep period
  // plus the CFO slot window after its last rep (fast additionally waits
  // out the calling-continuation window, since U-U prefixes U-U-D-D).
  void process(std::span<const cfloat> in, std::vector<ChirpDetection>& out);

  void reset();
  uint64_t samplesConsumed() const { return consumed_; }
  double noiseFloorNcc() const { return emaNoiseNcc_; }

  struct SlotHit {
    double gridF;   // interpolated peak position within the slot window
    float ncc;
    cfloat corr;
    bool up;
  };
  struct PatternScore {
    float score;                 // sum of slot NCC maxima
    std::vector<SlotHit> slots;  // per-rep best hits
  };

  // Diagnostics hook (test/tooling support).
  void debugScore(int64_t g0) const;

 private:
  struct GridVal {
    float nccUp, nccDown;
    cfloat corrUp, corrDown;
  };

  void evaluateGridPoint(int64_t grid);
  void finalizeStarts(std::vector<ChirpDetection>& out);
  PatternScore scorePattern(int64_t g0, ChirpSig sig, bool inverted) const;
  float nccAt(int64_t grid, bool up) const;
  const GridVal* gridAt(int64_t grid) const;
  void emitDetection(int64_t g0, ChirpSig sig, bool inverted,
                     const PatternScore& ps, std::vector<ChirpDetection>& out);
  ChirpDetection makeDetection(ChirpSig sig, bool inverted,
                               const PatternScore& ps) const;

  Family family_;
  std::vector<bool> pat_[4];     // per-signature slope pattern (family's)
  int maxReps_ = 4;              // longest 4-rep-timeline pattern
  // Length-normalized pattern score: sigmas above the noise expectation
  // (a noise slot window-max averages ~3.75x the NCC floor, sd ~1.42x).
  // For one pattern length it orders exactly as the raw score; it is what
  // lets a long CSS pattern compete with the 4-rep signatures.
  double zScore(const PatternScore& ps, double noise) const;
  double zOf(float score, size_t reps, double noise) const;
  std::vector<cfloat> refUp_;    // conj already applied
  std::vector<cfloat> refDown_;
  float refEnergy_;
  int crossTolGrid_;             // CFO-induced cross-slope slot shift bound
  FirDecimator bandFilter_;      // band-limit so NCC sees in-band noise only
  int filterDelay_;              // group delay (12 kHz samples)
  std::vector<cfloat> filtered_;

  std::vector<cfloat> buf_;      // rolling sample buffer
  int64_t bufStart_ = 0;         // absolute index of buf_[0]
  uint64_t consumed_ = 0;

  std::vector<GridVal> grid_;    // rolling NCC history
  int64_t gridStartIdx_ = 0;     // grid index of grid_[0]

  // Rolling per-start scores, on two timelines: fast decisions use a short
  // horizon (latency matters for connected frames); 4-rep decisions use
  // the full pattern horizon with suppression spanning +-2 reps so partial
  // alignments are vetoed by comparison against the true start's score.
  struct ScoreRing {
    std::vector<float> v;
    int64_t base = 0;
    float at(int64_t s) const {
      int64_t i = s - base;
      return (i >= 0 && i < int64_t(v.size())) ? v[size_t(i)] : 0.0f;
    }
  };
  ScoreRing fastScores_;
  ScoreRing best4Scores_;   // raw best 4-rep score per start (no validity)
  ScoreRing bestLongScores_;  // raw best VALID long-pattern score per start
  // Noise floor each start is judged against: the floor at the moment a
  // 4-rep-only detector would have decided it (pattern horizon 3 reps).
  // A long pattern lengthens the decision lag, and the floor - an EMA the
  // preamble's own chirps feed - would otherwise be judged later and
  // higher. With this, 4-rep detection is identical to a 4-rep detector.
  ScoreRing decisionFloor_;
  int64_t nextFloor_ = 0;
  int longReps_ = 0;  // length of the long pattern (0 = none)
  int64_t nextFast_ = 0;         // next start to decide on the fast timeline
  int64_t next4_ = 0;            // next start to decide on the 4-rep timeline
  int suppress4_ = 0;
  int suppressFast_ = 0;
  int64_t lastEmitStart_ = -1'000'000'000;

  void finalizeFast(std::vector<ChirpDetection>& out);
  void finalize4(std::vector<ChirpDetection>& out);

  double emaNoiseNcc_;           // tracked noise NCC floor
};

}  // namespace skw
