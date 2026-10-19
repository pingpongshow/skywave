// Channel memory (LINK_ADAPTATION.md): per-peer/frequency/family EMA
// store seeding reconnects. 64 entries, LRU eviction, small binary cache
// persisted across restarts. The recommendation feeds the START
// initial-mode proposal alongside the live quality measurement - the
// LOWER of the two wins (caller applies std::min).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skw {

struct ChanMemKey {
  uint32_t localHash = 0;   // callsignHash24
  uint32_t remoteHash = 0;
  int32_t freqBucketKhz = -1;  // dial/1kHz; -1 = unknown (own bucket)
  uint8_t family = 0;
  bool operator==(const ChanMemKey&) const = default;
};

// What recommend() consumes - nothing else is kept. (SNR, decode-failure
// ratio and selectivity were once stored as well and never read: the
// live handshake measurement already carries them, and a stored
// selectivity could never bind below the M10 initial-proposal cap.)
struct ChanMemStats {
  uint8_t stableMode = 4;
  double retryRatio = 0.0;
};

class ChannelMemory {
 public:
  static constexpr size_t kMaxEntries = 64;
  static constexpr double kEmaAlpha = 0.2;

  // Fold an observation into the entry (EMA; creates/LRU-bumps it).
  void record(const ChanMemKey& key, const ChanMemStats& stats, double now);

  // Recommended starting mode, or nullopt when nothing usable is stored:
  //  -1 mode if stored retry > 0.18, -2 if > 0.30
  //  -1 mode per 5 min of age, capped at -3; entries > 30 min ignored
  //  clamped to [M4, maxMode]
  std::optional<uint8_t> recommend(const ChanMemKey& key, double now,
                                   uint8_t maxMode) const;

  size_t entries() const { return entries_.size(); }

  // Small binary cache (versioned magic; unknown versions ignored).
  // Timestamps are persisted as WALL-CLOCK time: the `now` the engine
  // passes is a monotonic, boot-relative clock, meaningless after a
  // restart (ages went negative, entries never expired and a negative
  // age RAISED the recommended mode).
  bool save(const std::string& path) const;
  bool load(const std::string& path);

  // Wall clock in seconds (system_clock); replaceable for tests.
  static double wallNowSec();
  void setWallClockForTest(double (*fn)()) { wallNow_ = fn; }

 private:
  struct Entry {
    ChanMemKey key;
    ChanMemStats stats;
    double lastUpdate = 0.0;  // caller's clock (this process only)
    double wallUpdate = 0.0;  // wall clock at the same moment
    bool fromDisk = false;    // loaded: only wallUpdate is meaningful
    uint64_t lruStamp = 0;
  };
  double wallNow() const { return wallNow_ ? wallNow_() : wallNowSec(); }
  Entry* find(const ChanMemKey& key);
  const Entry* find(const ChanMemKey& key) const;

  std::vector<Entry> entries_;
  uint64_t lruCtr_ = 0;
  double (*wallNow_)() = nullptr;
};

}  // namespace skw
