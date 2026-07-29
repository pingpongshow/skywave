#include "link/chanmem.hpp"

#include <algorithm>
#include <cstdio>

#include "core/bytepack.hpp"
#include "core/constants.hpp"

namespace skw {

namespace {
constexpr uint32_t kCacheMagic = 0x534B434Du;  // "SKCM"
constexpr uint16_t kCacheVersion = 1;
}  // namespace

ChannelMemory::Entry* ChannelMemory::find(const ChanMemKey& key) {
  for (auto& e : entries_) {
    if (e.key == key) return &e;
  }
  return nullptr;
}

const ChannelMemory::Entry* ChannelMemory::find(const ChanMemKey& key) const {
  for (const auto& e : entries_) {
    if (e.key == key) return &e;
  }
  return nullptr;
}

void ChannelMemory::record(const ChanMemKey& key, const ChanMemStats& s,
                           double now) {
  Entry* e = find(key);
  if (!e) {
    if (entries_.size() >= kMaxEntries) {
      auto oldest = std::min_element(
          entries_.begin(), entries_.end(),
          [](const Entry& a, const Entry& b) {
            return a.lruStamp < b.lruStamp;
          });
      entries_.erase(oldest);
    }
    entries_.push_back(Entry{key, s, now, ++lruCtr_});
    return;
  }
  auto ema = [](double& acc, double v) { acc += kEmaAlpha * (v - acc); };
  // Mode is a discrete "last stable" value, not an average.
  e->stats.stableMode = s.stableMode;
  ema(e->stats.snrDb, s.snrDb);
  ema(e->stats.retryRatio, s.retryRatio);
  ema(e->stats.decodeFailRatio, s.decodeFailRatio);
  e->stats.selectivity = uint8_t(
      e->stats.selectivity + kEmaAlpha * (s.selectivity -
                                          e->stats.selectivity));
  e->lastUpdate = now;
  e->lruStamp = ++lruCtr_;
}

std::optional<uint8_t> ChannelMemory::recommend(const ChanMemKey& key,
                                                double now,
                                                uint8_t maxMode) const {
  const Entry* e = find(key);
  if (!e) return std::nullopt;
  const double ageMin = (now - e->lastUpdate) / 60.0;
  if (ageMin > 30.0) return std::nullopt;
  int m = e->stats.stableMode;
  if (e->stats.retryRatio > 0.30) {
    m -= 2;
  } else if (e->stats.retryRatio > 0.18) {
    m -= 1;
  }
  m -= std::min(int(ageMin / 5.0), 3);
  m = std::clamp(m, int(kFirstMode), int(maxMode));
  return uint8_t(m);
}

bool ChannelMemory::save(const std::string& path) const {
  std::vector<uint8_t> buf;
  ByteWriter w(buf);
  w.u32le(kCacheMagic);
  w.u16le(kCacheVersion);
  w.u16le(uint16_t(entries_.size()));
  for (const auto& e : entries_) {
    w.u32le(e.key.localHash);
    w.u32le(e.key.remoteHash);
    w.u32le(uint32_t(e.key.freqBucketKhz));
    w.u8(e.key.family);
    w.u8(e.stats.stableMode);
    w.u32le(uint32_t(int32_t(e.stats.snrDb * 100)));
    w.u16le(uint16_t(e.stats.retryRatio * 10000));
    w.u16le(uint16_t(e.stats.decodeFailRatio * 10000));
    w.u8(e.stats.selectivity);
    w.u64le(uint64_t(int64_t(e.lastUpdate * 1000)));
  }
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  size_t n = fwrite(buf.data(), 1, buf.size(), f);
  fclose(f);
  return n == buf.size();
}

bool ChannelMemory::load(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  std::vector<uint8_t> buf(size_t(64 + kMaxEntries * 40));
  size_t n = fread(buf.data(), 1, buf.size(), f);
  fclose(f);
  buf.resize(n);
  ByteReader r(buf);
  if (r.u32le() != kCacheMagic || r.u16le() != kCacheVersion) return false;
  uint16_t count = r.u16le();
  entries_.clear();
  for (uint16_t i = 0; i < count && r.ok(); ++i) {
    Entry e;
    e.key.localHash = r.u32le();
    e.key.remoteHash = r.u32le();
    e.key.freqBucketKhz = int32_t(r.u32le());
    e.key.family = r.u8();
    e.stats.stableMode = r.u8();
    e.stats.snrDb = int32_t(r.u32le()) / 100.0;
    e.stats.retryRatio = r.u16le() / 10000.0;
    e.stats.decodeFailRatio = r.u16le() / 10000.0;
    e.stats.selectivity = r.u8();
    e.lastUpdate = double(int64_t(r.u64le())) / 1000.0;
    e.lruStamp = ++lruCtr_;
    if (!r.ok()) break;
    if (entries_.size() < kMaxEntries) entries_.push_back(std::move(e));
  }
  return true;
}

}  // namespace skw
