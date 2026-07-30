// Voice session control (VOICE.md "Session Control", ROADMAP 5.5): the
// half-duplex talkspurt state machine, the jitter buffer, and the voice
// TCP port record format. Pure logic with injected time, like the rest of
// the link layer; the engine owns the frames and the worker owns audio.
#pragma once
#ifdef SKW_HAVE_VOICE

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "link/frame.hpp"

namespace skw {

// VOICE mode selector (control surface `VOICE OFF|CONNECTED|BROADCAST`).
enum class VoiceMode : uint8_t { Off = 0, Connected, Broadcast };

// Talkspurt state. OPEN means both peers agreed on a codec; PTT is the
// half-duplex grant discipline on top of it.
enum class VoiceState : uint8_t {
  Closed = 0,
  Opening,    // VOICE_OPEN sent, awaiting VOICE_OPEN_ACK
  Open,       // idle, either side may request the talkspurt
  Requesting, // VOICE_PTT_REQ sent, awaiting VOICE_PTT_GRANT
  Talking,    // we hold the grant: streaming VOICE_DATA
  Receiving,  // peer holds the grant
};

// Actions the FSM asks the engine to put on the air.
enum class VoiceAction : uint8_t {
  SendOpen,
  SendOpenAck,
  SendClose,
  SendPttReq,
  SendPttGrant,
  SendPttEnd,
  IndicateOpen,     // -> EVT:VOICE OPEN
  IndicateClosed,   // -> EVT:VOICE CLOSED
  IndicateTalking,  // grant received: start streaming
  IndicateStopped,  // talkspurt ended (ours or peer's)
};

struct VoiceFsmConfig {
  double openTimeoutSec = 8.0;   // OPEN / PTT_REQ response window
  double grantHoldSec = 180.0;   // safety cap on one talkspurt
  int openAttempts = 3;
};

// Half-duplex talkspurt arbitration. A tie (both sides requesting in the
// same instant) is broken deterministically by callsign order so the two
// stations never both believe they hold the grant.
class VoiceFsm {
 public:
  explicit VoiceFsm(VoiceFsmConfig cfg = {}) : cfg_(cfg) {}

  VoiceState state() const { return state_; }
  bool talking() const { return state_ == VoiceState::Talking; }
  std::optional<VoiceAction> nextAction();

  // Local commands.
  void cmdOpen(VoiceCodec codec, double now);
  void cmdClose(double now);
  void cmdPttOn(double now);
  void cmdPttOff(double now);

  // Received frames.
  void rxOpen(VoiceCodec codec, double now);
  void rxOpenAck(VoiceCodec codec, double now);
  void rxClose(double now);
  void rxPttReq(double now, bool weWinTie);
  void rxPttGrant(double now);
  void rxPttEnd(double now);

  void tick(double now);

  VoiceCodec codec() const { return codec_; }
  void setCodec(VoiceCodec c) { codec_ = c; }  // demotion step-down

 private:
  void emit(VoiceAction a) { queue_.push_back(a); }

  VoiceFsmConfig cfg_;
  VoiceState state_ = VoiceState::Closed;
  VoiceCodec codec_ = VoiceCodec::C700;
  double deadline_ = 0.0;
  int attempts_ = 0;
  bool pttWanted_ = false;
  std::deque<VoiceAction> queue_;
};

// Downlink jitter buffer: target depth 240 ms, adaptive +-120 ms
// (VOICE.md). Holds decoded PCM until the target is met, then releases at
// the requested rate; underrun is reported so the caller can conceal.
class JitterBuffer {
 public:
  static constexpr int kTargetMs = 240;
  static constexpr int kAdaptMs = 120;

  void push(std::span<const int16_t> pcm);
  // Take up to `samples`; short reads mean underrun (fill with silence).
  std::vector<int16_t> take(size_t samples);
  size_t depthSamples() const { return buf_.size(); }
  int depthMs() const { return int(buf_.size() * 1000 / 8000); }
  bool primed() const { return primed_; }
  uint32_t underruns() const { return underruns_; }
  void reset();

 private:
  std::deque<int16_t> buf_;
  bool primed_ = false;
  uint32_t underruns_ = 0;
};

// --- voice TCP port records (INTERFACES.md) ---------------------------------
inline constexpr uint8_t kVoiceRecVersion = 1;
inline constexpr uint8_t kVoiceRecUplink = 1;    // app -> modem (mic)
inline constexpr uint8_t kVoiceRecDownlink = 2;  // modem -> app (speaker)

struct VoiceRecord {
  uint8_t type = kVoiceRecDownlink;
  uint32_t sequence = 0;
  uint64_t sampleCounter = 0;
  std::vector<int16_t> pcm;  // 8 kHz mono
  std::vector<uint8_t> pack() const;
};
// Streaming parser: feed socket bytes, take complete records.
class VoiceRecordParser {
 public:
  void feed(std::span<const uint8_t> bytes);
  std::vector<VoiceRecord> take();
  uint32_t badRecords() const { return bad_; }

 private:
  std::vector<uint8_t> acc_;
  std::vector<VoiceRecord> out_;
  uint32_t bad_ = 0;
};

}  // namespace skw

#endif  // SKW_HAVE_VOICE
