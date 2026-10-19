// Gray-coded constellation mapping (WAVEFORM.md ladder: QPSK, 16QAM, 64QAM,
// and 256QAM for the VHF-FM M14 rung), unit average power. The soft
// demapper is demapLlr (equalizer.hpp).
#pragma once

#include <complex>
#include <cmath>
#include <cstdint>

#include "core/constants.hpp"

namespace skw {

using cfloat = std::complex<float>;

// Per-axis Gray mapping of `bits` bits to amplitude levels:
// 1 bit: {+1,-1}; 2 bits: {+3,+1,-1,-3} Gray-ordered; 3 and 4 bits likewise.
inline float grayAxis(uint32_t v, int bits) {
  // Gray decode to index, then map index i -> (max - 2i).
  uint32_t g = v;
  uint32_t b = 0;
  for (; g; g >>= 1) b ^= g;
  int levels = 1 << bits;
  return float(levels - 1) - 2.0f * float(b);
}

inline cfloat mapSymbol(Constellation c, uint32_t bits) {
  switch (c) {
    case Constellation::Qpsk: {
      // b0 -> I, b1 -> Q
      float i = grayAxis((bits >> 1) & 1, 1);
      float q = grayAxis(bits & 1, 1);
      return cfloat(i, q) / std::sqrt(2.0f);
    }
    case Constellation::Qam16: {
      float i = grayAxis((bits >> 2) & 3, 2);
      float q = grayAxis(bits & 3, 2);
      return cfloat(i, q) / std::sqrt(10.0f);
    }
    case Constellation::Qam64: {
      float i = grayAxis((bits >> 3) & 7, 3);
      float q = grayAxis(bits & 7, 3);
      return cfloat(i, q) / std::sqrt(42.0f);
    }
    case Constellation::Qam256: {
      float i = grayAxis((bits >> 4) & 15, 4);
      float q = grayAxis(bits & 15, 4);
      return cfloat(i, q) / std::sqrt(170.0f);
    }
  }
  return {0, 0};
}

// Hard demap (test support: mapper round trips; the receiver uses demapLlr).
inline uint32_t hardDemap(Constellation c, cfloat s) {
  uint32_t best = 0;
  float bestD = 1e30f;
  const int m = 1 << bitsPerSymbol(c);
  for (int v = 0; v < m; ++v) {
    float d = std::norm(s - mapSymbol(c, uint32_t(v)));
    if (d < bestD) {
      bestD = d;
      best = uint32_t(v);
    }
  }
  return best;
}

}  // namespace skw
