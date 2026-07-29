// Bit interleaving within one codeword (WAVEFORM.md): row/column block
// interleaver, fixed 36 columns (648 = 18x36, 1944 = 54x36). Write
// row-major, read column-major.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace skw {

inline constexpr int kInterleaveCols = 36;

// out[i] = in[perm[i]]; interleave uses the forward permutation.
inline void interleaveBits(std::span<const uint8_t> in,
                           std::vector<uint8_t>& out) {
  const int n = int(in.size());
  const int cols = kInterleaveCols;
  const int rows = (n + cols - 1) / cols;
  out.resize(size_t(n));
  int k = 0;
  for (int c = 0; c < cols; ++c) {
    for (int r = 0; r < rows; ++r) {
      int src = r * cols + c;
      if (src < n) out[size_t(k++)] = in[size_t(src)];
    }
  }
}

template <typename T>
inline void deinterleave(std::span<const T> in, std::vector<T>& out) {
  const int n = int(in.size());
  const int cols = kInterleaveCols;
  const int rows = (n + cols - 1) / cols;
  out.resize(size_t(n));
  int k = 0;
  for (int c = 0; c < cols; ++c) {
    for (int r = 0; r < rows; ++r) {
      int dst = r * cols + c;
      if (dst < n) out[size_t(dst)] = in[size_t(k++)];
    }
  }
}

}  // namespace skw
