// Sample-domain converters between the 48 kHz real passband and the 12 kHz
// complex internal baseband (DSP.md).
//
// RX: mix by -1500 Hz, then 2-stage polyphase decimation (x2, x2). The
//     stage-2 filter is the sharp one: it must pass the widest family
//     (ULTRA, +-1350 Hz after mixing) while rejecting the negative-frequency
//     image of the real input, whose modem content begins at -(187+1500) =
//     -1687 Hz. Design: pass 1400, stop 1650, 70 dB.
// TX: mirror interpolation (x2, x2) then mix +1500 and take the real part.
//     Images from zero-stuffing sit >= 10 kHz away, so both TX filters are
//     short.
#pragma once

#include <span>
#include <vector>

#include "dsp/fir.hpp"

namespace skw {

class Nco {
 public:
  Nco(double freqHz, double sampleRate);
  cfloat next();
  void reset();

 private:
  double phase_ = 0.0;
  double step_;
};

// 48 kHz real -> 12 kHz complex baseband centered on the waveform center.
class Downconverter {
 public:
  Downconverter();
  void process(std::span<const float> in48k, std::vector<cfloat>& out12k);
  // Total chain group delay expressed in 12 kHz output samples.
  double groupDelay12k() const { return delay12k_; }
  void reset();

 private:
  Nco nco_;
  FirDecimator stage1_;  // 48k -> 24k, mild
  FirDecimator stage2_;  // 24k -> 12k, sharp
  std::vector<cfloat> mixed_;
  std::vector<cfloat> mid_;
  double delay12k_;
};

// 12 kHz complex baseband -> 48 kHz real passband.
class Upconverter {
 public:
  Upconverter();
  void process(std::span<const cfloat> in12k, std::vector<float>& out48k);
  double groupDelay48k() const { return delay48k_; }
  void reset();

 private:
  FirInterpolator stage1_;  // 12k -> 24k
  FirInterpolator stage2_;  // 24k -> 48k
  Nco nco_;
  std::vector<cfloat> mid_;
  std::vector<cfloat> up_;
  double delay48k_;
};

}  // namespace skw
