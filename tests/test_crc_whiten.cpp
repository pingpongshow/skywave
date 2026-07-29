#include <cstring>
#include <set>
#include <vector>

#include "fec/crc.hpp"
#include "fec/whitener.hpp"
#include "skytest.hpp"

using namespace skw;

TEST_CASE(crc16_known_answer) {
  const char* s = "123456789";
  CHECK_EQ(crc16({reinterpret_cast<const uint8_t*>(s), 9}), 0x29B1);
  CHECK_EQ(crc16({reinterpret_cast<const uint8_t*>(""), 0}), 0xFFFF);
}

TEST_CASE(crc32_known_answer) {
  const char* s = "123456789";
  CHECK_EQ(crc32({reinterpret_cast<const uint8_t*>(s), 9}), 0xCBF43926u);
  CHECK_EQ(crc32({reinterpret_cast<const uint8_t*>(""), 0}), 0x00000000u);
}

TEST_CASE(crc_detects_single_bit_errors) {
  std::vector<uint8_t> data(64);
  for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 7 + 3);
  uint32_t ref = crc32(data);
  for (size_t byte = 0; byte < data.size(); byte += 5) {
    for (int bit = 0; bit < 8; bit += 3) {
      data[byte] ^= uint8_t(1 << bit);
      CHECK(crc32(data) != ref);
      data[byte] ^= uint8_t(1 << bit);
    }
  }
}

TEST_CASE(whitener_involutive) {
  std::vector<uint8_t> data(512);
  for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i & 0xFF);
  std::vector<uint8_t> orig = data;
  Whitener w1, w2;
  w1.apply(data);
  CHECK(std::memcmp(data.data(), orig.data(), data.size()) != 0);
  w2.apply(data);
  CHECK(std::memcmp(data.data(), orig.data(), data.size()) == 0);
}

TEST_CASE(whitener_msequence_properties) {
  // x^17+x^12+1 is primitive: state must not repeat within a long window,
  // and output bits must be near-balanced.
  Whitener w;
  std::set<uint32_t> seen;
  int ones = 0;
  constexpr int kBytes = 8192;
  for (int i = 0; i < kBytes; ++i) {
    CHECK(seen.insert(w.state()).second);  // no state repeat
    uint8_t b = w.nextByte();
    for (int k = 0; k < 8; ++k) ones += (b >> k) & 1;
  }
  double balance = double(ones) / (kBytes * 8);
  CHECK(balance > 0.48 && balance < 0.52);
}

TEST_CASE(whitener_breaks_periodicity) {
  // All-zeros payload becomes ~balanced bits on the wire.
  std::vector<uint8_t> zeros(1024, 0);
  Whitener w;
  w.apply(zeros);
  int ones = 0;
  for (uint8_t b : zeros) {
    for (int k = 0; k < 8; ++k) ones += (b >> k) & 1;
  }
  double balance = double(ones) / (1024 * 8);
  CHECK(balance > 0.45 && balance < 0.55);
}

SKYTEST_MAIN
