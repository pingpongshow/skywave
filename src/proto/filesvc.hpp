// File-transfer service (PROTOCOL.md "File Transfer"): rides the proto
// pipeline as kSvcFile messages. Wire records (LE):
//
//   META  = 0x01: file_id u32, name_len u8, name (UTF-8, <= 255),
//                 size u64, mtime u64, crc32 u32, chunk_size u16
//   CHUNK = 0x02: file_id u32, offset u64, data...
//
// ARQ + the proto layer make the message stream ordered and lossless
// within a session, so the receiver appends in order. Every 32 KiB of
// contiguous delivered bytes advances a checkpoint (file_id, offset) -
// the state session resume (4.4) continues from across reconnects.
// Pure logic; persistence of checkpoints to disk is worker territory.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace skw {

inline constexpr uint8_t kFileRecMeta = 0x01;
inline constexpr uint8_t kFileRecChunk = 0x02;
inline constexpr size_t kFileCheckpointBytes = 32 * 1024;
inline constexpr size_t kFileMaxNameBytes = 255;
inline constexpr uint64_t kFileMaxSize = 1ull << 32;  // sanity cap

struct FileMeta {
  uint32_t fileId = 0;
  std::string name;
  uint64_t size = 0;
  uint64_t mtime = 0;
  uint32_t crc32 = 0;
  uint16_t chunkSize = 1024;
  std::vector<uint8_t> pack() const;
  static std::optional<FileMeta> parse(std::span<const uint8_t> b);
};

std::vector<uint8_t> packFileChunk(uint32_t fileId, uint64_t offset,
                                   std::span<const uint8_t> data);

// --- sender -----------------------------------------------------------------
// One file at a time (v1). Produces META then sequential CHUNK messages;
// the engine pulls at its own pace (TX flow control).
class FileTx {
 public:
  // Returns false (and stays idle) if the name is oversized or a
  // transfer is already active.
  bool start(uint32_t fileId, const std::string& name,
             std::vector<uint8_t> data, uint64_t mtime);
  bool active() const { return active_; }
  uint32_t fileId() const { return meta_.fileId; }
  const std::string& name() const { return meta_.name; }
  // All messages produced (transfer fully handed to the pipeline)?
  bool drained() const { return active_ && offset_ >= data_.size(); }
  // Next kSvcFile message, or nullopt when everything was produced.
  std::optional<std::vector<uint8_t>> nextMessage();
  // Resume support: restart chunk production at a checkpoint offset.
  void rewindTo(uint64_t offset);
  void finish() { active_ = false; }
  void abort() { active_ = false; }

 private:
  bool active_ = false;
  bool metaSent_ = false;
  FileMeta meta_;
  std::vector<uint8_t> data_;
  size_t offset_ = 0;
};

// --- receiver ---------------------------------------------------------------
struct FileRxEvent {
  enum class Kind { Started, Checkpoint, Done, Error } kind;
  uint32_t fileId = 0;
  std::string name;     // file name (may be empty if META never parsed)
  std::string reason;   // Error only
  uint64_t offset = 0;  // checkpoint offset / final size
  bool crcOk = false;   // Done only
};

struct ReceivedFile {
  FileMeta meta;
  std::vector<uint8_t> data;
  bool crcOk = false;
};

class FileRx {
 public:
  // Feed one kSvcFile message; emits events into the internal queue.
  void onMessage(std::span<const uint8_t> msg);
  std::vector<FileRxEvent> takeEvents();
  std::optional<ReceivedFile> takeCompleted();
  // Resume support: highest checkpointed contiguous offset (0 if no
  // transfer in progress).
  bool inProgress() const { return active_; }
  uint32_t fileId() const { return meta_.fileId; }
  uint64_t checkpointOffset() const { return checkpoint_; }
  // Continuation across reconnect (4.4): keep state, expect chunks from
  // the checkpoint again.
  void rewindToCheckpoint();
  void reset();

 private:
  void fail(const char* what);

  bool active_ = false;
  FileMeta meta_;
  std::vector<uint8_t> data_;
  uint64_t checkpoint_ = 0;
  std::vector<FileRxEvent> events_;
  std::optional<ReceivedFile> completed_;
};

}  // namespace skw
