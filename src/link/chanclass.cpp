#include "link/chanclass.hpp"

#include <cstdlib>
#include <string>

namespace skw {

const char* toString(ChannelClass c) {
  switch (c) {
    case ChannelClass::Static: return "STATIC";
    case ChannelClass::Fading: return "FADING";
    case ChannelClass::Severe: return "SEVERE";
    case ChannelClass::Unknown: break;
  }
  return "UNKNOWN";
}

namespace {
// Decay applied to the SNR accumulators every window, so the verdict
// tracks the CURRENT regime instead of averaging a whole session.
constexpr int kWindow = 48;
constexpr double kEmaAlpha = 0.15;
}  // namespace

void ChannelClassifier::onFrame(double snrDb, double spreadMs,
                                double pilotStability, bool decodeOk) {
  ++frames_;
  if (!decodeOk) {
    ++failures_;
    // A failed frame yields no usable SNR/spread estimate - its only
    // evidence is the failure itself, which the FER gate consumes.
    if (frames_ >= cfg_.minSamples) {
      const ChannelClass now = classifyNow();
      if (now == candidate_) {
        ++candidateRuns_;
      } else {
        candidate_ = now;
        candidateRuns_ = 1;
      }
      if (candidateRuns_ >= cfg_.confirmRuns) verdict_ = candidate_;
    }
    return;
  }

  // Welford, with a soft window: once n_ reaches kWindow the oldest
  // evidence is bled off rather than kept forever.
  if (n_ >= kWindow) {
    snrM2_ *= double(kWindow - 1) / double(kWindow);
    n_ = kWindow - 1;
  }
  ++n_;
  const double delta = snrDb - snrMean_;
  snrMean_ += delta / n_;
  snrM2_ += delta * (snrDb - snrMean_);

  spreadEma_ = n_ == 1 ? spreadMs
                       : spreadEma_ + kEmaAlpha * (spreadMs - spreadEma_);
  pilotEma_ = n_ == 1
                  ? pilotStability
                  : pilotEma_ + kEmaAlpha * (pilotStability - pilotEma_);

  if (frames_ >= cfg_.minSamples && n_ >= 2) {
    const ChannelClass now = classifyNow();
    if (now == candidate_) {
      ++candidateRuns_;
    } else {
      candidate_ = now;
      candidateRuns_ = 1;
    }
    if (candidateRuns_ >= cfg_.confirmRuns) verdict_ = candidate_;
  }

  if (frames_ >= 4 * kWindow) {  // keep FER responsive to the present
    frames_ /= 2;
    failures_ /= 2;
  }
}

double ChannelClassifier::snrVariance() const {
  return n_ >= 2 ? snrM2_ / double(n_ - 1) : 0.0;
}

double ChannelClassifier::frameErrorRate() const {
  return frames_ > 0 ? double(failures_) / double(frames_) : 0.0;
}

ChannelClass ChannelClassifier::classifyNow() const {
  // Diagnostic/safety override: SKW_CHANCLASS=off pins the verdict to
  // Unknown, i.e. the pre-classifier behaviour (always fading floors).
  static const bool disabled = [] {
    const char* e = getenv("SKW_CHANCLASS");
    return e && std::string(e) == "off";
  }();
  if (disabled) return ChannelClass::Unknown;

  const double var = snrVariance();
  const double fer = frameErrorRate();

  // SEVERE first: it is the verdict with real consequences (a mode cap),
  // and its evidence overlaps Fading's. Any TWO of the three signals.
  int severe = 0;
  if (var >= cfg_.severeSnrVarMin) ++severe;
  if (spreadEma_ >= cfg_.severeSpreadMsMin) ++severe;
  if (fer >= cfg_.severeFerMin) ++severe;
  if (pilotEma_ >= cfg_.severePilotStabMin) ++severe;
  if (severe >= 2) return ChannelClass::Severe;

  // STATIC requires everything to be quiet at once.
  if (var <= cfg_.staticSnrVarMax && spreadEma_ <= cfg_.staticSpreadMsMax &&
      pilotEma_ <= cfg_.staticPilotStabMax && fer <= cfg_.staticFerMax) {
    return ChannelClass::Static;
  }
  return ChannelClass::Fading;
}

void ChannelClassifier::reset() {
  n_ = 0;
  snrMean_ = 0.0;
  snrM2_ = 0.0;
  spreadEma_ = 0.0;
  pilotEma_ = 0.0;
  frames_ = 0;
  failures_ = 0;
  verdict_ = ChannelClass::Unknown;
  candidate_ = ChannelClass::Unknown;
  candidateRuns_ = 0;
}

}  // namespace skw
