#include "sim/fmradio.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

#include "core/constants.hpp"
#include "dsp/fir.hpp"
#include "phy/layout.hpp"  // skwHash

namespace skw {
namespace {

using cdouble = std::complex<double>;

// Real-valued bandpass by lowpass difference (as radiochain.cpp).
std::vector<float> bandpassTaps(double lowHz, double highHz) {
  auto lpHigh = designKaiserLowpass(kFsAudio, highHz, highHz + 300.0, 60.0);
  auto lpLow = designKaiserLowpass(kFsAudio, lowHz, lowHz + 200.0, 60.0);
  const size_t n = std::max(lpHigh.size(), lpLow.size());
  std::vector<float> bp(n, 0.0f);
  const size_t offH = (n - lpHigh.size()) / 2;
  const size_t offL = (n - lpLow.size()) / 2;
  for (size_t i = 0; i < lpHigh.size(); ++i) bp[i + offH] += lpHigh[i];
  for (size_t i = 0; i < lpLow.size(); ++i) bp[i + offL] -= lpLow[i];
  return bp;
}

template <class T>
std::vector<T> firApply(const std::vector<float>& taps, std::span<const T> x) {
  std::vector<T> y(x.size() + taps.size(), T(0));
  for (size_t i = 0; i < x.size(); ++i) {
    const T xi = x[i];
    if (xi == T(0)) continue;
    for (size_t t = 0; t < taps.size(); ++t) y[i + t] += xi * double(taps[t]);
  }
  return y;
}

double uniform01(uint64_t& s) {
  s = skwHash(s);
  return (double(s >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

double gauss(uint64_t& s) {
  const double u1 = uniform01(s), u2 = uniform01(s);
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
}

// First-order emphasis H(s) = (1 + s*tauZ) / (1 + s*tauP) by the bilinear
// transform, normalised to unity gain at 1 kHz. Pre-emphasis puts the zero
// at the time constant (and a pole far above the audio band); de-emphasis
// is the inverse.
void emphasis(std::vector<double>& x, double tauZ, double tauP) {
  const double K = 2.0 * kFsAudio;
  const double b0 = 1.0 + K * tauZ, b1 = 1.0 - K * tauZ;
  const double a0 = 1.0 + K * tauP, a1 = 1.0 - K * tauP;
  const double w = 2.0 * M_PI * 1000.0;
  const double g1k = std::abs(cdouble(1.0, w * tauZ) / cdouble(1.0, w * tauP));
  double x1 = 0.0, y1 = 0.0;
  for (auto& v : x) {
    const double y = (b0 * v + b1 * x1 - a1 * y1) / a0;
    x1 = v;
    y1 = y;
    v = y / g1k;
  }
}
constexpr double kEmphasisFarPoleSec = 1.0 / (2.0 * M_PI * 15000.0);

}  // namespace

FmRadioParams fmDataPort() { return FmRadioParams{}; }

FmRadioParams fmDataPortNarrow() {
  FmRadioParams p;
  p.ifBwHz = 11000.0;
  p.devHzPerUnit = 2500.0;
  return p;
}

FmRadioParams fmVoicePort() {
  FmRadioParams p;
  p.preEmphasisUs = 750.0;
  p.deEmphasisUs = 750.0;
  p.txLowHz = p.rxLowHz = 300.0;
  p.txHighHz = p.rxHighHz = 3000.0;
  p.devLimitHz = 3500.0;
  return p;
}

std::vector<float> fmRadio(const FmRadioParams& p, std::span<const float> tx48k) {
  uint64_t rng = skwHash(p.seed ^ 0x464D524144494Full);  // "FMRADIO"
  const double fs = kFsAudio;

  // --- TX rig: level, emphasis, limiter, audio (splatter) filter ---------
  // The carrier is keyed carrierLeadMs before the audio; that lead is cut
  // from the output again, so the output keeps the input's timing.
  const size_t lead = size_t(p.carrierLeadMs * 1e-3 * fs);
  std::vector<double> a(lead, 0.0);
  a.insert(a.end(), tx48k.begin(), tx48k.end());
  a.insert(a.end(), size_t(p.carrierTailMs * 1e-3 * fs), 0.0);
  const size_t keyup = std::min(a.size(), size_t(p.keyupMs * 1e-3 * fs));
  std::fill(a.begin(), a.begin() + long(keyup), 0.0);
  const double drive = std::pow(10.0, p.driveLevelDb / 20.0);
  for (auto& v : a) v *= drive;
  if (p.preEmphasisUs > 0.0) {
    emphasis(a, p.preEmphasisUs * 1e-6, kEmphasisFarPoleSec);
  }
  if (p.devLimitHz > 0.0) {
    const double lim = p.devLimitHz / p.devHzPerUnit;
    for (auto& v : a) v = std::clamp(v, -lim, lim);
  }
  a = firApply<double>(bandpassTaps(p.txLowHz, p.txHighHz), a);

  // --- modulator + path --------------------------------------------------
  // Flat fade: sum of sinusoids (Clarke), unit mean power; a Rician LOS
  // term at zero Doppler on top.
  constexpr int kPaths = 16;
  double pathF[kPaths], pathPh[kPaths];
  const double alpha0 = uniform01(rng);
  for (int n = 0; n < kPaths; ++n) {
    pathF[n] = p.flutterHz * std::cos(2.0 * M_PI * (n + alpha0) / kPaths);
    pathPh[n] = 2.0 * M_PI * uniform01(rng);
  }
  const double kLos = std::sqrt(p.ricianK), kNorm = 1.0 / std::sqrt(p.ricianK + 1.0);

  // Noise: the IF filter keeps ifBw/fs of the white noise's power, so the
  // CNR inside the IF is cnrDb for a unit carrier.
  const double noiseVar = std::pow(10.0, -p.cnrDb / 10.0) * fs / p.ifBwHz;
  const double noiseStd = p.cnrDb >= 90.0 ? 0.0 : std::sqrt(noiseVar / 2.0);

  std::vector<cdouble> rf(a.size());
  double phase = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    const double t = double(i) / fs;
    phase += 2.0 * M_PI * (a[i] * p.devHzPerUnit + p.rfOffsetHz) / fs;
    if (phase > M_PI) phase -= 2.0 * M_PI * std::floor((phase + M_PI) / (2.0 * M_PI));
    cdouble s = std::polar(1.0, phase);
    if (p.flutterHz > 0.0) {
      cdouble g(0, 0);
      for (int n = 0; n < kPaths; ++n) {
        g += std::polar(1.0, 2.0 * M_PI * pathF[n] * t + pathPh[n]);
      }
      g /= std::sqrt(double(kPaths));
      s *= (kLos + g) * kNorm;
    }
    if (noiseStd > 0.0) s += cdouble(gauss(rng) * noiseStd, gauss(rng) * noiseStd);
    rf[i] = s;
  }

  // --- RX rig: IF filter, limiter-discriminator ---------------------------
  const auto ifTaps = designKaiserLowpass(fs, p.ifBwHz / 2.0, p.ifBwHz / 2.0 + 2000.0, 60.0);
  rf = firApply<cdouble>(ifTaps, rf);
  std::vector<double> d(rf.size(), 0.0);
  for (size_t i = 1; i < rf.size(); ++i) {
    const double dphi = std::arg(rf[i] * std::conj(rf[i - 1]));
    d[i] = dphi * fs / (2.0 * M_PI) / p.devHzPerUnit;
  }
  if (p.deEmphasisUs > 0.0) {
    emphasis(d, kEmphasisFarPoleSec, p.deEmphasisUs * 1e-6);
  }
  d = firApply<double>(bandpassTaps(p.rxLowHz, p.rxHighHz), d);

  // Carrier drop: the squelch closes, so the IF filter's ramp-out (a
  // click of several times full scale) is never heard.
  const size_t carrierEnd = std::min(d.size(), a.size() + ifTaps.size() / 2);
  std::fill(d.begin() + long(carrierEnd), d.end(), 0.0);
  // Before key-up the receiver is squelched: the IF filter's ramp-in (the
  // carrier-onset click) is inside the cut lead.
  d.erase(d.begin(), d.begin() + long(std::min(lead, d.size())));

  std::vector<float> out;
  if (p.soundcardPpm != 0.0) {
    out.reserve(d.size());
    const double step = 1.0 + p.soundcardPpm * 1e-6;
    for (double pos = 0.0; pos + 1.0 < double(d.size()); pos += step) {
      const size_t i = size_t(pos);
      const double fr = pos - double(i);
      out.push_back(float(d[i] + fr * (d[i + 1] - d[i])));
    }
  } else {
    out.assign(d.begin(), d.end());
  }
  return out;
}

}  // namespace skw
