#include "core/bytepack.hpp"
#include "core/clock.hpp"
#include "core/constants.hpp"
#include "skytest.hpp"

using namespace skw;

TEST_CASE(profile_symbol_rates) {
  // WAVEFORM.md: 34.88 / 32.61 / 30.61 sym/s.
  CHECK_NEAR(symbolRate(CpMode::Short), 34.88, 0.01);
  CHECK_NEAR(symbolRate(CpMode::Medium), 32.61, 0.01);
  CHECK_NEAR(symbolRate(CpMode::Long), 30.61, 0.01);
}

TEST_CASE(profile_family_edges) {
  CHECK_NEAR(plan(Family::Wide).firstBin * kSubcarrierHz, 300.0, 1e-9);
  CHECK_NEAR(plan(Family::Wide).lastBin * kSubcarrierHz, 2662.5, 1e-9);
  CHECK_NEAR(plan(Family::Ultra).firstBin * kSubcarrierHz, 187.5, 1e-9);
  CHECK_NEAR(plan(Family::Ultra).lastBin * kSubcarrierHz, 2850.0, 1e-9);
  CHECK(plan(Family::Ultra).optIn);
  CHECK(!plan(Family::Wide).optIn);
}

TEST_CASE(profile_rate_tables) {
  // Spot-check WAVEFORM.md NET tables (values quoted at MEDIUM CP).
  CHECK_NEAR(netRate(Family::Wide, 4, kCpDefault), 114.0, 1.0);     // M4
  CHECK_NEAR(netRate(Family::Wide, 8, kCpDefault), 1826.0, 5.0);    // M8
  CHECK_NEAR(netRate(Family::Wide, 13, kCpDefault), 9131.0, 15.0);  // M13
  CHECK_NEAR(netRate(Family::Mid, 4, kCpDefault), 48.0, 1.0);
  CHECK_NEAR(netRate(Family::Narrow, 4, kCpDefault), 23.0, 1.0);
  CHECK_NEAR(netRate(Family::Ultra, 13, kCpDefault), 10272.0, 20.0);
  // GROSS = NET / rate.
  CHECK_NEAR(grossRate(Family::Wide, 13, kCpDefault), 10957.0, 20.0);
}

TEST_CASE(profile_ladder_monotone) {
  // Active ladder is ordered by throughput and by floor, per family.
  for (int f = 0; f < kNumFamilies; ++f) {
    for (int m = kFirstMode + 1; m < kNumModes; ++m) {
      CHECK(netRate(Family(f), m, kCpDefault) >
            netRate(Family(f), m - 1, kCpDefault));
      CHECK(floorDb(Family(f), m) > floorDb(Family(f), m - 1));
    }
  }
}

TEST_CASE(bytepack_roundtrip) {
  std::vector<uint8_t> buf;
  ByteWriter w(buf);
  w.u8(0xAB);
  w.u16le(0x1234);
  w.u32le(0xDEADBEEF);
  w.u64le(0x0123456789ABCDEFull);
  w.u24be(0xC0FFEE);
  w.u16be(0xBEAD);

  ByteReader r(buf);
  CHECK_EQ(r.u8(), 0xAB);
  CHECK_EQ(r.u16le(), 0x1234);
  CHECK_EQ(r.u32le(), 0xDEADBEEFu);
  CHECK_EQ(r.u64le(), 0x0123456789ABCDEFull);
  CHECK_EQ(r.u24be(), 0xC0FFEEu);
  CHECK_EQ(r.u16be(), 0xBEAD);
  CHECK(r.ok());
  CHECK_EQ(r.remaining(), 0u);
}

TEST_CASE(bytepack_overread_safe) {
  std::vector<uint8_t> buf = {0x01, 0x02};
  ByteReader r(buf);
  CHECK_EQ(r.u16le(), 0x0201);
  CHECK(r.ok());
  CHECK_EQ(r.u32le(), 0u);  // over-read
  CHECK(!r.ok());
  CHECK_EQ(r.u8(), 0);      // stays failed, stays safe
  CHECK(!r.ok());
}

TEST_CASE(fake_clock) {
  FakeClock c(1000);
  CHECK_EQ(c.nowUs(), 1000);
  c.advanceSec(1.5);
  CHECK_EQ(c.nowUs(), 1501000);
}

SKYTEST_MAIN
