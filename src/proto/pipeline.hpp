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

#include "core/constants.hpp"

namespace skw {

// The marker byte alone says whether a message is compressed (the link
// envelope carries no compression flag).
inline constexpr uint8_t kProtoMarkerStored = 0x00;
inline constexpr uint8_t kProtoMarkerZstd = 0x5A;
// Inner service tag, first byte of every (decompressed) message
// (PROTOCOL.md "Payload Pipeline"). The byte stream is the only service;
// other values are reserved and dropped as malformed.
inline constexpr uint8_t kSvcStream = 0x01;
inline constexpr size_t kProtoDecompressCap = 8u << 20;  // 8 MiB
inline constexpr double kCoalesceWindowSec = 0.25;
// Window-full seal cap: TX-local segmentation policy (the receiver
// reassembles whatever message sizes arrive). The VHF-FM profile uses
// larger messages so tail segments do not eat container slots at
// 25+ kbps; HF keeps small messages for latency under loss.
inline constexpr size_t kCoalesceMaxBytes = 4096;
inline size_t coalesceMaxBytes() {
  return activeBand() == Band::VhfFm ? 16384 : kCoalesceMaxBytes;
}
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

// Bytes a DATA frame adds around its payload on the air: the link
// envelope + CRC (kFrameOverhead, 12), the HARQ rv byte, and - inside a
// burst container (framesPerBurst > 1) - the u16 entry length prefix.
inline constexpr size_t dataFrameOverhead(int framesPerBurst) {
  return 12 + 1 + (framesPerBurst > 1 ? 2 : 0);
}

// Largest DATA payload size (bytes handed to DataBody) such that the
// on-air PHY payload - a solo link frame when framesPerBurst == 1, or an
// F-entry burst container otherwise - fills `codewordsTarget` codewords
// with no wasted codeword (residual padding < 8 bits, from byte
// flooring only).
size_t dataPayloadBudget(int mode, int framesPerBurst, int codewordsTarget);

// --- TX side ---------------------------------------------------------------
struct ProtoSegment {
  uint8_t flags = 0;  // kFlagMoreFragments
  std::vector<uint8_t> bytes;
};

class ProtoTx {
 public:
  void setZstd(bool on) { zstd_ = on; }  // committed capability
  // Seal-size ceiling for STREAM coalescing, set by the engine from the
  // session mode's frame budget. Under saturated submit the window
  // otherwise always fills to coalesceMaxBytes() before the deadline,
  // and at deep modes a single 4 KiB message spans ~160 frames - the
  // app sees NOTHING until every one of them lands (observed: a
  // connected M0 session with zero delivered bytes for 900 s).
  // Bounding the message to a handful of frames makes delivery
  // incremental at every mode.
  void setSealCap(size_t bytes) { sealCap_ = bytes ? bytes : 1; }
  // Buffer app STREAM bytes (kSvcStream); the coalesce deadline arms at
  // the first byte of an empty window.
  void submit(std::span<const uint8_t> bytes, double now);
  // Next wire segment of at most `budget` bytes, or nullopt if the
  // coalesce window is still open and nothing is sealed.
  std::optional<ProtoSegment> nextSegment(size_t budget, double now);
  size_t backlogBytes() const;
  bool idle() const { return coalesce_.empty() && msgs_.empty(); }
  void reset();

  // --- replay across a peer that lost its session state -------------
  // A message stays here from the moment its last fragment is handed
  // out until the ARQ confirms every frame of it was acknowledged. The
  // set is bounded by the ARQ window (the engine only pulls a segment
  // when the window has room), so this is a handful of KiB.
  //
  // It exists because a peer that RESTARTS keeps nothing: it cannot
  // honour a resume token, and the session that follows is a fresh one
  // whose receiver has no assembly buffer and no delivered prefix. The
  // only bytes it can be assumed to have taken are those in messages
  // FULLY acknowledged before it died - head-of-line assembly means a
  // partially acknowledged message was never delivered to anyone.
  // Replay therefore restarts at a MESSAGE boundary, never at an ARQ
  // sequence boundary: resuming mid-message would hand the fresh peer
  // a more-fragments continuation whose head it never saw, and the
  // whole chain would be dropped as malformed.
  void confirmMessages(size_t n);
  void replayUnconfirmed();
  size_t unconfirmedMessages() const { return unconfirmed_.size(); }
  // APPLICATION bytes held unconfirmed (not the framed size).
  size_t unconfirmedBytes() const;

 private:
  void maybeSeal(double now);
  void seal(std::span<const uint8_t> raw);

  bool zstd_ = false;
  size_t sealCap_ = size_t(-1);
  std::vector<uint8_t> coalesce_;
  double deadline_ = 0.0;
  struct Msg {
    std::vector<uint8_t> bytes;  // marker + body
    size_t off = 0;
    // APPLICATION bytes this message carries, before the service tag,
    // the marker and any compression. EVT:STREAM_REPLAY reports these,
    // because the number is only useful to an application if it counts
    // in the units the application submitted - the framed size is 2-3
    // bytes larger when stored and can be far SMALLER when compressed.
    size_t rawBytes = 0;
  };
  std::deque<Msg> msgs_;
  std::deque<Msg> unconfirmed_;
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
  uint32_t badMessages() const { return bad_; }
  // Drop any half-assembled message (session teardown without resume).
  void abortMessage() {
    assembly_.clear();
    resync_ = false;
  }

 private:
  void deliver(std::span<const uint8_t> message);

  std::vector<uint8_t> assembly_;
  bool resync_ = false;  // discarding the rest of an oversize chain
  std::vector<uint8_t> out_;
  uint32_t bad_ = 0;
};

}  // namespace skw
