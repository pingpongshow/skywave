// Watterson HF channel simulator (BUILD_PLAN 1.9, TESTING.md).
//
// Model: two equal-power independent Rayleigh taps (delays 0 and the
// configured delay spread), each a complex Gaussian process with a
// Gaussian Doppler spectrum; plus AWGN (3 kHz-referenced SNR), carrier
// frequency offset, and sample-clock offset. Operates on the 12 kHz
// complex baseband. Fully deterministic per seed.
#pragma once

#include <complex>
#include <cstdint>
#include <span>
#include <vector>

namespace skw {

using cfloat = std::complex<float>;

struct ChansimParams {
  // Channel dynamics. delaySpreadMs = 0 disables the second tap;
  // dopplerSpreadHz = 0 freezes the taps (static channel).
  double delaySpreadMs = 1.0;   // CCIR Moderate default
  double dopplerSpreadHz = 0.5; // two-sigma frequency spread
  bool fading = true;           // false = static unity channel

  // Impairments.
  double snr3kDb = 99.0;        // AWGN, referenced to 3 kHz noise bandwidth
  double refSignalPower = 1.0;  // signal power the SNR refers to
  double cfoHz = 0.0;
  double cfoDriftHzPerSec = 0.0;  // linear CFO drift from t = 0 (LO drift)
  double clockPpm = 0.0;        // sample-clock offset
  double toneHz = 0.0;          // narrowband interferer (baseband offset)
  double toneAmp = 0.0;         // 0 = off

  uint64_t seed = 1;
};

// CCIR 520 presets (delay spread ms / Doppler spread Hz).
ChansimParams ccirGood();      // 0.5 / 0.1
ChansimParams ccirModerate();  // 1.0 / 0.5
ChansimParams ccirPoor();      // 2.0 / 1.0
// VHF-FM data-port channel (WAVEFORM.md Phase 7): flat and near-static
// (Rician K -> inf approximation) - no multipath, no Doppler process;
// AWGN plus whatever CFO/clock impairments the caller sets.
ChansimParams vhfFlat();

class Chansim {
 public:
  explicit Chansim(const ChansimParams& p);

  // Streaming: append processed samples to out (output length can differ
  // from input length when clockPpm != 0).
  void process(std::span<const cfloat> in, std::vector<cfloat>& out);

  // Convenience one-shot.
  static std::vector<cfloat> run(const ChansimParams& p,
                                 std::span<const cfloat> in);

 private:
  cfloat tapGain(int tap);  // advance + interpolate the fading process

  ChansimParams p_;
  int delaySamples_;
  std::vector<cfloat> history_;  // delay line for tap 2
  size_t histPos_ = 0;

  // Fading processes: white Gaussians filtered by a Gaussian-shaped FIR,
  // generated at a low rate and linearly interpolated to 12 kHz.
  struct FadeProc {
    std::vector<cfloat> taps;    // shaping FIR
    std::vector<cfloat> state;   // white noise history
    cfloat cur, next;            // current interpolation endpoints
    double frac = 0.0;
    uint64_t rng;
  };
  FadeProc fade_[2];
  double fadeStep_ = 0.0;        // interpolation step per 12 kHz sample
  cfloat nextLowRate(FadeProc& f);

  // Resampler state (clock offset).
  std::vector<cfloat> resampBuf_;
  double readPos_ = 0.0;

  double noiseStd_ = 0.0;
  uint64_t noiseRng_;
  double cfoPhase_ = 0.0;
  uint64_t cfoN_ = 0;  // samples since start, for the drift term
  double tonePhase_ = 0.0;
  uint64_t samplesOut_ = 0;
};

}  // namespace skw
