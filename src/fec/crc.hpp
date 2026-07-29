// CRC-16-CCITT (headers) and CRC-32 IEEE (link frames / payloads).
#pragma once

#include <cstdint>
#include <span>

namespace skw {

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xor-out.
// KAT: "123456789" -> 0x29B1.
uint16_t crc16(std::span<const uint8_t> data);

// CRC-32 IEEE (zlib): reflected poly 0xEDB88320, init/xor-out 0xFFFFFFFF.
// KAT: "123456789" -> 0xCBF43926.
uint32_t crc32(std::span<const uint8_t> data);

}  // namespace skw
