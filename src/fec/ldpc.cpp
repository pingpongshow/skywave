#include "fec/ldpc.hpp"

#include <algorithm>

#include <cmath>
#include <stdexcept>

namespace skw::ldpc_tables {
extern const int8_t kH648R12[], kH648R23[], kH648R34[], kH648R56[];
extern const int8_t kH1944R12[], kH1944R23[], kH1944R34[], kH1944R56[];
}  // namespace skw::ldpc_tables

namespace skw {
namespace {
constexpr int kBaseCols = 24;
constexpr float kMinSumNorm = 0.8f;  // normalized min-sum scale

int rateRows(CodeRate r) {
  if (r.num == 1 && r.den == 2) return 12;
  if (r.num == 2 && r.den == 3) return 8;
  if (r.num == 3 && r.den == 4) return 6;
  if (r.num == 5 && r.den == 6) return 4;
  return 0;
}
}  // namespace

LdpcCode::LdpcCode(int n, int z, int baseRows, const int8_t* base)
    : n_(n), k_(n - baseRows * z), z_(z), baseRows_(baseRows), base_(base) {
  // Build sparse H edge structure.
  numChecks_ = baseRows_ * z_;
  checkHead_.assign(size_t(numChecks_), -1);
  varDegree_.assign(size_t(n_), 0);
  varEdges_.resize(size_t(n_));
  for (int bi = 0; bi < baseRows_; ++bi) {
    for (int bj = 0; bj < kBaseCols; ++bj) {
      int shift = base_[bi * kBaseCols + bj];
      if (shift < 0) continue;
      for (int r = 0; r < z_; ++r) {
        int check = bi * z_ + r;
        int var = bj * z_ + (r + shift) % z_;
        int idx = int(edges_.size());
        edges_.push_back({var, checkHead_[size_t(check)]});
        checkHead_[size_t(check)] = idx;
        varDegree_[size_t(var)]++;
        varEdges_[size_t(var)].push_back(idx);
      }
    }
  }
}

const LdpcCode& LdpcCode::get(int n, CodeRate rate) {
  using namespace ldpc_tables;
  const int rows = rateRows(rate);
  if (rows == 0) throw std::invalid_argument("ldpc: unsupported rate");
  auto make = [](int nn, int zz, int rr, const int8_t* t) {
    return LdpcCode(nn, zz, rr, t);
  };
  if (n == kLdpcNShort) {
    static const LdpcCode c12 = make(648, 27, 12, kH648R12);
    static const LdpcCode c23 = make(648, 27, 8, kH648R23);
    static const LdpcCode c34 = make(648, 27, 6, kH648R34);
    static const LdpcCode c56 = make(648, 27, 4, kH648R56);
    switch (rows) {
      case 12: return c12;
      case 8: return c23;
      case 6: return c34;
      default: return c56;
    }
  }
  if (n == kLdpcNLong) {
    static const LdpcCode c12 = make(1944, 81, 12, kH1944R12);
    static const LdpcCode c23 = make(1944, 81, 8, kH1944R23);
    static const LdpcCode c34 = make(1944, 81, 6, kH1944R34);
    static const LdpcCode c56 = make(1944, 81, 4, kH1944R56);
    switch (rows) {
      case 12: return c12;
      case 8: return c23;
      case 6: return c34;
      default: return c56;
    }
  }
  throw std::invalid_argument("ldpc: unsupported codeword length");
}

void LdpcCode::encode(std::span<const uint8_t> info,
                      std::vector<uint8_t>& out) const {
  if (int(info.size()) != k_) throw std::invalid_argument("ldpc: info size");
  const int kb = kBaseCols - baseRows_;  // info block-columns
  out.assign(size_t(n_), 0);
  for (int i = 0; i < k_; ++i) out[size_t(i)] = info[size_t(i)] & 1;

  // lambda_i = sum_j P^{a_ij} s_j over info blocks (Z bits per block-row).
  std::vector<uint8_t> lambda(size_t(baseRows_ * z_), 0);
  for (int bi = 0; bi < baseRows_; ++bi) {
    for (int bj = 0; bj < kb; ++bj) {
      int shift = base_[bi * kBaseCols + bj];
      if (shift < 0) continue;
      const uint8_t* s = out.data() + size_t(bj) * size_t(z_);
      uint8_t* lam = lambda.data() + size_t(bi) * size_t(z_);
      for (int r = 0; r < z_; ++r) lam[r] ^= s[(r + shift) % z_];
    }
  }

  // Dual-diagonal parity solve (802.11n structure):
  //   p0 = sum_i lambda_i          (the first-column shifts {1,0,1} cancel)
  //   p_{i+1} = lambda_i + P^{e_i} p0 + p_i
  uint8_t* parity = out.data() + size_t(k_);
  uint8_t* p0 = parity;
  for (int bi = 0; bi < baseRows_; ++bi) {
    const uint8_t* lam = lambda.data() + size_t(bi) * size_t(z_);
    for (int r = 0; r < z_; ++r) p0[r] ^= lam[r];
  }
  for (int bi = 0; bi + 1 < baseRows_; ++bi) {
    const uint8_t* lam = lambda.data() + size_t(bi) * size_t(z_);
    const uint8_t* prev = parity + size_t(bi) * size_t(z_);      // p_i (i>=1)
    uint8_t* next = parity + size_t(bi + 1) * size_t(z_);        // p_{i+1}
    const int e = base_[bi * kBaseCols + kb];
    for (int r = 0; r < z_; ++r) {
      uint8_t v = lam[r];
      if (e >= 0) v ^= p0[(r + e) % z_];
      if (bi >= 1) v ^= prev[r];
      next[r] = v;
    }
  }
}

bool LdpcCode::checkCodeword(std::span<const uint8_t> code) const {
  if (int(code.size()) != n_) return false;
  for (int c = 0; c < numChecks_; ++c) {
    uint8_t sum = 0;
    for (int e = checkHead_[size_t(c)]; e >= 0; e = edges_[size_t(e)].nextInCheck) {
      sum ^= code[size_t(edges_[size_t(e)].var)] & 1;
    }
    if (sum) return false;
  }
  return true;
}

bool LdpcCode::decode(std::span<const float> llr, std::vector<uint8_t>& info,
                      int maxIterations) const {
  if (int(llr.size()) != n_) throw std::invalid_argument("ldpc: llr size");
  const size_t ne = edges_.size();
  std::vector<float> c2v(ne, 0.0f);                    // check-to-variable
  std::vector<float> post(static_cast<size_t>(n_));    // posterior LLRs
  std::vector<uint8_t> hard(static_cast<size_t>(n_));

  auto hardDecideAndCheck = [&]() -> bool {
    for (int v = 0; v < n_; ++v) hard[size_t(v)] = post[size_t(v)] < 0.0f;
    for (int c = 0; c < numChecks_; ++c) {
      uint8_t sum = 0;
      for (int e = checkHead_[size_t(c)]; e >= 0;
           e = edges_[size_t(e)].nextInCheck) {
        sum ^= hard[size_t(edges_[size_t(e)].var)];
      }
      if (sum) return false;
    }
    return true;
  };

  for (int v = 0; v < n_; ++v) post[size_t(v)] = llr[size_t(v)];
  if (hardDecideAndCheck()) {
    info.assign(hard.begin(), hard.begin() + k_);
    return true;
  }

  for (int iter = 0; iter < maxIterations; ++iter) {
    // Check-node update on v2c = post - c2v (computed on the fly).
    for (int c = 0; c < numChecks_; ++c) {
      float min1 = 1e30f, min2 = 1e30f;
      int minEdge = -1;
      uint32_t signBits = 0;
      int deg = 0;
      for (int e = checkHead_[size_t(c)]; e >= 0;
           e = edges_[size_t(e)].nextInCheck, ++deg) {
        float v2c = post[size_t(edges_[size_t(e)].var)] - c2v[size_t(e)];
        float mag = std::fabs(v2c);
        if (v2c < 0.0f) signBits ^= 1u;
        if (mag < min1) {
          min2 = min1;
          min1 = mag;
          minEdge = e;
        } else if (mag < min2) {
          min2 = mag;
        }
      }
      // Second pass: write new c2v and update posteriors incrementally.
      for (int e = checkHead_[size_t(c)]; e >= 0;
           e = edges_[size_t(e)].nextInCheck) {
        int var = edges_[size_t(e)].var;
        float v2c = post[size_t(var)] - c2v[size_t(e)];
        float mag = (e == minEdge) ? min2 : min1;
        uint32_t sign = signBits ^ (v2c < 0.0f ? 1u : 0u);
        float newMsg = kMinSumNorm * (sign ? -mag : mag);
        post[size_t(var)] = v2c + newMsg;
        c2v[size_t(e)] = newMsg;
      }
    }
    if (hardDecideAndCheck()) {
      info.assign(hard.begin(), hard.begin() + k_);
      return true;
    }
  }
  info.assign(hard.begin(), hard.begin() + k_);
  return false;
}

}  // namespace skw
