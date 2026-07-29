// FIR design (Kaiser windowed-sinc) and streaming FIR decimator/interpolator.
#pragma once

#include <complex>
#include <span>
#include <vector>

namespace skw {

using cfloat = std::complex<float>;

// Kaiser lowpass design: passband/stopband edges in Hz at sampleRate, with
// stopband attenuation atten dB. Returns odd-length linear-phase taps with
// the given DC gain.
std::vector<float> designKaiserLowpass(double sampleRate, double passHz,
                                       double stopHz, double attenDb,
                                       double gain = 1.0);

// Streaming FIR + downsample-by-factor on complex samples.
class FirDecimator {
 public:
  FirDecimator(std::vector<float> taps, int factor);
  // Appends outputs to out; input length need not be a multiple of factor.
  void process(std::span<const cfloat> in, std::vector<cfloat>& out);
  int groupDelayInput() const { return int(taps_.size() / 2); }
  void reset();

 private:
  std::vector<float> taps_;
  int factor_;
  std::vector<cfloat> hist_;  // last taps-1 inputs
  int phase_ = 0;             // samples since last emitted output
};

// Streaming upsample-by-factor (zero-stuff) + FIR on complex samples.
class FirInterpolator {
 public:
  FirInterpolator(std::vector<float> taps, int factor);
  // Emits factor outputs per input sample (polyphase - no explicit zeros).
  void process(std::span<const cfloat> in, std::vector<cfloat>& out);
  int groupDelayOutput() const { return int(taps_.size() / 2); }
  void reset();

 private:
  std::vector<float> taps_;
  int factor_;
  std::vector<cfloat> hist_;  // input-rate history
};

}  // namespace skw
