#include "link/chanclass.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace skw {

const char* toString(ChannelClass c) {
  switch (c) {
    case ChannelClass::Static: return "STATIC";
    case ChannelClass::Fading: return "FADING";
    case ChannelClass::Severe: return "SEVERE";
    case ChannelClass::Moderate: return "MODERATE";
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
                                double pilotStability, bool decodeOk,
                                Family family) {
  family_ = family;
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
  // A negative pilot figure means "no sample" (a frame without a decoded
  // payload has no motion-free noise reference, receiver.cpp): the
  // window holds rather than being diluted toward zero by every ACK.
  if (pilotStability >= 0.0) {
    pilotWin_[size_t(pilotHead_)] = pilotStability;
    pilotHead_ = (pilotHead_ + 1) % kPilotWindow;
    if (pilotN_ < kPilotWindow) ++pilotN_;
  }

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

double ChannelClassifier::pilotMedian() const {
  if (pilotN_ == 0) return -1.0;
  std::array<double, kPilotWindow> v{};
  for (int i = 0; i < pilotN_; ++i) v[size_t(i)] = pilotWin_[size_t(i)];
  std::sort(v.begin(), v.begin() + pilotN_);
  return v[size_t(pilotN_ / 2)];
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
  const double pilot = pilotMedian();  // -1 with no samples yet
  if (pilotN_ > 0 && pilot >= cfg_.severePilotStabMin[size_t(family_)]) {
    ++severe;
  }
  if (severe >= 2) return ChannelClass::Severe;

  // MODERATE: channel motion beyond CCIR Good, read from the pilot
  // excess alone (its noise share is already removed, so it is not an
  // SNR proxy). Checked before STATIC: a Moderate channel at high SNR
  // can look quiet on every other signal.
  if (pilotN_ >= cfg_.moderatePilotMinSamples &&
      pilot >= cfg_.moderatePilotMin[size_t(family_)]) {
    return ChannelClass::Moderate;
  }

  // STATIC requires everything to be quiet at once.
  if (var <= cfg_.staticSnrVarMax && spreadEma_ <= cfg_.staticSpreadMsMax &&
      (pilotN_ == 0 || pilot <= cfg_.staticPilotStabMax) &&
      fer <= cfg_.staticFerMax) {
    return ChannelClass::Static;
  }
  return ChannelClass::Fading;
}

void ChannelClassifier::reset() {
  n_ = 0;
  snrMean_ = 0.0;
  snrM2_ = 0.0;
  spreadEma_ = 0.0;
  pilotN_ = 0;
  pilotHead_ = 0;
  frames_ = 0;
  failures_ = 0;
  verdict_ = ChannelClass::Unknown;
  candidate_ = ChannelClass::Unknown;
  candidateRuns_ = 0;
}

}  // namespace skw
