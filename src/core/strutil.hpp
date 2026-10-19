// Small ASCII string helpers shared by the config, control port, worker
// and GUI: case mapping and hex. Header-only.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace skw {

inline char upperAscii(char c) {
  return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
}
inline char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

inline std::string toUpper(std::string s) {
  for (char& c : s) c = upperAscii(c);
  return s;
}
inline std::string toLower(std::string s) {
  for (char& c : s) c = lowerAscii(c);
  return s;
}

// Value of one hex digit, or -1.
inline int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Strict hex decode: an even number of hex digits (either case), nothing
// else. nullopt on any other character or an odd length.
inline std::optional<std::vector<uint8_t>> hexDecode(std::string_view hex) {
  if (hex.size() % 2 != 0) return std::nullopt;
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hexNibble(hex[i]), lo = hexNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out.push_back(uint8_t(hi << 4 | lo));
  }
  return out;
}

// Uppercase hex, two digits per byte.
inline std::string hexEncode(std::span<const uint8_t> bytes) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t b : bytes) {
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 0x0F]);
  }
  return out;
}

}  // namespace skw
