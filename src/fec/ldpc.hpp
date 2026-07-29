// IEEE 802.11n QC-LDPC encoder/decoder (WAVEFORM.md FEC).
//
// Codewords N=648 (Z=27) and N=1944 (Z=81), rates 1/2, 2/3, 3/4, 5/6.
// Encoder exploits the standard's dual-diagonal parity structure; decoder is
// flooding normalized min-sum with early termination on parity check.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/constants.hpp"

namespace skw {

class LdpcCode {
 public:
  // n in {648, 1944}; rate one of {1,2},{2,3},{3,4},{5,6}.
  // Returns a shared immutable instance; throws on unsupported parameters.
  static const LdpcCode& get(int n, CodeRate rate);

  int n() const { return n_; }
  int k() const { return k_; }
  int z() const { return z_; }

  // info.size() == k(); one bit per byte (0/1). out receives n() bits.
  void encode(std::span<const uint8_t> info, std::vector<uint8_t>& out) const;

  // llr.size() == n(); positive LLR = bit 0 more likely. info receives k()
  // decoded bits. Returns true when all parity checks were satisfied.
  bool decode(std::span<const float> llr, std::vector<uint8_t>& info,
              int maxIterations = 30) const;

  // Direct parity verification (test support): true iff H*c == 0.
  bool checkCodeword(std::span<const uint8_t> code) const;

 private:
  LdpcCode(int n, int z, int baseRows, const int8_t* base);

  int n_, k_, z_;
  int baseRows_;                 // parity block-rows (24 block-cols always)
  const int8_t* base_;           // base matrix, -1 = zero block

  // Sparse H as edge lists (built once at construction).
  struct Edge {
    int32_t var;
    int32_t nextInCheck;  // linked list index or -1
  };
  int numChecks_ = 0;
  std::vector<int32_t> checkHead_;       // first edge index per check
  std::vector<int32_t> varDegree_;
  std::vector<Edge> edges_;
  std::vector<std::vector<int32_t>> varEdges_;  // edge indices per variable
};

}  // namespace skw
