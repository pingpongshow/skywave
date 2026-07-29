#include "fec/crc.hpp"

namespace skw {

uint16_t crc16(std::span<const uint8_t> data) {
  uint16_t crc = 0xFFFF;
  for (uint8_t b : data) {
    crc ^= uint16_t(b) << 8;
    for (int i = 0; i < 8; ++i) {
      crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
  }
  return crc;
}

uint32_t crc32(std::span<const uint8_t> data) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (uint8_t b : data) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace skw
