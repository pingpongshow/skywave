// Proto payload pipeline (PROTOCOL.md "Payload Pipeline"): the layer
// between application bytes and link DATA payloads.
//
//   app bytes -> coalesce window (250 ms or window-full)
//             -> zstd compress (if negotiated AND it saves >= 8 bytes)
//             -> marker byte: 0x00 stored / 0x5A zstd
//             -> segment to DATA payloads sized to the current mode's
//                EXACT codeword budget (more-fragments chaining)
//
// Pure logic, injected time. The engine owns negotiation (kCapZstd) and
// asks for segments sized by dataPayloadBudget(). Messages are sealed
// (compression decided) lazily inside nextSegment(), which only runs
// once the session is connected - i.e. after capability negotiation.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace skw {

inline constexpr uint8_t kProtoMarkerStored = 0x00;
inline constexpr uint8_t kProtoMarkerZstd = 0x5A;
// Inner service tag, first byte of every (decompressed) message
// (PROTOCOL.md "File Transfer"): which service the bytes belong to.
inline constexpr uint8_t kSvcStream = 0x01;
inline constexpr uint8_t kSvcFile = 0x02;
inline constexpr size_t kProtoDecompressCap = 8u << 20;  // 8 MiB
inline constexpr double kCoalesceWindowSec = 0.25;
inline constexpr size_t kCoalesceMaxBytes = 4096;  // window-full seal
inline constexpr size_t kCompressMinSaving = 8;    // bytes, PROTOCOL.md

// Single-shot zstd. zstdUnpack returns nullopt on malformed input,
// unknown content size, or decompressed size above `cap`.
std::vector<uint8_t> zstdPack(std::span<const uint8_t> in);
std::optional<std::vector<uint8_t>> zstdUnpack(
    std::span<const uint8_t> in, size_t cap = kProtoDecompressCap);

// --- exact codeword-budget segmentation -----------------------------------
// Number of payload LDPC codewords the PHY framer will emit for a link
// payload of `phyPayloadBytes` at `mode` (mirrors buildFrameBits: 30-byte
// header piggyback, then greedy payloadCodewordN codewords).
int payloadCodewordCount(size_t phyPayloadBytes, int mode);

// Largest DATA payload size (bytes handed to DataBody) such that the
// on-air PHY payload - a solo link frame when framesPerBurst == 1, or an
// F-entry burst container otherwise - fills `codewordsTarget` codewords
// with no wasted codeword (residual padding < 8 bits, from byte
// flooring only).
size_t dataPayloadBudget(int mode, int framesPerBurst, int codewordsTarget);

// --- TX side ---------------------------------------------------------------
struct ProtoSegment {
  uint8_t flags = 0;  // kFlagCompressed | kFlagMoreFragments
  std::vector<uint8_t> bytes;
};

class ProtoTx {
 public:
  void setZstd(bool on) { zstd_ = on; }  // committed capability
  // Buffer app STREAM bytes (kSvcStream); the coalesce deadline arms at
  // the first byte of an empty window.
  void submit(std::span<const uint8_t> bytes, double now);
  // Enqueue one complete service message (e.g. a kSvcFile record). Any
  // pending stream window seals first so cross-service order is kept.
  void submitMessage(uint8_t svcTag, std::span<const uint8_t> bytes);
  // Next wire segment of at most `budget` bytes, or nullopt if the
  // coalesce window is still open and nothing is sealed.
  std::optional<ProtoSegment> nextSegment(size_t budget, double now);
  size_t backlogBytes() const;
  bool idle() const { return coalesce_.empty() && msgs_.empty(); }
  void reset();

 private:
  void maybeSeal(double now);
  void seal(uint8_t svcTag, std::span<const uint8_t> raw);

  bool zstd_ = false;
  std::vector<uint8_t> coalesce_;
  double deadline_ = 0.0;
  struct Msg {
    uint8_t flags = 0;
    std::vector<uint8_t> bytes;  // marker + body
    size_t off = 0;
  };
  std::deque<Msg> msgs_;
};

// --- RX side ---------------------------------------------------------------
// Consumes the in-order segment stream (ARQ guarantees ordered lossless
// delivery within a session); reassembles more-fragments chains and
// reverses the marker/compression framing. Malformed messages (unknown
// marker, zstd failure, cap breach) are counted and dropped - never fatal.
class ProtoRx {
 public:
  void onSegment(uint8_t flags, std::span<const uint8_t> bytes);
  std::vector<uint8_t> takeBytes();  // kSvcStream payload, in order
  // Complete non-stream service messages (tag stripped), in order.
  std::vector<std::vector<uint8_t>> takeServiceMessages(uint8_t svcTag);
  uint32_t badMessages() const { return bad_; }
  // Drop any half-assembled message (session teardown without resume).
  void abortMessage() { assembly_.clear(); }
  void reset();

 private:
  void deliver(std::span<const uint8_t> message);

  std::vector<uint8_t> assembly_;
  std::vector<uint8_t> out_;
  std::vector<std::pair<uint8_t, std::vector<uint8_t>>> svcOut_;
  uint32_t bad_ = 0;
};

}  // namespace skw
