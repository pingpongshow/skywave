#include <thread>
#include <vector>

#include "io/ring.hpp"
#include "skytest.hpp"

using namespace skw;

TEST_CASE(ring_basic_write_read) {
  SampleRing r(64);
  std::vector<float> in = {1, 2, 3, 4, 5};
  CHECK_EQ(r.write(in), 5u);
  CHECK_EQ(r.readAvailable(), 5u);
  std::vector<float> out(5);
  CHECK_EQ(r.read(out), 5u);
  for (int i = 0; i < 5; ++i) CHECK_NEAR(out[size_t(i)], in[size_t(i)], 0.0f);
  CHECK_EQ(r.readAvailable(), 0u);
}

TEST_CASE(ring_wraparound) {
  SampleRing r(8);  // rounds to 16 slots, 15 usable
  std::vector<float> chunk(6);
  std::vector<float> out(6);
  for (int pass = 0; pass < 100; ++pass) {
    for (int i = 0; i < 6; ++i) chunk[size_t(i)] = float(pass * 6 + i);
    CHECK_EQ(r.write(chunk), 6u);
    CHECK_EQ(r.read(out), 6u);
    for (int i = 0; i < 6; ++i) CHECK_NEAR(out[size_t(i)], chunk[size_t(i)], 0.0f);
  }
}

TEST_CASE(ring_overflow_drops) {
  SampleRing r(8);  // 15 usable
  std::vector<float> big(100, 1.0f);
  size_t wrote = r.write(big);
  CHECK_EQ(wrote, 15u);
  CHECK_EQ(r.writeAvailable(), 0u);
}

TEST_CASE(ring_spsc_threads) {
  // Producer and consumer on separate threads; verify sequence integrity.
  constexpr size_t kTotal = 500000;
  SampleRing r(1024);
  std::vector<float> received;
  received.reserve(kTotal);

  std::thread consumer([&] {
    std::vector<float> buf(256);
    while (received.size() < kTotal) {
      size_t got = r.read(buf);
      for (size_t i = 0; i < got; ++i) received.push_back(buf[i]);
      if (got == 0) std::this_thread::yield();
    }
  });

  std::vector<float> chunk(128);
  size_t sent = 0;
  while (sent < kTotal) {
    size_t n = std::min(chunk.size(), kTotal - sent);
    for (size_t i = 0; i < n; ++i) chunk[i] = float(sent + i);
    size_t put = r.write({chunk.data(), n});
    sent += put;
    if (put == 0) std::this_thread::yield();
  }
  consumer.join();

  CHECK_EQ(received.size(), kTotal);
  bool ordered = true;
  for (size_t i = 0; i < received.size(); ++i) {
    if (received[i] != float(i)) { ordered = false; break; }
  }
  CHECK(ordered);
}

SKYTEST_MAIN
