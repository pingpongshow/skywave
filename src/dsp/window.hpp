// Edge ramps (chirp preamble reps).
#pragma once

#include <cmath>
#include <vector>

namespace skw {

// Raised-cosine edge ramp of `len` samples rising 0 -> 1 (the 2 ms edges
// of each chirp rep). Falling edge = reversed.
inline std::vector<float> raisedCosineRamp(int len) {
  std::vector<float> r(static_cast<size_t>(len));
  for (int i = 0; i < len; ++i) {
    r[size_t(i)] = 0.5f - 0.5f * float(std::cos(M_PI * (i + 0.5) / len));
  }
  return r;
}

}  // namespace skw
