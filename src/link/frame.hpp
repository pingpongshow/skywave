// Link-layer frame envelope and per-type bodies (PROTOCOL.md).
//
// Every frame shares a 12-byte envelope inside the PHY payload:
//   byte 0     frame_type (matches the PHY header frame_type, 6-bit space)
//   byte 1     flags
//   bytes 2-3  session_id (LE; 0 for pre-session frames)
//   bytes 4-5  sequence (LE, per-direction, wraps mod 65536)
//   bytes 6-7  ack_base (LE, cumulative receive floor, piggybacked)
//   bytes 8..  type-specific body
//   last 4     CRC-32 (LE) over envelope+body (before crypto framing)
//
// SEQUENCE-SPACE RULE (load-bearing): the envelope `sequence` of VOICE_DATA
// frames is an independent voice-stream counter, NOT an ARQ sequence
// number. ARQ sequencing, ack_base, and gap bitmaps cover non-voice frames
// only - see usesVoiceSequence().
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace skw {

// ------------------------------------------------------------- frame types
// Values live in the PHY header's 6-bit frame_type field (0..63).
enum class LinkType : uint8_t {
  Call = 1,
  CallAck = 2,
  Start = 3,
  StartAck = 4,
  StartNak = 5,
  Data = 6,
  Ack = 7,  // NAK is an Ack with >=1 gap bit set - one format, one parser
  ModeChange = 8,
  ModeAck = 9,
  Probe = 10,
  ProbeAck = 11,
  MaskUpdate = 12,
  MaskAck = 13,
  Keepalive = 14,
  ResumeReq = 15,
  ResumeAck = 16,
  Disc = 17,
  DiscAck = 18,
  Beacon = 19,
  Cq = 20,
  BcastData = 21,
  VoiceOpen = 22,
  VoiceOpenAck = 23,
  VoiceClose = 24,
  VoicePttReq = 25,
  VoicePttGrant = 26,
  VoicePttEnd = 27,
  VoiceData = 28,
  KeyUpdate = 29,
  TurnGrant = 30,
};
bool isKnownLinkType(uint8_t v);

// VOICE_DATA rides the independent voice-stream counter; everything else
// (that carries a meaningful sequence at all) is ARQ space.
inline bool usesVoiceSequence(LinkType t) { return t == LinkType::VoiceData; }

// -------------------------------------------------------------------- flags
inline constexpr uint8_t kFlagCompressed    = 1u << 0;
inline constexpr uint8_t kFlagEncrypted     = 1u << 1;
inline constexpr uint8_t kFlagMoreFragments = 1u << 2;
inline constexpr uint8_t kFlagResumeContext = 1u << 3;
inline constexpr uint8_t kFlagsReservedMask = 0xF0;  // must be 0 in v1

// ----------------------------------------------------------------- envelope
inline constexpr size_t kEnvelopeBytes = 8;
inline constexpr size_t kFrameOverhead = 12;  // envelope + CRC-32

struct LinkEnvelope {
  LinkType type{};
  uint8_t flags = 0;
  uint16_t sessionId = 0;
  uint16_t sequence = 0;
  uint16_t ackBase = 0;
};

struct ParsedFrame {
  LinkEnvelope env;
  std::vector<uint8_t> body;
};

// Envelope + body + CRC-32.
std::vector<uint8_t> packFrame(const LinkEnvelope& env,
                               std::span<const uint8_t> body);
// nullopt on: short buffer, CRC mismatch, unknown type, reserved flag bits.
std::optional<ParsedFrame> parseFrame(std::span<const uint8_t> raw);

// ----------------------------------------------------------------- identity
// Callsigns: 3-12 chars [A-Z0-9/], stored uppercase. validateCallsign
// accepts lowercase input (canonicalizes); returns nullopt if invalid.
std::optional<std::string> validateCallsign(const std::string& raw);
// 24-bit FNV-1a (xor-folded from 32-bit) over the canonical callsign.
uint32_t callsignHash24(const std::string& canonical);
inline constexpr const char* kPlaceholderCall = "N0CALL";

// ------------------------------------------------------------------- bodies
// Every body has pack() and a static parse() that returns nullopt on any
// malformed input (short, bad ranges, bad magic). Parsers never throw.

enum class StartNakReason : uint8_t {
  FamilyMismatch = 1,
  VersionMismatch = 2,
  CapabilityConflict = 3,
  Busy = 4,
  PolicyRefusal = 5,
  // RESUME_REQ rejected (bad/expired token): the initiator falls back
  // to a clean START in place - never a session failure.
  ResumeInvalid = 6,
};

// Capability bitmap (u16). Bits 9-15 reserved, must be 0.
inline constexpr uint16_t kCapZstd        = 1u << 0;
inline constexpr uint16_t kCapEncryption  = 1u << 1;
inline constexpr uint16_t kCapResume      = 1u << 2;
inline constexpr uint16_t kCapHarqIr      = 1u << 3;
inline constexpr uint16_t kCapMasking     = 1u << 4;
inline constexpr uint16_t kCapBurstMac    = 1u << 5;
inline constexpr uint16_t kCapVoice       = 1u << 6;
inline constexpr uint16_t kCapCssTier     = 1u << 7;
inline constexpr uint16_t kCapTurnSched   = 1u << 8;
inline constexpr uint16_t kCapReservedMask = 0xFE00;

struct CallBody {
  std::string srcCall;  // canonical
  std::string dstCall;
  uint8_t version = 1;
  uint8_t family = 0;
  uint16_t caps = 0;
  std::vector<uint8_t> pack() const;
  static std::optional<CallBody> parse(std::span<const uint8_t> b);
};

struct CallAckBody {
  int8_t snrHalfDb = 0;      // measured CALL SNR, 0.5 dB units
  uint8_t selectivity = 0;   // frequency-selectivity index
  int8_t cfoHalfHz = 0;      // CFO estimate, 0.5 Hz units
  uint16_t caps = 0;
  std::vector<uint8_t> pack() const;
  static std::optional<CallAckBody> parse(std::span<const uint8_t> b);
};

struct StartBody {
  uint16_t caps = 0;         // committed intersection
  uint8_t initialMode = 4;   // proposed
  uint8_t cpMode = 1;
  uint8_t maxMode = 13;
  uint8_t macProfile = 0;
  bool hasResumeToken = false;
  std::array<uint8_t, 8> resumeToken{};
  std::vector<uint8_t> kexPublic;  // empty = absent; <= 255 bytes
  std::vector<uint8_t> pack() const;
  static std::optional<StartBody> parse(std::span<const uint8_t> b);
};

struct StartAckBody {
  uint8_t finalMode = 4;  // responder may only lower the proposal
  std::vector<uint8_t> kexResponse;  // empty = absent
  // Resume token minted here (PROTOCOL.md "Session Resume"): 8-byte ID
  // bound to family + callsign pair, valid for the resume window.
  bool hasResumeToken = false;
  std::array<uint8_t, 8> resumeToken{};
  std::vector<uint8_t> pack() const;
  static std::optional<StartAckBody> parse(std::span<const uint8_t> b);
};

struct StartNakBody {
  StartNakReason reason = StartNakReason::Busy;
  std::vector<uint8_t> pack() const;
  static std::optional<StartNakBody> parse(std::span<const uint8_t> b);
};

// DATA: HARQ redundancy version + opaque payload (service tag is a proto-
// layer concern inside the payload).
struct DataBody {
  uint8_t rv = 0;
  std::vector<uint8_t> payload;
  std::vector<uint8_t> pack() const;
  static std::optional<DataBody> parse(std::span<const uint8_t> b);
};

// Compact ARQ feedback - exactly 7 bytes. NAK == gapBitmap != 0.
// ACK `rx_flags` bit assignment. The byte shipped as a constant 0 in
// every ACK; it is where the RESPONDER asks the initiator for a mode,
// which is how responder-side pinning works without a second mode
// controller on the air (two controllers collided in the harness -
// PROTOCOL.md "the session initiator owns mode control").
inline constexpr uint8_t kRxFlagModeReq = 0x80;  // low nibble is the mode
inline constexpr uint8_t kRxFlagModeMask = 0x0F;
inline constexpr uint8_t kRxModeAuto = 0x0F;     // sentinel: unpin
// Turn-taking (PROTOCOL.md "Turn-Taking"): the IRS sets this in its ACKs
// while it has data waiting - the request to become the data sender.
inline constexpr uint8_t kRxFlagBreakReq = 0x40;
// Explicit NAK (PROTOCOL.md "ARQ"): a DATA burst from the peer was heard
// (PHY header decoded) but its payload failed every decode attempt. The
// sender fast-retransmits this ACK's `ack_base` - the receiver's
// next-expected frame - instead of waiting out its retransmission timer.
inline constexpr uint8_t kRxFlagRxFail = 0x20;

struct AckBody {
  int8_t snrHalfDb = 0;
  uint8_t rxFlags = 0;
  uint16_t ackBase = 0;
  uint16_t gapBitmap = 0;  // relative to ackBase+1
  int8_t cfoHalfHz = 0;    // drives TX frequency alignment
  bool isNak() const { return gapBitmap != 0; }
  std::vector<uint8_t> pack() const;
  static std::optional<AckBody> parse(std::span<const uint8_t> b);
};
inline constexpr size_t kAckBodyBytes = 7;

struct ModeChangeBody {
  uint8_t targetMode = 4;
  uint8_t cpMode = 1;
  uint8_t epoch = 0;  // echoed by MODE_ACK
  std::vector<uint8_t> pack() const;
  static std::optional<ModeChangeBody> parse(std::span<const uint8_t> b);
};

struct ModeAckBody {
  uint8_t epoch = 0;
  std::vector<uint8_t> pack() const;
  static std::optional<ModeAckBody> parse(std::span<const uint8_t> b);
};

struct ProbeBody {
  uint8_t probeId = 0;
  uint8_t probeMode = 4;
  std::vector<uint8_t> pack() const;
  static std::optional<ProbeBody> parse(std::span<const uint8_t> b);
};

struct ProbeAckBody {
  uint8_t probeId = 0;
  int8_t snrHalfDb = 0;
  uint8_t decodedOk = 0;
  std::vector<uint8_t> pack() const;
  static std::optional<ProbeAckBody> parse(std::span<const uint8_t> b);
};

// Carrier mask: one bit per family carrier (<= 72 => 9 bytes), 1 = usable.
struct MaskUpdateBody {
  uint8_t epoch = 0;
  uint8_t family = 0;
  std::array<uint8_t, 9> mask{};
  std::vector<uint8_t> pack() const;
  static std::optional<MaskUpdateBody> parse(std::span<const uint8_t> b);
};

struct MaskAckBody {
  uint8_t epoch = 0;
  std::vector<uint8_t> pack() const;
  static std::optional<MaskAckBody> parse(std::span<const uint8_t> b);
};

struct KeepaliveBody {
  uint8_t remoteSnr = 0;  // dB + 20
  std::vector<uint8_t> pack() const;
  static std::optional<KeepaliveBody> parse(std::span<const uint8_t> b);
};

// Both resume bodies may carry the sender's file-RECEIVE checkpoint
// (file_id, contiguous offset) so the peer's paused file TX rewinds to
// exactly what survived the drop.
struct ResumeReqBody {
  std::array<uint8_t, 8> token{};
  uint16_t lastRxSeq = 0;  // sender's next-expected ARQ sequence
  bool hasFileCkpt = false;
  uint32_t fileId = 0;
  uint64_t fileOffset = 0;
  std::vector<uint8_t> pack() const;
  static std::optional<ResumeReqBody> parse(std::span<const uint8_t> b);
};

struct ResumeAckBody {
  uint16_t lastRxSeq = 0;
  bool hasFileCkpt = false;
  uint32_t fileId = 0;
  uint64_t fileOffset = 0;
  std::vector<uint8_t> pack() const;
  static std::optional<ResumeAckBody> parse(std::span<const uint8_t> b);
};

enum class DiscReason : uint8_t {
  Normal = 0,
  IdleTimeout = 1,
  RetryExhausted = 2,
  Operator = 3,
  Fault = 4,
};

struct DiscBody {
  DiscReason reason = DiscReason::Normal;
  std::vector<uint8_t> pack() const;
  static std::optional<DiscBody> parse(std::span<const uint8_t> b);
};
// DISC_ACK carries no body.

struct BeaconBody {
  std::string callsign;
  uint8_t snrFloorPlus20 = 0;
  uint8_t family = 0;
  uint8_t swVersion = 1;
  std::vector<uint8_t> pack() const;
  static std::optional<BeaconBody> parse(std::span<const uint8_t> b);
};

struct CqBody {
  std::string callsign;
  uint16_t caps = 0;
  std::vector<uint8_t> pack() const;
  static std::optional<CqBody> parse(std::span<const uint8_t> b);
};

// BCAST_DATA inner format: magic 0x5B 0x01, u16 length, 4-bit fragment
// index/count, payload (<= 256 bytes per frame).
struct BcastDataBody {
  uint8_t fragIndex = 0;  // 0..15
  uint8_t fragCount = 1;  // 1..16 (wire stores count-1)
  std::vector<uint8_t> payload;
  std::vector<uint8_t> pack() const;
  static std::optional<BcastDataBody> parse(std::span<const uint8_t> b);
};
inline constexpr size_t kBcastMaxPayload = 256;

enum class VoiceCodec : uint8_t { C700 = 0, C1300 = 1, C2400 = 2, C3200 = 3 };

struct VoiceOpenBody {  // also VOICE_OPEN_ACK (echoed/possibly lowered codec)
  VoiceCodec codec = VoiceCodec::C700;
  std::vector<uint8_t> pack() const;
  static std::optional<VoiceOpenBody> parse(std::span<const uint8_t> b);
};
// VOICE_CLOSE / VOICE_PTT_REQ / VOICE_PTT_GRANT / VOICE_PTT_END: no body.
// VOICE_DATA body: opaque back-to-back Codec2 frames (VOICE.md).

struct KeyUpdateBody {
  uint8_t newEpoch = 0;
  std::vector<uint8_t> pack() const;
  static std::optional<KeyUpdateBody> parse(std::span<const uint8_t> b);
};

// TURN_GRANT (PROTOCOL.md "Turn-Taking"): the current data sender hands
// the sending role to the peer. The epoch increments per handover so a
// retried grant is idempotent at the receiver.
struct TurnGrantBody {
  uint8_t epoch = 0;
  std::vector<uint8_t> pack() const;
  static std::optional<TurnGrantBody> parse(std::span<const uint8_t> b);
};

}  // namespace skw
