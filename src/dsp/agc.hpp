// RX AGC and DC blocker (DSP.md: target -18 dBFS RMS, attack 10 ms,
// release 500 ms, freeze during detected frames).
#pragma once

#include <cmath>
#include <span>

namespace skw {

// One-pole DC blocker: y[n] = x[n] - x[n-1] + a*y[n-1] (~7 Hz at 48 kHz).
class DcBlocker {
 public:
  explicit DcBlocker(float a = 0.999f) : a_(a) {}
  void process(std::span<float> buf) {
    for (float& s : buf) {
      float y = s - x1_ + a_ * y1_;
      x1_ = s;
      y1_ = y;
      s = y;
    }
  }
  void reset() { x1_ = y1_ = 0.0f; }

 private:
  float a_;
  float x1_ = 0.0f;
  float y1_ = 0.0f;
};

class Agc {
 public:
  Agc(double sampleRate, double targetDbfs = -18.0, double attackMs = 10.0,
      double releaseMs = 500.0, double maxGainDb = 40.0)
      : target_(float(std::pow(10.0, targetDbfs / 20.0))),
        attackCoef_(float(std::exp(-1.0 / (attackMs * 1e-3 * sampleRate)))),
        releaseCoef_(float(std::exp(-1.0 / (releaseMs * 1e-3 * sampleRate)))),
        maxGain_(float(std::pow(10.0, maxGainDb / 20.0))) {}

  void setFrozen(bool f) { frozen_ = f; }
  bool frozen() const { return frozen_; }
  float gain() const { return gain_; }
  float envelope() const { return env_; }
  float inputLevelDbfs() const {
    return env_ > 1e-9f ? 20.0f * std::log10(env_) : -120.0f;
  }

  void process(std::span<float> buf) {
    for (float& s : buf) {
      float mag = std::fabs(s);
      // Envelope: fast attack on rising signal, slow release on falling.
      float coef = mag > env_ ? attackCoef_ : releaseCoef_;
      env_ = coef * env_ + (1.0f - coef) * mag;
      if (!frozen_) {
        float desired = env_ > 1e-6f ? target_ / env_ : maxGain_;
        if (desired > maxGain_) desired = maxGain_;
        if (desired < minGain_) desired = minGain_;
        // Gain reductions track the attack rate, increases the release rate.
        float gcoef = desired < gain_ ? attackCoef_ : releaseCoef_;
        gain_ = gcoef * gain_ + (1.0f - gcoef) * desired;
      }
      s *= gain_;
    }
  }

  void reset() {
    env_ = 0.0f;
    gain_ = 1.0f;
    frozen_ = false;
  }

 private:
  float target_;
  float attackCoef_;
  float releaseCoef_;
  float maxGain_;
  float minGain_ = 0.05f;  // -26 dB: never fully mutes strong signals
  float env_ = 0.0f;
  float gain_ = 1.0f;
  bool frozen_ = false;
};

}  // namespace skw
