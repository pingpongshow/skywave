#include "dsp/updown.hpp"

#include <cmath>

#include "core/constants.hpp"

namespace skw {
namespace {
// RX stage 1 (at 48 kHz, decimating to 24 kHz): pass the full shifted band,
// stop the first alias region (fold at 12 kHz).
std::vector<float> rxStage1Taps() {
  return designKaiserLowpass(48000.0, 2000.0, 9500.0, 70.0);
}
// RX stage 2 (at 24 kHz, decimating to 12 kHz): the sharp image-reject and
// anti-alias filter. Pass +-1400 (covers ULTRA +-1350), stop 1650.
std::vector<float> rxStage2Taps() {
  return designKaiserLowpass(24000.0, 1400.0, 1650.0, 70.0);
}
// TX stage 1 (at 24 kHz, from 12 kHz): kill the zero-stuff image around
// +-(12000 - f). Gain 2 restores amplitude after x2 upsampling.
std::vector<float> txStage1Taps() {
  return designKaiserLowpass(24000.0, 1500.0, 10000.0, 70.0, 2.0);
}
// TX stage 2 (at 48 kHz, from 24 kHz): image around +-(24000 - f).
std::vector<float> txStage2Taps() {
  return designKaiserLowpass(48000.0, 1500.0, 22000.0, 70.0, 2.0);
}
}  // namespace

Nco::Nco(double freqHz, double sampleRate)
    : step_(2.0 * M_PI * freqHz / sampleRate) {}

cfloat Nco::next() {
  cfloat v(float(std::cos(phase_)), float(std::sin(phase_)));
  phase_ += step_;
  if (phase_ > 2.0 * M_PI) phase_ -= 2.0 * M_PI;
  if (phase_ < -2.0 * M_PI) phase_ += 2.0 * M_PI;
  return v;
}

void Nco::reset() { phase_ = 0.0; }

Downconverter::Downconverter()
    : nco_(-kCenterHz, kFsAudio),
      stage1_(rxStage1Taps(), 2),
      stage2_(rxStage2Taps(), 2) {
  // Stage-1 delay (48k samples) /4 -> 12k samples; stage-2 delay (24k) /2.
  delay12k_ = stage1_.groupDelayInput() / 4.0 + stage2_.groupDelayInput() / 2.0;
}

void Downconverter::reset() {
  nco_.reset();
  stage1_.reset();
  stage2_.reset();
}

void Downconverter::process(std::span<const float> in48k,
                            std::vector<cfloat>& out12k) {
  mixed_.clear();
  mixed_.reserve(in48k.size());
  for (float s : in48k) mixed_.push_back(s * nco_.next());
  mid_.clear();
  stage1_.process(mixed_, mid_);
  stage2_.process(mid_, out12k);
}

Upconverter::Upconverter()
    : stage1_(txStage1Taps(), 2),
      stage2_(txStage2Taps(), 2),
      nco_(kCenterHz, kFsAudio) {
  delay48k_ = stage1_.groupDelayOutput() * 2.0 + stage2_.groupDelayOutput() * 1.0;
}

void Upconverter::reset() {
  nco_.reset();
  stage1_.reset();
  stage2_.reset();
}

void Upconverter::process(std::span<const cfloat> in12k,
                          std::vector<float>& out48k) {
  mid_.clear();
  stage1_.process(in12k, mid_);
  up_.clear();
  stage2_.process(mid_, up_);
  out48k.reserve(out48k.size() + up_.size());
  for (const cfloat& c : up_) {
    out48k.push_back((c * nco_.next()).real());
  }
}

}  // namespace skw
