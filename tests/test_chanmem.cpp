// Channel memory (ROADMAP 3.5): EMA store, recommendation clamps,
// aging, LRU eviction, persistence, lower-of-two START seeding.
#include <cstdio>
#include <string>

#include "link/chanmem.hpp"
#include "link/handshake.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

ChanMemKey key(uint32_t remote = 0xABCDE, int32_t bucket = 7100) {
  ChanMemKey k;
  k.localHash = 0x12345;
  k.remoteHash = remote;
  k.freqBucketKhz = bucket;
  k.family = 0;
  return k;
}

ChanMemStats stats(uint8_t mode, double retry = 0.0) {
  ChanMemStats s;
  s.stableMode = mode;
  s.snrDb = 15.0;
  s.retryRatio = retry;
  return s;
}

}  // namespace

TEST_CASE(record_recommend_basics) {
  ChannelMemory cm;
  CHECK(!cm.recommend(key(), 0.0, 13).has_value());
  cm.record(key(), stats(9), 100.0);
  auto r = cm.recommend(key(), 130.0, 13);  // 30 s old
  CHECK(r && *r == 9);
  // Distinct keys are independent (bucket, remote, family).
  CHECK(!cm.recommend(key(0xABCDE, 7101), 130.0, 13).has_value());
  CHECK(!cm.recommend(key(0x99999), 130.0, 13).has_value());
  // Config cap clamps.
  CHECK_EQ(int(*cm.recommend(key(), 130.0, 7)), 7);
}

TEST_CASE(retry_and_age_penalties) {
  ChannelMemory cm;
  // Stored retry 0.2: -1 mode.
  cm.record(key(1), stats(9, 0.20), 0.0);
  CHECK_EQ(int(*cm.recommend(key(1), 10.0, 13)), 8);
  // Stored retry 0.35: -2 modes.
  cm.record(key(2), stats(9, 0.35), 0.0);
  CHECK_EQ(int(*cm.recommend(key(2), 10.0, 13)), 7);
  // Age: -1 per 5 min, capped -3.
  cm.record(key(3), stats(10), 0.0);
  CHECK_EQ(int(*cm.recommend(key(3), 6 * 60.0, 13)), 9);    // 6 min
  CHECK_EQ(int(*cm.recommend(key(3), 12 * 60.0, 13)), 8);   // 12 min
  CHECK_EQ(int(*cm.recommend(key(3), 28 * 60.0, 13)), 7);   // capped -3
  // > 30 min: ignored entirely.
  CHECK(!cm.recommend(key(3), 31 * 60.0, 13).has_value());
  // Clamp floor: penalties never go below M4.
  cm.record(key(4), stats(5, 0.4), 0.0);
  CHECK_EQ(int(*cm.recommend(key(4), 20 * 60.0, 13)), 4);
}

TEST_CASE(ema_smoothing) {
  ChannelMemory cm;
  cm.record(key(), stats(8, 0.0), 0.0);
  // A single bad session nudges, not overwrites (alpha 0.2).
  cm.record(key(), stats(8, 0.5), 10.0);
  auto r = cm.recommend(key(), 20.0, 13);
  CHECK(r && *r == 8);  // EMA retry = 0.1 < 0.18: no penalty yet
  cm.record(key(), stats(8, 0.5), 20.0);
  cm.record(key(), stats(8, 0.5), 30.0);
  r = cm.recommend(key(), 40.0, 13);
  CHECK(r && *r == 7);  // EMA now ~0.24: -1
}

TEST_CASE(lru_eviction) {
  ChannelMemory cm;
  for (uint32_t i = 0; i < 70; ++i) {
    cm.record(key(1000 + i), stats(6), double(i));
  }
  CHECK_EQ(int(cm.entries()), int(ChannelMemory::kMaxEntries));
  // Oldest (first) evicted, newest retained.
  CHECK(!cm.recommend(key(1000), 70.0, 13).has_value());
  CHECK(cm.recommend(key(1069), 70.0, 13).has_value());
}

TEST_CASE(persistence_round_trip) {
  std::string path = "/tmp/skw_test_chanmem.bin";
  {
    ChannelMemory cm;
    cm.record(key(1), stats(9, 0.25), 1000.0);
    cm.record(key(2), stats(6), 1000.0);
    CHECK(cm.save(path));
  }
  {
    ChannelMemory cm;
    CHECK(cm.load(path));
    CHECK_EQ(int(cm.entries()), 2);
    auto r = cm.recommend(key(1), 1010.0, 13);
    CHECK(r && *r == 8);  // retry 0.25 survives the round trip
  }
  // Corrupt magic: clean refusal.
  {
    FILE* f = fopen(path.c_str(), "wb");
    fputs("garbage", f);
    fclose(f);
    ChannelMemory cm;
    CHECK(!cm.load(path));
  }
  remove(path.c_str());
}

namespace {
double g_fakeWall = 1.7e9;
double fakeWall() { return g_fakeWall; }
}  // namespace

TEST_CASE(persisted_ages_use_wall_clock) {
  // C3: the engine clock restarts at boot; persisted entries must age by
  // wall time, and a timestamp in the future must never raise the mode.
  std::string path = "/tmp/skw_test_chanmem_wall.bin";
  g_fakeWall = 1.7e9;
  {
    ChannelMemory cm;
    cm.setWallClockForTest(&fakeWall);
    cm.record(key(1), stats(10), 5000.0);  // boot-relative engine time
    CHECK(cm.save(path));
  }
  ChannelMemory cm;
  cm.setWallClockForTest(&fakeWall);
  CHECK(cm.load(path));
  // After a reboot the engine clock is small again; age is wall time.
  g_fakeWall = 1.7e9 + 6 * 60.0;
  CHECK_EQ(int(*cm.recommend(key(1), 3.0, 13)), 9);  // 6 min: -1
  g_fakeWall = 1.7e9 + 2 * 3600.0;
  CHECK(!cm.recommend(key(1), 3.0, 13).has_value());  // expired
  // Slightly in the future (clock skew): no decay, and never a raise.
  g_fakeWall = 1.7e9 - 60.0;
  CHECK_EQ(int(*cm.recommend(key(1), 3.0, 13)), 10);
  // Far in the future: untrustworthy, ignored.
  g_fakeWall = 1.7e9 - 86400.0;
  CHECK(!cm.recommend(key(1), 3.0, 13).has_value());
  // In-process entries still use the caller's clock; a negative age is
  // clamped rather than raising the mode.
  cm.record(key(2), stats(9), 100.0);
  CHECK_EQ(int(*cm.recommend(key(2), 40.0, 13)), 9);
  remove(path.c_str());
}

TEST_CASE(recommend_clamp_with_odd_max_mode) {
  // C2: maxMode 255 ("auto") or below M4 must not hit std::clamp lo>hi.
  ChannelMemory cm;
  cm.record(key(), stats(12), 0.0);
  CHECK_EQ(int(*cm.recommend(key(), 1.0, 255)), 12);
  CHECK_EQ(int(*cm.recommend(key(), 1.0, 2)), 4);
}

TEST_CASE(start_seeding_lower_of_two) {
  // The START proposal takes min(memory recommendation, live quality).
  ChannelMemory cm;
  cm.record(key(), stats(6), 0.0);
  uint8_t live = initialModeFromQuality(Family::Wide, 30.0, 0, 13);  // M10
  auto mem = cm.recommend(key(), 10.0, 13);
  uint8_t proposal = std::min(live, mem.value_or(live));
  CHECK_EQ(int(proposal), 6);
  // Great memory + poor live quality: live wins.
  cm.record(key(), stats(10), 20.0);
  cm.record(key(), stats(10), 21.0);
  live = initialModeFromQuality(Family::Wide, 3.0, 0, 13);
  mem = cm.recommend(key(), 30.0, 13);
  proposal = std::min(live, mem.value_or(live));
  CHECK_EQ(int(proposal), int(live));
}

SKYTEST_MAIN
