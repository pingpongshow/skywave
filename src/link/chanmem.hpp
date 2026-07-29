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

struct ChanMemStats {
  uint8_t stableMode = 4;
  double snrDb = 0.0;
  double retryRatio = 0.0;
  double decodeFailRatio = 0.0;
  uint8_t selectivity = 0;
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
  bool save(const std::string& path) const;
  bool load(const std::string& path);

 private:
  struct Entry {
    ChanMemKey key;
    ChanMemStats stats;
    double lastUpdate = 0.0;
    uint64_t lruStamp = 0;
  };
  Entry* find(const ChanMemKey& key);
  const Entry* find(const ChanMemKey& key) const;

  std::vector<Entry> entries_;
  uint64_t lruCtr_ = 0;
};

}  // namespace skw
