#include "sim/chansim.hpp"

#include <cmath>

#include "core/constants.hpp"
#include "phy/layout.hpp"  // skwHash

namespace skw {
namespace {

// Low-rate at which the fading processes are generated before linear
// interpolation to 12 kHz. Comfortably above any HF Doppler spread.
constexpr double kFadeRate = 64.0;

// xorshift-based gaussian pair (Box-Muller) with splitmix stream.
double uniform01(uint64_t& s) {
  s = skwHash(s);
  return (double(s >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}
cfloat gaussianPair(uint64_t& s, double std) {
  double u1 = uniform01(s), u2 = uniform01(s);
  double r = std::sqrt(-2.0 * std::log(u1)) * std;
  return cfloat(float(r * std::cos(2 * M_PI * u2)),
                float(r * std::sin(2 * M_PI * u2)));
}

}  // namespace

ChansimParams ccirGood() {
  ChansimParams p;
  p.delaySpreadMs = 0.5;
  p.dopplerSpreadHz = 0.1;
  return p;
}
ChansimParams ccirModerate() {
  ChansimParams p;
  p.delaySpreadMs = 1.0;
  p.dopplerSpreadHz = 0.5;
  return p;
}
ChansimParams ccirPoor() {
  ChansimParams p;
  p.delaySpreadMs = 2.0;
  p.dopplerSpreadHz = 1.0;
  return p;
}

ChansimParams vhfFlat() {
  ChansimParams p;
  p.fading = false;
  p.delaySpreadMs = 0.0;
  p.dopplerSpreadHz = 0.0;
  return p;
}

Chansim::Chansim(const ChansimParams& p) : p_(p) {
  delaySamples_ = int(std::lround(p.delaySpreadMs * 1e-3 * kFsBaseband));
  history_.assign(size_t(std::max(delaySamples_, 1)), cfloat(0, 0));

  // Gaussian shaping FIR at the fade rate. The PROCESS PSD is |H(f)|^2,
  // whose sigma is sigma_H / sqrt(2); the CCIR "frequency spread" is the
  // two-sigma width of the PSD, so sigma_H = (spread/2) * sqrt(2).
  // The 0.05 Hz floor also guards static presets (dopplerSpreadHz = 0
  // with fading off): the FIR length scales as 1/doppler and a 1 mHz
  // floor builds a ~500x-cost filter that is then never used.
  const double sigmaF =
      std::max(p.dopplerSpreadHz / 2.0 * std::sqrt(2.0), 0.05);
  const double sigmaT = kFadeRate / (2.0 * M_PI * sigmaF);  // in fade samples
  const int half = int(std::ceil(4.0 * sigmaT));
  std::vector<cfloat> taps(size_t(2 * half + 1));
  double sumSq = 0.0;
  for (int i = -half; i <= half; ++i) {
    double v = std::exp(-0.5 * (i / sigmaT) * (i / sigmaT));
    taps[size_t(i + half)] = cfloat(float(v), 0.0f);
    sumSq += v * v;
  }
  // Normalize so the filtered process has unit variance, then scale each
  // tap to power 1/2 (two equal-power taps sum to unity).
  const float g = float(1.0 / std::sqrt(sumSq) / std::sqrt(2.0));
  for (auto& t : taps) t *= g;

  fadeStep_ = kFadeRate / kFsBaseband;
  for (int t = 0; t < 2; ++t) {
    fade_[t].taps = taps;
    fade_[t].state.assign(taps.size(), cfloat(0, 0));
    fade_[t].rng = skwHash(p.seed ^ (0xFADE0000ull + uint64_t(t)));
    // Warm the filter state so fading starts in steady state.
    for (size_t i = 0; i < taps.size(); ++i) nextLowRate(fade_[t]);
    fade_[t].cur = nextLowRate(fade_[t]);
    fade_[t].next = nextLowRate(fade_[t]);
    fade_[t].frac = 0.0;
  }

  // AWGN: SNR referenced to 3 kHz noise bandwidth; full-band variance is
  // N0 * fs = refPower / snr / 3000 * 12000.
  const double snr = std::pow(10.0, p.snr3kDb / 10.0);
  noiseStd_ = std::sqrt(p.refSignalPower / snr / 3000.0 * kFsBaseband / 2.0);
  noiseRng_ = skwHash(p.seed ^ 0xA0153ull);
}

cfloat Chansim::nextLowRate(FadeProc& f) {
  // Push one white sample, output the FIR dot product.
  f.state.erase(f.state.begin());
  f.state.push_back(gaussianPair(f.rng, 1.0 / std::sqrt(2.0)));
  cfloat acc(0, 0);
  for (size_t i = 0; i < f.taps.size(); ++i) acc += f.state[i] * f.taps[i];
  return acc;
}

cfloat Chansim::tapGain(int tap) {
  FadeProc& f = fade_[tap];
  cfloat g = f.cur + float(f.frac) * (f.next - f.cur);
  return g;
}

void Chansim::process(std::span<const cfloat> in, std::vector<cfloat>& out) {
  for (const cfloat& s : in) {
    // Advance fading interpolators once per input sample.
    for (int t = 0; t < 2; ++t) {
      fade_[t].frac += fadeStep_;
      if (fade_[t].frac >= 1.0) {
        fade_[t].frac -= 1.0;
        fade_[t].cur = fade_[t].next;
        fade_[t].next = nextLowRate(fade_[t]);
      }
    }

    cfloat y;
    if (p_.fading && delaySamples_ == 0) {
      // delaySpreadMs = 0 is FLAT fading: one tap. (The history buffer
      // has a minimum length of 1, so the two-tap path below silently
      // added a 1-sample echo.) Each tap carries power 1/2, so the lone
      // tap is scaled back to unit mean power.
      y = s * tapGain(0) * float(M_SQRT2);
    } else if (p_.fading) {
      cfloat delayed = history_[histPos_];
      history_[histPos_] = s;
      histPos_ = (histPos_ + 1) % history_.size();
      y = s * tapGain(0) + delayed * tapGain(1);
    } else {
      y = s;
    }

    // CFO.
    if (p_.cfoHz != 0.0 || p_.cfoDriftHzPerSec != 0.0) {
      const double fNow =
          p_.cfoHz + p_.cfoDriftHzPerSec * double(cfoN_++) / kFsBaseband;
      cfoPhase_ += 2.0 * M_PI * fNow / kFsBaseband;
      if (cfoPhase_ < 0.0) cfoPhase_ += 2.0 * M_PI;
      if (cfoPhase_ > 2.0 * M_PI) cfoPhase_ -= 2.0 * M_PI;
      y *= cfloat(float(std::cos(cfoPhase_)), float(std::sin(cfoPhase_)));
    }

    // Narrowband interferer (persistent tone), then AWGN.
    if (p_.toneAmp > 0.0) {
      tonePhase_ += 2.0 * M_PI * p_.toneHz / kFsBaseband;
      if (tonePhase_ > 2.0 * M_PI) tonePhase_ -= 2.0 * M_PI;
      y += float(p_.toneAmp) * cfloat(float(std::cos(tonePhase_)),
                                      float(std::sin(tonePhase_)));
    }
    if (noiseStd_ > 0.0 && p_.snr3kDb < 90.0) {
      y += gaussianPair(noiseRng_, noiseStd_);
    }

    // Sample-clock offset via linear-interpolation resampling.
    if (p_.clockPpm != 0.0) {
      resampBuf_.push_back(y);
      const double step = 1.0 + p_.clockPpm * 1e-6;
      while (readPos_ + 1.0 < double(resampBuf_.size())) {
        size_t i = size_t(readPos_);
        double fr = readPos_ - double(i);
        out.push_back(resampBuf_[i] +
                      float(fr) * (resampBuf_[i + 1] - resampBuf_[i]));
        readPos_ += step;
      }
      // Bound the buffer.
      if (resampBuf_.size() > 4096) {
        size_t drop = resampBuf_.size() - 2048;
        resampBuf_.erase(resampBuf_.begin(), resampBuf_.begin() + long(drop));
        readPos_ -= double(drop);
      }
    } else {
      out.push_back(y);
    }
    ++samplesOut_;
  }
}

std::vector<cfloat> Chansim::run(const ChansimParams& p,
                                 std::span<const cfloat> in) {
  Chansim c(p);
  std::vector<cfloat> out;
  out.reserve(in.size());
  c.process(in, out);
  return out;
}

}  // namespace skw
