// Window functions and edge ramps.
#pragma once

#include <cmath>
#include <vector>

namespace skw {

// Periodic Hann window (for spectral analysis / waterfall).
inline std::vector<float> hannWindow(int n) {
  std::vector<float> w(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    w[size_t(i)] = 0.5f - 0.5f * float(std::cos(2.0 * M_PI * i / n));
  }
  return w;
}

// 4-term Blackman-Harris (92 dB sidelobes) - noise floor estimation.
inline std::vector<float> blackmanHarrisWindow(int n) {
  constexpr double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
  std::vector<float> w(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    double x = 2.0 * M_PI * i / (n - 1);
    w[size_t(i)] = float(a0 - a1 * std::cos(x) + a2 * std::cos(2 * x) -
                         a3 * std::cos(3 * x));
  }
  return w;
}

// Raised-cosine edge ramp of `len` samples rising 0 -> 1 (DSP.md: 2 ms
// overlap between TX symbols). Falling edge = reversed.
inline std::vector<float> raisedCosineRamp(int len) {
  std::vector<float> r(static_cast<size_t>(len));
  for (int i = 0; i < len; ++i) {
    r[size_t(i)] = 0.5f - 0.5f * float(std::cos(M_PI * (i + 0.5) / len));
  }
  return r;
}

}  // namespace skw
