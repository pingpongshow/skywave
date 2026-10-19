#include "dsp/chirp.hpp"

#include <algorithm>
#include <cmath>

#include "dsp/window.hpp"

namespace skw {
namespace {

// Rep period on the detector grid (profile-dependent rep duration).
int repGrid() { return chirpRepSamples() / ChirpDetector::gridStep(); }

constexpr int kEdgeMs = 2;
constexpr int kSlot0TolGrid = 3;      // start-position window
constexpr int kSameSlopeTolGrid = 6;  // same-slope slot jitter
constexpr double kCfoCaptureMaxHz = 55.0;
// Joint-score thresholds in units of the tracked noise NCC floor. A 4-rep
// noise score sums four window maxima (mean ~15x floor for WIDE window
// sizes); 32x sits ~6 sigma above. Fast (2-rep) has less redundancy and a
// stricter bar.
constexpr double kScoreFactor4 = 32.0;
// The same bar as a length-normalized z-score (zScore): 32x the floor
// for four slots of mean 3.75x and sd 1.417x is 6 sigma.
constexpr double kNoiseSlotMean = 3.75;
constexpr double kNoiseSlotSd = 17.0 / 12.0;
constexpr double kScoreZ4 = (kScoreFactor4 - 4 * kNoiseSlotMean) /
                            (kNoiseSlotSd * 2.0);
constexpr double kScoreFactor2 = 24.0;
// Slot validity: a pattern match must be supported by multiple individually
// significant slots, or a single strong peak seen through a distant window
// can fabricate a match (degenerate single-slot domination).
constexpr double kSlotFloorFactor = 6.0;    // per-slot significance
constexpr double kSlotFloorFactorFast = 12.0;
constexpr int kMinValidSlots4 = 3;
constexpr double kEmaAlpha = 5e-4;    // noise-floor tracker (~1.6 s)
// Runner-up signatures reported with a 4-rep detection (ChirpDetection::
// alts) when they score at least this fraction of the winner.
constexpr double kAltScoreFraction = 0.7;

std::vector<cfloat> makeRep(Family family, bool up) {
  const auto& p = plan(family);
  const double f0 = (up ? p.chirpStartHz : p.chirpEndHz) - centerHz();
  const double f1 = (up ? p.chirpEndHz : p.chirpStartHz) - centerHz();
  const int n = chirpRepSamples();
  const double T = activeProfile().chirpRepSec;
  const double rate = (f1 - f0) / T;
  std::vector<cfloat> rep(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    double t = double(i) / kFsBaseband;
    double phase = 2.0 * M_PI * (f0 * t + 0.5 * rate * t * t);
    rep[size_t(i)] = cfloat(float(std::cos(phase)), float(std::sin(phase)));
  }
  const int edge = kEdgeMs * kFsBaseband / 1000;
  auto ramp = raisedCosineRamp(edge);
  for (int i = 0; i < edge; ++i) {
    rep[size_t(i)] *= ramp[size_t(i)];
    rep[size_t(n - 1 - i)] *= ramp[size_t(i)];
  }
  return rep;
}

std::vector<float> familyBandTaps(Family family) {
  // Band-limit before NCC: without this the normalization counts full
  // 12 kHz noise and NARROW loses ~14 dB of detection margin.
  const auto& p = plan(family);
  const double half = (p.chirpEndHz - p.chirpStartHz) / 2.0;
  return designKaiserLowpass(kFsBaseband, half + kCfoCaptureMaxHz + 30.0,
                             half + kCfoCaptureMaxHz + 380.0, 50.0);
}

}  // namespace

const std::vector<bool>& chirpPattern(Family family, ChirpSig sig) {
  constexpr bool U = true, D = false;
  static const std::vector<bool> kFast{U, U};
  static const std::vector<bool> kRobust{U, D, U, D};
  static const std::vector<bool> kCalling{U, U, D, D};
  static const std::vector<bool> kCss{U, D, D, U};
  // LONG CSS PREAMBLE (NARROW, MID). The 4-rep 160 ms signature was the
  // acquisition limit on a 500 Hz band: at -18 dB a third of frames went
  // undetected and many more were misread as robust/calling. 12 reps
  // (1.92 s, 1-2% of a NARROW CSS frame): balanced (unbiased start and
  // slope-diverse coarse CFO), one embedded 4-rep signature (any
  // balanced pattern has one), shifted self-match <= 6/12 - chosen by
  // exhaustive search. With the sync branch check: M0 10% point
  // ~-16 -> ~-18.3 dB, M1 ~-15.7 -> ~-17.5 dB (60 trials). 8 reps gave
  // about half that. MID (1 kHz band) too, with the receiver's CSS
  // branch retry: M0 at floor-1 33 -> 10%, floor-2 58 -> 37%; M1 and M2
  // better. WIDE/ULTRA keep 4 reps (their CSS floors are met).
  static const std::vector<bool> kCssLong{U, D, U, U, U, U, D, D, D, U, D, D};
  switch (sig) {
    case ChirpSig::Fast: return kFast;
    case ChirpSig::Robust: return kRobust;
    case ChirpSig::Calling: return kCalling;
    case ChirpSig::Css:
      return family == Family::Narrow || family == Family::Mid ? kCssLong
                                                               : kCss;
  }
  return kFast;
}

double chirpCfoCaptureMaxHz() { return kCfoCaptureMaxHz; }

double chirpCfoAmbiguityHz(Family family, ChirpSig sig) {
  const auto& pat = chirpPattern(family, sig);
  int bestGap = 0;
  for (size_t i = 0; i < pat.size(); ++i) {
    for (size_t j = i + 1; j < pat.size(); ++j) {
      if (pat[i] != pat[j]) continue;
      const int gap = int(j - i);
      if (bestGap == 0 || gap < bestGap) bestGap = gap;
    }
  }
  return bestGap > 0 ? 1.0 / (bestGap * activeProfile().chirpRepSec) : 0.0;
}

double chirpStartCfoSensitivity(Family family, ChirpSig sig) {
  const auto& pat = chirpPattern(family, sig);
  int nUp = 0;
  for (bool u : pat) nUp += u ? 1 : 0;
  const int nDown = int(pat.size()) - nUp;
  const auto& p = plan(family);
  const double bw = p.chirpEndHz - p.chirpStartHz;
  return activeProfile().chirpRepSec / bw * kFsBaseband *
         double(nUp - nDown) / double(pat.size());
}

std::vector<cfloat> makeChirpPreamble(Family family, ChirpSig sig) {
  auto up = makeRep(family, true);
  auto down = makeRep(family, false);
  std::vector<cfloat> out;
  for (bool u : chirpPattern(family, sig)) {
    const auto& rep = u ? up : down;
    out.insert(out.end(), rep.begin(), rep.end());
  }
  return out;
}

// ---------------------------------------------------------------- detector

ChirpDetector::ChirpDetector(Family family)
    : family_(family), bandFilter_(familyBandTaps(family), 1) {
  filterDelay_ = bandFilter_.groupDelayInput();
  emaNoiseNcc_ = 1.0 / chirpRepSamples();
  for (ChirpSig sig : {ChirpSig::Fast, ChirpSig::Robust, ChirpSig::Calling,
                       ChirpSig::Css}) {
    pat_[size_t(sig)] = chirpPattern(family, sig);
    if (sig != ChirpSig::Fast) {
      maxReps_ = std::max(maxReps_, int(pat_[size_t(sig)].size()));
      if (pat_[size_t(sig)].size() > 4) longReps_ = int(pat_[size_t(sig)].size());
    }
  }
  const auto& p = plan(family);
  const double bw = p.chirpEndHz - p.chirpStartHz;
  crossTolGrid_ = int(2.0 * kCfoCaptureMaxHz *
                        activeProfile().chirpRepSec / bw *
                      kFsBaseband / gridStep()) + 6;
  // 4-rep suppression spans +-2 reps: +-1/2-rep partial alignments (which
  // can pass slot-validity at the floor with one lucky noise slot) are
  // vetoed by direct comparison against the true start's higher score;
  // +-3-rep partials contain only one real slot and die on validity.
  suppress4_ = 2 * repGrid() + crossTolGrid_ + kSlot0TolGrid + 8;
  // Fast suppression spans its own +-1-rep partials.
  suppressFast_ = repGrid() + crossTolGrid_ + kSlot0TolGrid + 8;
  auto up = makeRep(family, true);
  auto down = makeRep(family, false);
  refUp_.resize(up.size());
  refDown_.resize(down.size());
  refEnergy_ = 0.0f;
  for (size_t i = 0; i < up.size(); ++i) {
    refUp_[i] = std::conj(up[i]);
    refDown_[i] = std::conj(down[i]);
    refEnergy_ += std::norm(up[i]);
  }
}

const ChirpDetector::GridVal* ChirpDetector::gridAt(int64_t g) const {
  int64_t idx = g - gridStartIdx_;
  if (idx < 0 || idx >= int64_t(grid_.size())) return nullptr;
  return &grid_[size_t(idx)];
}

float ChirpDetector::nccAt(int64_t g, bool up) const {
  const GridVal* v = gridAt(g);
  if (!v) return 0.0f;
  return up ? v->nccUp : v->nccDown;
}

void ChirpDetector::process(std::span<const cfloat> in,
                            std::vector<ChirpDetection>& out) {
  filtered_.clear();
  bandFilter_.process(in, filtered_);
  buf_.insert(buf_.end(), filtered_.begin(), filtered_.end());

  const int n = chirpRepSamples();
  while (true) {
    int64_t nextGrid = gridStartIdx_ + int64_t(grid_.size());
    if (nextGrid * gridStep() + n > bufStart_ + int64_t(buf_.size())) break;
    evaluateGridPoint(nextGrid);
  }
  finalizeFast(out);
  finalize4(out);

  // Bound memory. Grid history must cover the 4-rep decision lag (pattern
  // horizon + suppression) measured back from the newest grid point.
  const size_t keepSamples = size_t(n) * size_t(maxReps_ + 4);
  if (buf_.size() > keepSamples * 2) {
    size_t drop = buf_.size() - keepSamples;
    buf_.erase(buf_.begin(), buf_.begin() + long(drop));
    bufStart_ += int64_t(drop);
  }
  const size_t keepGrid =
      size_t(maxReps_ * repGrid() + 2 * crossTolGrid_ + 2 * suppress4_ + 128);
  if (grid_.size() > keepGrid * 2) {
    size_t drop = grid_.size() - keepGrid;
    grid_.erase(grid_.begin(), grid_.begin() + long(drop));
    gridStartIdx_ += int64_t(drop);
  }
  auto pruneRing = [](ScoreRing& r, size_t keep) {
    if (r.v.size() > keep * 2) {
      size_t drop = r.v.size() - keep;
      r.v.erase(r.v.begin(), r.v.begin() + long(drop));
      r.base += int64_t(drop);
    }
  };
  pruneRing(fastScores_, size_t(2 * suppressFast_ + 8));
  pruneRing(best4Scores_, size_t(2 * suppress4_ + 8));
  pruneRing(bestLongScores_, size_t(2 * suppress4_ + 8));
  pruneRing(decisionFloor_,
            size_t(2 * suppress4_ + (maxReps_ - 3) * repGrid() + 64));
}

void ChirpDetector::evaluateGridPoint(int64_t g) {
  const int n = chirpRepSamples();
  const cfloat* x = buf_.data() + (g * gridStep() - bufStart_);

  cfloat cu(0, 0), cd(0, 0);
  float energy = 0;
  for (int k = 0; k < n; ++k) {
    cu += x[k] * refUp_[size_t(k)];
    cd += x[k] * refDown_[size_t(k)];
    energy += std::norm(x[k]);
  }
  GridVal v;
  float denom = energy * refEnergy_;
  v.nccUp = denom > 1e-12f ? std::norm(cu) / denom : 0.0f;
  v.nccDown = denom > 1e-12f ? std::norm(cd) / denom : 0.0f;
  v.corrUp = cu;
  v.corrDown = cd;
  grid_.push_back(v);

  // Track the noise NCC floor (clipped so signal peaks cannot poison it).
  //
  // DIGITAL SILENCE IS NOT NOISE. A window with no energy has no NCC at
  // all (both read 0 above), and folding those zeros into the EMA drove
  // the floor geometrically to 0 - every threshold below is a multiple of
  // it, so after ~30 s of exact zeros (a virtual audio cable whose writer
  // is idle or dead, a muted interface) the detector emitted a phantom
  // q=0.00 detection every ~0.65 s, forever: the clip below caps recovery
  // at 6x the CURRENT floor, so a collapsed floor cannot climb back. On
  // the ST3 kill/resume path each phantom opened a capture that climbed
  // the whole 90 s ladder, the worker fell behind real time, the RX ring
  // overflowed and handshake frames were lost in the dropped audio.
  // Skip zero-energy windows, and never let the floor fall below half the
  // white-noise expectation (E[NCC] = 1/N), which real noise never does.
  auto emaUpdate = [&](double ncc) {
    double clipped = std::min(ncc, 6.0 * emaNoiseNcc_);
    emaNoiseNcc_ += kEmaAlpha * (clipped - emaNoiseNcc_);
  };
  if (denom > 1e-12f) {
    emaUpdate(v.nccUp);
    emaUpdate(v.nccDown);
  }
  emaNoiseNcc_ = std::max(emaNoiseNcc_, 0.5 / double(n));
}

ChirpDetector::PatternScore ChirpDetector::scorePattern(int64_t g0,
                                                        ChirpSig sig,
                                                        bool inverted) const {
  const auto& pat = pat_[size_t(sig)];
  PatternScore ps;
  ps.score = 0.0f;
  const bool slope0 = inverted ? !pat[0] : pat[0];
  for (size_t i = 0; i < pat.size(); ++i) {
    const bool up = inverted ? !pat[i] : pat[i];
    const int tol = (i == 0) ? kSlot0TolGrid
                             : (up == slope0 ? kSameSlopeTolGrid
                                             : crossTolGrid_);
    const int64_t want = g0 + int64_t(i) * repGrid();
    float best = 0.0f;
    int64_t bestG = want;
    for (int64_t d = -tol; d <= tol; ++d) {
      float ncc = nccAt(want + d, up);
      if (ncc > best) {
        best = ncc;
        bestG = want + d;
      }
    }
    // Parabolic sub-grid refinement around the slot maximum.
    double gridF = double(bestG);
    float prev = nccAt(bestG - 1, up), next = nccAt(bestG + 1, up);
    double dn = double(prev) - 2.0 * best + double(next);
    if (dn < -1e-12) {
      double delta = 0.5 * (double(prev) - double(next)) / dn;
      gridF += std::clamp(delta, -0.6, 0.6);
    }
    const GridVal* gv = gridAt(bestG);
    SlotHit hit;
    hit.gridF = gridF;
    hit.ncc = best;
    hit.corr = gv ? (up ? gv->corrUp : gv->corrDown) : cfloat(0, 0);
    hit.up = up;
    ps.slots.push_back(hit);
    ps.score += best;
  }
  return ps;
}

namespace {
// A slot is valid if it clears BOTH the absolute noise-relative floor and a
// pattern-relative floor (score/12): a strong 2-slot prefix plus one lucky
// noise window-max must not count as 3 valid slots.
int validSlotCount(const ChirpDetector::PatternScore& ps, float absFloor) {
  // Half the mean slot (score/8 for four slots).
  const float floor = std::max(
      absFloor, ps.score / (2.0f * float(std::max<size_t>(ps.slots.size(), 1))));
  int n = 0;
  for (const auto& s : ps.slots) n += (s.ncc >= floor) ? 1 : 0;
  return n;
}
// Valid slots a 4-rep-timeline pattern needs: 3 of 4; half of a long one
// (the joint sum, not per-slot significance, is what a long preamble buys).
int minValidSlots(size_t reps) {
  return reps <= 4 ? kMinValidSlots4 : int((reps + 1) / 2);
}
}  // namespace

double ChirpDetector::zScore(const PatternScore& ps, double noise) const {
  const double L = double(ps.slots.size());
  if (L <= 0.0 || noise <= 0.0) return -1e9;
  return (double(ps.score) / noise - kNoiseSlotMean * L) /
         (kNoiseSlotSd * std::sqrt(L));
}

double ChirpDetector::zOf(float score, size_t reps, double noise) const {
  if (reps == 0 || noise <= 0.0 || score <= -1e8f) return -1e9;
  const double L = double(reps);
  return (double(score) / noise - kNoiseSlotMean * L) /
         (kNoiseSlotSd * std::sqrt(L));
}

// Fast timeline: short horizon, because connected-frame latency matters.
// A fast candidate must not be part of a 4-rep signature: forward check
// (calling's D-reps start 2 rep periods after its U-U prefix) and backward
// check (the D-D tail of calling is itself a valid fast-inverted pattern).
void ChirpDetector::finalizeFast(std::vector<ChirpDetection>& out) {
  const int64_t newest = gridStartIdx_ + int64_t(grid_.size()) - 1;
  // Decision lag: own-pattern horizon + suppression, and the containment
  // checks (slot 3 of a would-be calling signature at full CFO shift).
  const int64_t lag =
      std::max<int64_t>(repGrid() + kSameSlopeTolGrid + suppressFast_,
                        3 * repGrid() + crossTolGrid_ + 8);

  while (nextFast_ + lag <= newest) {
    const int64_t g0 = nextFast_++;
    while (fastScores_.base + int64_t(fastScores_.v.size()) <=
           g0 + suppressFast_) {
      int64_t s = fastScores_.base + int64_t(fastScores_.v.size());
      fastScores_.v.push_back(
          std::max(scorePattern(s, ChirpSig::Fast, false).score,
                   scorePattern(s, ChirpSig::Fast, true).score));
    }
    // Directional refractory: suppress starts shortly AFTER an emission
    // (an emitted signature's sub-patterns), never earlier ones (or a
    // later cursor's emission would veto the true earlier start).
    const int64_t dRef = g0 - lastEmitStart_;
    if (dRef > 0 && dRef <= 2 * repGrid() + suppress4_) continue;

    const float thr2 = float(kScoreFactor2 * emaNoiseNcc_);
    const float here = fastScores_.at(g0);
    if (here < thr2) continue;

    bool isMax = true;
    for (int64_t s = g0 - suppressFast_; s <= g0 + suppressFast_; ++s) {
      if (s != g0 && fastScores_.at(s) > here) {
        isMax = false;
        break;
      }
    }
    if (!isMax) continue;

    PatternScore up = scorePattern(g0, ChirpSig::Fast, false);
    PatternScore dn = scorePattern(g0, ChirpSig::Fast, true);
    bool inv = dn.score > up.score;
    const PatternScore& ps = inv ? dn : up;
    if (validSlotCount(ps, float(kSlotFloorFactorFast * emaNoiseNcc_)) < 2) {
      continue;
    }

    // Continuation / prefix checks against 4-rep signatures: a fast pair
    // is either standalone or embedded in a calling signature (its U-U
    // head or D-D tail). The opposite-slope reps of the containing
    // signature sit at known offsets, and fast-class sessions are near
    // frequency-locked, so NARROW windows (+-10 grid) suffice - which
    // keeps the noise false-defer rate low enough that genuine fast
    // frames are almost never sacrificed, while floor-level calling
    // D-reps still register.
    const bool fastUp = ps.slots[0].up;
    const float contFloor =
        std::max(float(5.5 * emaNoiseNcc_), ps.score / 10.0f);
    auto oppositeMax = [&](int64_t center) {
      float m = 0;
      for (int64_t d = -10; d <= 10; ++d) {
        m = std::max(m, nccAt(center + d, !fastUp));
      }
      return m;
    };
    if (oppositeMax(g0 + 2 * repGrid()) >= contFloor ||
        oppositeMax(g0 + 3 * repGrid()) >= contFloor ||
        oppositeMax(g0 - repGrid()) >= contFloor ||
        oppositeMax(g0 - 2 * repGrid()) >= contFloor) {
      continue;
    }
    // Large-CFO containment: during calling acquisition the opposite-slope
    // reps shift by 2d (d = cfo*T/B*fs/grid) relative to the pair. Search
    // candidate separations and require BOTH partner slots to light up at
    // a consistent d - the joint requirement keeps the noise false-defer
    // rate ~2% where one wide window would cost ~20% of genuine fasts.
    {
      const int dmax = (crossTolGrid_ - 6) / 2 + 2;
      const float jointFloor =
          std::max(float(4.5 * emaNoiseNcc_), ps.score / 12.0f);
      auto narrowMax = [&](int64_t c) {
        float m = 0;
        for (int e = -4; e <= 4; ++e) m = std::max(m, nccAt(c + e, !fastUp));
        return m;
      };
      bool contained = false;
      for (int d = -dmax; d <= dmax && !contained; ++d) {
        // Pair as head: partners at +2/+3 reps, shifted +2d.
        if (narrowMax(g0 + 2 * repGrid() + 2 * d) >= jointFloor &&
            narrowMax(g0 + 3 * repGrid() + 2 * d) >= jointFloor) {
          contained = true;
        }
        // Pair as tail: partners at -1/-2 reps, shifted -2d.
        if (narrowMax(g0 - repGrid() - 2 * d) >= jointFloor &&
            narrowMax(g0 - 2 * repGrid() - 2 * d) >= jointFloor) {
          contained = true;
        }
        // Pair as the U-U middle of an inverted CSS signature (D U U D):
        // opposite-slope partners at -1 and +2 reps, common shift.
        if (narrowMax(g0 - repGrid() + 2 * d) >= jointFloor &&
            narrowMax(g0 + 2 * repGrid() + 2 * d) >= jointFloor) {
          contained = true;
        }
      }
      if (contained) continue;
    }
    emitDetection(g0, ChirpSig::Fast, inv, ps, out);
  }
}

// 4-rep timeline: full pattern horizon; suppression spans +-2 reps so
// partial alignments are vetoed by score comparison against the true start.
void ChirpDetector::finalize4(std::vector<ChirpDetection>& out) {
  const int64_t newest = gridStartIdx_ + int64_t(grid_.size()) - 1;
  const int64_t horizon = int64_t(maxReps_ - 1) * repGrid() + crossTolGrid_;
  // Floor snapshot at the 4-rep decision point (see decisionFloor_).
  const int64_t horizon4 = 3 * repGrid() + crossTolGrid_;
  while (nextFloor_ + horizon4 + suppress4_ <= newest) {
    if (decisionFloor_.v.empty()) decisionFloor_.base = nextFloor_;
    decisionFloor_.v.push_back(float(emaNoiseNcc_));
    ++nextFloor_;
  }

  while (next4_ + horizon + suppress4_ <= newest) {
    const int64_t g0 = next4_++;
    while (best4Scores_.base + int64_t(best4Scores_.v.size()) <=
           g0 + suppress4_) {
      int64_t s = best4Scores_.base + int64_t(best4Scores_.v.size());
      // Raw best score over the 4-rep patterns (as a 4-rep detector), and
      // separately the best LONG-pattern reading - which only counts if
      // it could be emitted (enough valid slots): a partial long-CSS
      // alignment over a faded robust preamble must not veto the true
      // start and then fail validity itself.
      float b4 = -1e9f, bL = -1e9f;
      const float ringFloor = float(kSlotFloorFactor * emaNoiseNcc_);
      for (ChirpSig sig :
           {ChirpSig::Robust, ChirpSig::Calling, ChirpSig::Css}) {
        for (bool inv : {false, true}) {
          const PatternScore ps = scorePattern(s, sig, inv);
          if (ps.slots.size() <= 4) {
            b4 = std::max(b4, ps.score);
          } else if (validSlotCount(ps, ringFloor) >=
                     minValidSlots(ps.slots.size())) {
            bL = std::max(bL, ps.score);
          }
        }
      }
      best4Scores_.v.push_back(b4);
      bestLongScores_.v.push_back(bL);
    }
    const int64_t dRef = g0 - lastEmitStart_;
    if (dRef > 0 && dRef <= int64_t(maxReps_ - 2) * repGrid() + suppress4_) {
      continue;
    }

    // Everything at this start is judged against ONE floor, so for equal
    // lengths the z comparisons below are exactly the raw-score ones.
    const float fl = decisionFloor_.at(g0);
    const double n0 = fl > 0.0f ? double(fl) : emaNoiseNcc_;
    auto ringZ = [&](int64_t q) {
      return std::max(zOf(best4Scores_.at(q), 4, n0),
                      longReps_ > 0
                          ? zOf(bestLongScores_.at(q), size_t(longReps_), n0)
                          : -1e9);
    };
    const double here = ringZ(g0);
    if (here < kScoreZ4) continue;

    const float slotFloor = float(kSlotFloorFactor * n0);
    ChirpSig bestSig = ChirpSig::Robust;
    bool bestInv = false;
    PatternScore bestValid;
    bestValid.score = -1.0f;
    double bestZ = -1e9;
    for (ChirpSig sig :
         {ChirpSig::Robust, ChirpSig::Calling, ChirpSig::Css}) {
      for (bool inv : {false, true}) {
        PatternScore ps = scorePattern(g0, sig, inv);
        const double z = zScore(ps, n0);
        if (z > bestZ &&
            validSlotCount(ps, slotFloor) >= minValidSlots(ps.slots.size())) {
          bestValid = ps;
          bestZ = z;
          bestSig = sig;
          bestInv = inv;
        }
      }
    }
    if (bestValid.score < 0.0f || bestZ < kScoreZ4) continue;

    bool isMax = true;
    for (int64_t s = g0 - suppress4_; s <= g0 + suppress4_; ++s) {
      if (s != g0 && ringZ(s) > here) {
        isMax = false;
        break;
      }
    }
    if (!isMax) continue;
    emitDetection(g0, bestSig, bestInv, bestValid, out);
    // RUNNER-UP SIGNATURES. Near the floor single slope slots flip, and
    // the true pattern is usually ONE REP AWAY from the winner: calling
    // U-U-D-D started a rep early matches three of CSS U-D-D-U's four
    // slots, so a misread CSS preamble's own pattern scores 0.76-0.96 of
    // the winner at +-1 rep (measured, NARROW M0 -17 dB) and only 0.4-0.5
    // at the winner's start. Report the best reading of each OTHER
    // signature at the winner's start or +-1 rep (+-8 grid), with two
    // valid slots: the receiver tries them only after the winner's path
    // failed, and the header CRC arbitrates.
    ChirpDetection& d = out.back();
    for (ChirpSig sig :
         {ChirpSig::Robust, ChirpSig::Calling, ChirpSig::Css}) {
      if (sig == bestSig) continue;
      PatternScore bestAlt;
      bestAlt.score = -1.0f;
      bool altInv = false;
      for (int64_t g = g0 - repGrid() - 8; g <= g0 + repGrid() + 8; ++g) {
        const int64_t off = std::abs(g - g0);
        if (off > 8 && std::abs(off - repGrid()) > 8) continue;
        for (bool inv : {false, true}) {
          PatternScore ps = scorePattern(g, sig, inv);
          if (ps.score > bestAlt.score && validSlotCount(ps, slotFloor) >= 2) {
            bestAlt = ps;
            altInv = inv;
          }
        }
      }
      // Per-slot mean ratio (the raw-score ratio for equal lengths).
      if (bestAlt.score < 0.0f ||
          bestAlt.score / float(bestAlt.slots.size()) <
              float(kAltScoreFraction) * bestValid.score /
                  float(bestValid.slots.size())) {
        continue;
      }
      const ChirpDetection a = makeDetection(sig, altInv, bestAlt);
      ChirpDetection::Alt alt{sig, altInv, a.startSample, a.cfoHz,
                              bestAlt.score};
      if (d.nAlt < 2) {
        d.alts[d.nAlt++] = alt;
      }
    }
    if (d.nAlt == 2 && d.alts[1].score > d.alts[0].score) {
      std::swap(d.alts[0], d.alts[1]);
    }
  }
}

void ChirpDetector::emitDetection(int64_t g0, ChirpSig sig, bool inverted,
                                  const PatternScore& ps,
                                  std::vector<ChirpDetection>& out) {
  out.push_back(makeDetection(sig, inverted, ps));
  lastEmitStart_ = g0;
}

ChirpDetection ChirpDetector::makeDetection(ChirpSig sig, bool inverted,
                                            const PatternScore& ps) const {
  // Timing from per-slot interpolated positions; slope-dependent CFO
  // shifts cancel when both slopes are present.
  double meanUp = 0, meanDown = 0, meanAll = 0;
  int nUp = 0, nDown = 0;
  for (size_t i = 0; i < ps.slots.size(); ++i) {
    double start = ps.slots[i].gridF - double(i) * repGrid();
    meanAll += start;
    if (ps.slots[i].up) { meanUp += start; ++nUp; }
    else { meanDown += start; ++nDown; }
  }
  meanAll /= double(ps.slots.size());

  const auto& p = plan(family_);
  const double bw = p.chirpEndHz - p.chirpStartHz;
  // Coarse CFO from slope diversity. Under +cfo an up-rep's correlation
  // peak arrives EARLY by d = cfo*T/B*fs samples, a down-rep's LATE.
  double cfoCoarse = 0.0;
  if (nUp > 0 && nDown > 0) {
    double d = (meanUp / nUp - meanDown / nDown) / 2.0 * gridStep();
    cfoCoarse = -d / kFsBaseband * bw / activeProfile().chirpRepSec;
  }

  // Fine CFO from same-slope inter-rep correlation phase (ambiguity
  // 1/(gap*T)), summed over pairs at the smallest same-slope gap, then
  // unwrapped against the coarse estimate.
  double cfoFine = cfoCoarse;
  int bestGap = 0;
  for (size_t i = 0; i < ps.slots.size(); ++i) {
    for (size_t j = i + 1; j < ps.slots.size(); ++j) {
      if (ps.slots[i].up != ps.slots[j].up) continue;
      int gap = int(j - i);
      if (bestGap == 0 || gap < bestGap) bestGap = gap;
    }
  }
  if (bestGap > 0) {
    cfloat rotSum(0, 0);
    for (size_t i = 0; i < ps.slots.size(); ++i) {
      for (size_t j = i + 1; j < ps.slots.size(); ++j) {
        if (ps.slots[i].up == ps.slots[j].up && int(j - i) == bestGap) {
          rotSum += ps.slots[j].corr * std::conj(ps.slots[i].corr);
        }
      }
    }
    double gapT = bestGap * activeProfile().chirpRepSec;
    double phase = std::atan2(rotSum.imag(), rotSum.real());
    double base = phase / (2.0 * M_PI * gapT);
    double amb = 1.0 / gapT;
    double k = std::round((cfoCoarse - base) / amb);
    cfoFine = base + k * amb;
  }

  ChirpDetection det;
  det.family = family_;
  det.sig = sig;
  det.inverted = inverted;
  det.startSample = meanAll * gridStep() - filterDelay_;
  det.cfoHz = cfoFine;
  det.quality = ps.score / float(ps.slots.size());
  return det;
}

}  // namespace skw
