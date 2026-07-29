// Little-endian byte packing/parsing helpers (PROTOCOL.md: all multi-byte
// wire fields little-endian unless a format says otherwise).
#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace skw {

class ByteWriter {
 public:
  explicit ByteWriter(std::vector<uint8_t>& out) : out_(out) {}
  void u8(uint8_t v) { out_.push_back(v); }
  void u16le(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
  void u32le(uint32_t v) { u16le(uint16_t(v)); u16le(uint16_t(v >> 16)); }
  void u64le(uint64_t v) { u32le(uint32_t(v)); u32le(uint32_t(v >> 32)); }
  void u24be(uint32_t v) { u8(uint8_t(v >> 16)); u8(uint8_t(v >> 8)); u8(uint8_t(v)); }
  void u16be(uint16_t v) { u8(uint8_t(v >> 8)); u8(uint8_t(v)); }
  void u32be(uint32_t v) { u16be(uint16_t(v >> 16)); u16be(uint16_t(v)); }
  void u64be(uint64_t v) { u32be(uint32_t(v >> 32)); u32be(uint32_t(v)); }
  void bytes(std::span<const uint8_t> b) { out_.insert(out_.end(), b.begin(), b.end()); }
  size_t size() const { return out_.size(); }

 private:
  std::vector<uint8_t>& out_;
};

// Bounds-checked reader: ok() goes false on over-read, reads return 0 after.
class ByteReader {
 public:
  explicit ByteReader(std::span<const uint8_t> in) : in_(in) {}
  bool ok() const { return ok_; }
  size_t remaining() const { return in_.size() - pos_; }

  uint8_t u8() {
    if (!need(1)) return 0;
    return in_[pos_++];
  }
  uint16_t u16le() { uint16_t a = u8(); return uint16_t(a | (u8() << 8)); }
  uint32_t u32le() { uint32_t a = u16le(); return a | (uint32_t(u16le()) << 16); }
  uint64_t u64le() { uint64_t a = u32le(); return a | (uint64_t(u32le()) << 32); }
  uint16_t u16be() { uint16_t a = u8(); return uint16_t((a << 8) | u8()); }
  uint32_t u24be() { uint32_t a = u16be(); return (a << 8) | u8(); }
  uint32_t u32be() { uint32_t a = u16be(); return (a << 16) | u16be(); }
  uint64_t u64be() { uint64_t a = u32be(); return (a << 32) | u32be(); }
  bool bytes(std::span<uint8_t> out) {
    if (!need(out.size())) return false;
    std::memcpy(out.data(), in_.data() + pos_, out.size());
    pos_ += out.size();
    return true;
  }

 private:
  bool need(size_t n) {
    if (!ok_ || in_.size() - pos_ < n) {
      ok_ = false;
      return false;
    }
    return true;
  }
  std::span<const uint8_t> in_;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace skw
