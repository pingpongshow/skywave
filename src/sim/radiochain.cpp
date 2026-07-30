#include "sim/radiochain.hpp"

#include <algorithm>
#include <cmath>

#include "dsp/fir.hpp"
#include "dsp/updown.hpp"
#include "phy/layout.hpp"  // skwHash

namespace skw {
namespace {

// Real-valued bandpass by lowpass difference, applied by direct FIR.
std::vector<float> bandpassTaps(double lowHz, double highHz) {
  auto lpHigh = designKaiserLowpass(kFsAudio, highHz, highHz + 300.0, 60.0);
  auto lpLow = designKaiserLowpass(kFsAudio, lowHz, lowHz + 200.0, 60.0);
  // Align lengths (pad the shorter, centered) then subtract.
  const size_t n = std::max(lpHigh.size(), lpLow.size());
  std::vector<float> bp(n, 0.0f);
  const size_t offH = (n - lpHigh.size()) / 2;
  const size_t offL = (n - lpLow.size()) / 2;
  for (size_t i = 0; i < lpHigh.size(); ++i) bp[i + offH] += lpHigh[i];
  for (size_t i = 0; i < lpLow.size(); ++i) bp[i + offL] -= lpLow[i];
  return bp;
}

std::vector<float> firApply(const std::vector<float>& taps,
                            std::span<const float> x) {
  std::vector<float> y(x.size() + taps.size(), 0.0f);
  for (size_t i = 0; i < x.size(); ++i) {
    const float xi = x[i];
    if (xi == 0.0f) continue;
    for (size_t t = 0; t < taps.size(); ++t) y[i + t] += xi * taps[t];
  }
  return y;
}

double uniform01(uint64_t& s) {
  s = skwHash(s);
  return (double(s >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

}  // namespace

std::vector<float> radioChain(const RadioChainParams& p,
                              std::span<const float> tx48k) {
  std::vector<float> a(tx48k.begin(), tx48k.end());
  uint64_t rng = skwHash(p.seed ^ 0x52414443484E31ull);  // "RADCHN1"

  // --- TX rig ------------------------------------------------------------
  const size_t lead = size_t(p.pttLeadClipMs * 1e-3 * kFsAudio);
  for (size_t i = 0; i < std::min(lead, a.size()); ++i) a[i] = 0.0f;
  const size_t tail = size_t(p.pttTailClipMs * 1e-3 * kFsAudio);
  for (size_t i = 0; i < std::min(tail, a.size()); ++i) {
    a[a.size() - 1 - i] = 0.0f;
  }
  const float gain = float(std::pow(10.0, p.driveLevelDb / 20.0));
  for (auto& v : a) v *= gain;
  if (p.alcKnee > 0.0) {
    const float k = float(p.alcKnee);
    for (auto& v : a) v = k * std::tanh(v / k);  // soft ALC compression
  }
  {
    auto taps = bandpassTaps(p.txBandLowHz, p.txBandHighHz);
    a = firApply(taps, a);
  }

  // --- path: complex-baseband channel through the REAL converters --------
  {
    Downconverter down;
    std::vector<cfloat> bb;
    down.process(a, bb);
    ChansimParams cp = p.channel;
    cp.seed = p.channel.seed ? p.channel.seed : p.seed;
    auto faded = Chansim::run(cp, bb);
    Upconverter up;
    std::vector<float> back;
    up.process(faded, back);
    a = std::move(back);
  }

  // --- RX rig --------------------------------------------------------------
  {
    auto taps = bandpassTaps(p.rxBandLowHz, p.rxBandHighHz);
    a = firApply(taps, a);
  }
  if (p.humAmp > 0.0 && p.humHz > 0.0) {
    for (size_t i = 0; i < a.size(); ++i) {
      const double t = double(i) / kFsAudio;
      a[i] += float(p.humAmp * (std::sin(2 * M_PI * p.humHz * t) +
                                0.5 * std::sin(4 * M_PI * p.humHz * t)));
    }
  }
  if (p.dcOffset != 0.0) {
    for (auto& v : a) v += float(p.dcOffset);
  }
  if (p.impulsePerSec > 0.0 && p.impulseAmp > 0.0) {
    // Poisson crashes, 1-3 ms of heavy noise each.
    const double perSample = p.impulsePerSec / kFsAudio;
    for (size_t i = 0; i < a.size(); ++i) {
      if (uniform01(rng) < perSample) {
        const size_t len = size_t((1.0 + 2.0 * uniform01(rng)) * 1e-3 *
                                  kFsAudio);
        for (size_t j = i; j < std::min(i + len, a.size()); ++j) {
          a[j] += float(p.impulseAmp * (uniform01(rng) * 2.0 - 1.0));
        }
        i += len;
      }
    }
  }
  if (p.soundcardPpm != 0.0) {
    // RX soundcard clock offset: linear-interpolation resample.
    std::vector<float> r;
    r.reserve(a.size());
    const double step = 1.0 + p.soundcardPpm * 1e-6;
    for (double pos = 0.0; pos + 1.0 < double(a.size()); pos += step) {
      const size_t i = size_t(pos);
      const float fr = float(pos - double(i));
      r.push_back(a[i] + fr * (a[i + 1] - a[i]));
    }
    a = std::move(r);
  }
  return a;
}

}  // namespace skw
