// Payload whitener: LFSR x^17 + x^12 + 1 (WAVEFORM.md), fixed seed shared
// TX/RX. XOR-based, therefore involutive: whiten(whiten(x)) == x.
#pragma once

#include <cstdint>
#include <span>

#include "core/constants.hpp"

namespace skw {

class Whitener {
 public:
  explicit Whitener(uint32_t seed = kSeedWhitener)
      : seed_(seed & 0x1FFFF ? seed & 0x1FFFF : 0x1u), state_(seed_) {}

  void reset() { state_ = seed_; }

  uint8_t nextByte() {
    uint8_t out = 0;
    for (int i = 0; i < 8; ++i) {
      // Fibonacci LFSR, taps at bit 16 (x^17) and bit 11 (x^12).
      uint32_t bit = ((state_ >> 16) ^ (state_ >> 11)) & 1u;
      state_ = ((state_ << 1) | bit) & 0x1FFFF;
      out = uint8_t((out << 1) | bit);
    }
    return out;
  }

  void apply(std::span<uint8_t> data) {
    for (uint8_t& b : data) b ^= nextByte();
  }

  uint32_t state() const { return state_; }

 private:
  uint32_t seed_;
  uint32_t state_;
};

}  // namespace skw
