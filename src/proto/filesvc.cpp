#include "proto/filesvc.hpp"

#include <algorithm>

#include "core/bytepack.hpp"
#include "fec/crc.hpp"

namespace skw {

std::vector<uint8_t> FileMeta::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u8(kFileRecMeta);
  w.u32le(fileId);
  w.u8(uint8_t(name.size()));
  w.bytes({reinterpret_cast<const uint8_t*>(name.data()), name.size()});
  w.u64le(size);
  w.u64le(mtime);
  w.u32le(crc32);
  w.u16le(chunkSize);
  return out;
}

std::optional<FileMeta> FileMeta::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  if (r.u8() != kFileRecMeta) return std::nullopt;
  FileMeta m;
  m.fileId = r.u32le();
  const size_t nameLen = r.u8();
  std::vector<uint8_t> name(nameLen);
  if (!r.bytes(name)) return std::nullopt;
  m.name.assign(name.begin(), name.end());
  m.size = r.u64le();
  m.mtime = r.u64le();
  m.crc32 = r.u32le();
  m.chunkSize = r.u16le();
  if (!r.ok() || r.remaining() != 0) return std::nullopt;
  if (m.size > kFileMaxSize || m.chunkSize == 0) return std::nullopt;
  return m;
}

std::vector<uint8_t> packFileChunk(uint32_t fileId, uint64_t offset,
                                   std::span<const uint8_t> data) {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u8(kFileRecChunk);
  w.u32le(fileId);
  w.u64le(offset);
  w.bytes(data);
  return out;
}

// --- FileTx -----------------------------------------------------------------

bool FileTx::start(uint32_t fileId, const std::string& name,
                   std::vector<uint8_t> data, uint64_t mtime) {
  if (active_ || name.empty() || name.size() > kFileMaxNameBytes ||
      data.size() > kFileMaxSize) {
    return false;
  }
  meta_.fileId = fileId;
  meta_.name = name;
  meta_.size = data.size();
  meta_.mtime = mtime;
  meta_.crc32 = crc32(data);
  data_ = std::move(data);
  offset_ = 0;
  metaSent_ = false;
  active_ = true;
  return true;
}

std::optional<std::vector<uint8_t>> FileTx::nextMessage() {
  if (!active_) return std::nullopt;
  if (!metaSent_) {
    metaSent_ = true;
    return meta_.pack();
  }
  if (offset_ >= data_.size()) {
    // Zero-length files complete on META alone.
    return std::nullopt;
  }
  const size_t n =
      std::min(size_t(meta_.chunkSize), data_.size() - offset_);
  auto msg = packFileChunk(
      meta_.fileId, offset_,
      {data_.data() + offset_, n});
  offset_ += n;
  return msg;
}

void FileTx::rewindTo(uint64_t offset) {
  if (!active_) return;
  offset_ = size_t(std::min<uint64_t>(offset, data_.size()));
}

// --- FileRx -----------------------------------------------------------------

void FileRx::fail(const char* what) {
  FileRxEvent e;
  e.kind = FileRxEvent::Kind::Error;
  e.fileId = meta_.fileId;
  e.name = meta_.name;
  e.reason = what;
  events_.push_back(std::move(e));
  active_ = false;
  data_.clear();
  checkpoint_ = 0;
}

void FileRx::onMessage(std::span<const uint8_t> msg) {
  if (msg.empty()) return;
  if (msg[0] == kFileRecMeta) {
    auto m = FileMeta::parse(msg);
    if (!m) {
      fail("bad-meta");
      return;
    }
    if (active_ && m->fileId == meta_.fileId) {
      // Re-announced META for the transfer in progress (resume): keep
      // the buffered prefix, expect chunks from our checkpoint.
      events_.push_back({FileRxEvent::Kind::Started, meta_.fileId,
                         meta_.name, "", checkpoint_, false});
      return;
    }
    meta_ = *m;
    active_ = true;
    // No reserve(m->size): the size is the PEER's claim, and reserving it
    // let one frame demand gigabytes. The buffer grows as chunks actually
    // arrive (the overrun check below bounds it by the declared size).
    data_.clear();
    data_.shrink_to_fit();
    checkpoint_ = 0;
    events_.push_back({FileRxEvent::Kind::Started, m->fileId, m->name, "",
                       m->size, false});
    // fall through to the zero-size completion check below
  } else if (msg[0] == kFileRecChunk) {
    if (!active_) return;  // chunk with no META: stale, ignore
    ByteReader r(msg);
    r.u8();
    const uint32_t id = r.u32le();
    const uint64_t off = r.u64le();
    if (!r.ok() || id != meta_.fileId) return;
    if (off > data_.size()) {
      fail("gap");  // ordered transport should make this impossible
      return;
    }
    // Overlap (resume replays from the checkpoint) is fine: keep the
    // already-buffered prefix, append only the new tail.
    const size_t have = data_.size() - size_t(off);
    std::span<const uint8_t> body(msg.data() + 13, msg.size() - 13);
    if (body.size() > have) {
      auto tail = body.subspan(have);
      if (data_.size() + tail.size() > meta_.size) {
        fail("overrun");
        return;
      }
      data_.insert(data_.end(), tail.begin(), tail.end());
    }
    while (data_.size() >= checkpoint_ + kFileCheckpointBytes) {
      checkpoint_ += kFileCheckpointBytes;
      events_.push_back({FileRxEvent::Kind::Checkpoint, meta_.fileId,
                         meta_.name, "", checkpoint_, false});
    }
  } else {
    return;  // unknown record type: ignore, never fatal
  }

  if (active_ && data_.size() == meta_.size) {
    ReceivedFile f;
    f.meta = meta_;
    f.crcOk = crc32(data_) == meta_.crc32;
    f.data = std::move(data_);
    events_.push_back({FileRxEvent::Kind::Done, meta_.fileId, meta_.name,
                       "", meta_.size, f.crcOk});
    completed_ = std::move(f);
    active_ = false;
    data_.clear();
    checkpoint_ = 0;
  }
}

std::vector<FileRxEvent> FileRx::takeEvents() {
  auto out = std::move(events_);
  events_.clear();
  return out;
}

std::optional<ReceivedFile> FileRx::takeCompleted() {
  auto out = std::move(completed_);
  completed_.reset();
  return out;
}

void FileRx::rewindToCheckpoint() {
  if (!active_) return;
  data_.resize(size_t(checkpoint_));
}

void FileRx::reset() {
  active_ = false;
  data_.clear();
  checkpoint_ = 0;
  events_.clear();
  completed_.reset();
}

}  // namespace skw
