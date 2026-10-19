#include "proto/pipeline.hpp"

#include <zstd.h>

#include <algorithm>

#include "link/frame.hpp"  // kFlagMoreFragments, kFrameOverhead
#include "phy/framer.hpp"

namespace skw {

std::vector<uint8_t> zstdPack(std::span<const uint8_t> in) {
  std::vector<uint8_t> out(ZSTD_compressBound(in.size()));
  const size_t n = ZSTD_compress(out.data(), out.size(), in.data(),
                                 in.size(), 9);
  if (ZSTD_isError(n)) return {};  // caller falls back to stored
  out.resize(n);
  return out;
}

std::optional<std::vector<uint8_t>> zstdUnpack(std::span<const uint8_t> in,
                                               size_t cap) {
  const unsigned long long sz =
      ZSTD_getFrameContentSize(in.data(), in.size());
  // Our compressor is single-shot, so the content size is always present;
  // unknown/error means a foreign or corrupt stream - reject.
  if (sz == ZSTD_CONTENTSIZE_UNKNOWN || sz == ZSTD_CONTENTSIZE_ERROR ||
      sz > cap) {
    return std::nullopt;
  }
  std::vector<uint8_t> out(static_cast<size_t>(sz));
  const size_t n =
      ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
  if (ZSTD_isError(n) || n != out.size()) return std::nullopt;
  return out;
}

int payloadCodewordCount(size_t phyPayloadBytes, int mode) {
  const CodeRate rate = kModes[size_t(mode)].rate;
  const size_t piggy =
      std::min(phyPayloadBytes, size_t(kHeaderPiggybackBytes));
  size_t remaining = (phyPayloadBytes - piggy) * 8;
  int count = 0;
  while (remaining > 0) {
    const int n = payloadCodewordN(remaining, rate);
    const size_t k = size_t(n) * rate.num / rate.den;
    remaining -= std::min(remaining, k);
    ++count;
  }
  return count;
}

size_t dataPayloadBudget(int mode, int framesPerBurst, int codewordsTarget) {
  // PHY payload length as a function of the per-frame DATA payload size:
  // solo frame = envelope + rv + payload + CRC; container entries add a
  // u16 length prefix each (engine burst container, PHY frameType 62).
  static_assert(dataFrameOverhead(1) == kFrameOverhead + 1);
  const size_t perFrame = dataFrameOverhead(framesPerBurst);
  const bool css = kModes[size_t(mode)].waveform == Waveform::Css;
  size_t best = 1;
  for (size_t p = 1;; ++p) {
    const size_t phyLen = size_t(framesPerBurst) * (perFrame + p);
    if (phyLen > 4095) break;  // PHY header length field is 12 bits
    // CSS transport counts N=648 codewords (framer.hpp rule); OFDM
    // counts by the 648/1944 sizing rule.
    const int cw = css ? int(cssPayloadCodewords(phyLen))
                       : payloadCodewordCount(phyLen, mode);
    if (cw > codewordsTarget) break;
    best = p;
  }
  return best;
}

// --- ProtoTx ----------------------------------------------------------------

void ProtoTx::submit(std::span<const uint8_t> bytes, double now) {
  if (bytes.empty()) return;
  if (coalesce_.empty()) deadline_ = now + kCoalesceWindowSec;
  coalesce_.insert(coalesce_.end(), bytes.begin(), bytes.end());
}

void ProtoTx::seal(std::span<const uint8_t> raw) {
  // Message body = [service tag][bytes]; compression covers the whole
  // body, the marker byte stays outermost.
  std::vector<uint8_t> body;
  body.reserve(1 + raw.size());
  body.push_back(kSvcStream);
  body.insert(body.end(), raw.begin(), raw.end());
  Msg m;
  m.rawBytes = raw.size();
  if (zstd_) {
    auto z = zstdPack(body);
    if (!z.empty() && z.size() + kCompressMinSaving <= body.size()) {
      m.bytes.reserve(1 + z.size());
      m.bytes.push_back(kProtoMarkerZstd);
      m.bytes.insert(m.bytes.end(), z.begin(), z.end());
      msgs_.push_back(std::move(m));
      return;
    }
  }
  m.bytes.reserve(1 + body.size());
  m.bytes.push_back(kProtoMarkerStored);
  m.bytes.insert(m.bytes.end(), body.begin(), body.end());
  msgs_.push_back(std::move(m));
}

void ProtoTx::maybeSeal(double now) {
  const size_t cap = std::min(coalesceMaxBytes(), sealCap_);
  while (coalesce_.size() >= cap) {
    std::vector<uint8_t> chunk(coalesce_.begin(),
                               coalesce_.begin() + long(cap));
    coalesce_.erase(coalesce_.begin(), coalesce_.begin() + long(cap));
    seal(chunk);
  }
  if (!coalesce_.empty() && now >= deadline_) {
    seal(coalesce_);
    coalesce_.clear();
  }
}

std::optional<ProtoSegment> ProtoTx::nextSegment(size_t budget, double now) {
  if (budget == 0) return std::nullopt;
  maybeSeal(now);
  if (msgs_.empty()) return std::nullopt;
  Msg& m = msgs_.front();
  const size_t n = std::min(budget, m.bytes.size() - m.off);
  ProtoSegment s;
  const bool last = (m.off + n == m.bytes.size());
  if (!last) s.flags |= kFlagMoreFragments;
  s.bytes.assign(m.bytes.begin() + long(m.off),
                 m.bytes.begin() + long(m.off + n));
  m.off += n;
  if (last) {
    // Handed out in full, but NOT yet known to have arrived. Hold it,
    // rewound, until the ARQ confirms it (or a fresh peer needs it
    // replayed). See the header for why this is message-granular.
    Msg done = std::move(msgs_.front());
    msgs_.pop_front();
    done.off = 0;
    unconfirmed_.push_back(std::move(done));
  }
  return s;
}

void ProtoTx::confirmMessages(size_t n) {
  while (n-- > 0 && !unconfirmed_.empty()) unconfirmed_.pop_front();
}

void ProtoTx::replayUnconfirmed() {
  // The message at the head of msgs_ is part-way out; its earlier
  // fragments went to a peer that no longer exists, so it restarts too.
  if (!msgs_.empty()) msgs_.front().off = 0;
  while (!unconfirmed_.empty()) {
    msgs_.push_front(std::move(unconfirmed_.back()));
    unconfirmed_.pop_back();
  }
}

size_t ProtoTx::unconfirmedBytes() const {
  size_t n = 0;
  for (const auto& m : unconfirmed_) n += m.rawBytes;
  return n;
}

size_t ProtoTx::backlogBytes() const {
  size_t n = coalesce_.size();
  for (const auto& m : msgs_) n += m.bytes.size() - m.off;
  return n;
}

void ProtoTx::reset() {
  coalesce_.clear();
  msgs_.clear();
  unconfirmed_.clear();
  deadline_ = 0.0;
}

// --- ProtoRx ----------------------------------------------------------------

void ProtoRx::onSegment(uint8_t flags, std::span<const uint8_t> bytes) {
  if (resync_) {
    // Remainder of a chain already dropped as oversize: discard through
    // its final fragment, or the tail would be parsed as a new message
    // (its first byte taken as a marker) and corrupt the next one too.
    if (!(flags & kFlagMoreFragments)) resync_ = false;
    return;
  }
  if (assembly_.size() + bytes.size() > kProtoDecompressCap) {
    // Runaway more-fragments chain: drop the whole message.
    assembly_.clear();
    ++bad_;
    resync_ = (flags & kFlagMoreFragments) != 0;
    return;
  }
  assembly_.insert(assembly_.end(), bytes.begin(), bytes.end());
  if (flags & kFlagMoreFragments) return;  // chain continues
  if (assembly_.empty()) {
    ++bad_;
    return;
  }
  const uint8_t marker = assembly_[0];
  std::span<const uint8_t> body(assembly_.data() + 1,
                                assembly_.size() - 1);
  if (marker == kProtoMarkerStored) {
    deliver(body);
  } else if (marker == kProtoMarkerZstd) {
    if (auto raw = zstdUnpack(body)) {
      deliver(*raw);
    } else {
      ++bad_;
    }
  } else {
    ++bad_;
  }
  assembly_.clear();
}

void ProtoRx::deliver(std::span<const uint8_t> message) {
  if (message.empty()) {  // missing service tag
    ++bad_;
    return;
  }
  const uint8_t tag = message[0];
  auto payload = message.subspan(1);
  if (tag == kSvcStream) {
    out_.insert(out_.end(), payload.begin(), payload.end());
  } else {
    ++bad_;  // unknown service
  }
}

std::vector<uint8_t> ProtoRx::takeBytes() {
  auto out = std::move(out_);
  out_.clear();
  return out;
}

}  // namespace skw
