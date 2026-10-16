// Codec2 wrapper + voice framing (VOICE.md). Compiled only under
// ENABLE_VOICE (SKW_HAVE_VOICE).
//
// The voice port carries 8 kHz mono PCM16; the modem does all codec work.
// A granted talkspurt is ONE continuous transmission: VOICE_DATA units are
// packed to fill a codeword's worth of speech, so the preamble + PHY
// header cost is paid once per spurt rather than per packet.
#pragma once
#ifdef SKW_HAVE_VOICE

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "core/constants.hpp"
#include "link/frame.hpp"

namespace skw {

inline constexpr int kVoiceSampleRate = 8000;

// Codec bit rates (bps) for the four supported Codec2 modes.
int voiceCodecBps(VoiceCodec c);
// Samples of speech in one codec frame (Codec2 frame size at 8 kHz).
int voiceCodecSamplesPerFrame(VoiceCodec c);
// Packed bytes per codec frame.
int voiceCodecBytesPerFrame(VoiceCodec c);

// VOICECODEC AUTO resolution (VOICE.md mapping table): highest codec whose
// streamed rate fits the mode's NET rate with margin. Returns nullopt when
// the family/mode combination is below the voice floor.
std::optional<VoiceCodec> autoVoiceCodec(Family f, int mode);
// Clamp a manual selection to what the current mode can carry (nullopt =
// voice not possible at all here).
std::optional<VoiceCodec> clampVoiceCodec(Family f, int mode, VoiceCodec want);

// VOICE_DATA body: codec id (u8), frame count (u8), timestamp (u32, 8 kHz
// sample units), packed codec frames.
struct VoiceDataBody {
  VoiceCodec codec = VoiceCodec::C700;
  uint32_t timestamp = 0;  // 8 kHz sample units
  uint8_t frameCount = 0;
  std::vector<uint8_t> frames;  // packed, frameCount * bytesPerFrame
  std::vector<uint8_t> pack() const;
  static std::optional<VoiceDataBody> parse(std::span<const uint8_t> b);
};

// --- encoder ----------------------------------------------------------------
// Accepts PCM16 at 8 kHz, emits VOICE_DATA bodies sized to `bytesBudget`
// (one LDPC codeword's worth of payload at the session mode).
class VoiceEncoder {
 public:
  explicit VoiceEncoder(VoiceCodec c);
  ~VoiceEncoder();
  // Non-copyable (owns codec state) but movable, so the owning
  // LinkEngine stays move-assignable. Defined out-of-line where Impl is
  // complete.
  VoiceEncoder(const VoiceEncoder&) = delete;
  VoiceEncoder& operator=(const VoiceEncoder&) = delete;
  VoiceEncoder(VoiceEncoder&&) noexcept;
  VoiceEncoder& operator=(VoiceEncoder&&) noexcept;

  VoiceCodec codec() const { return codec_; }
  // Mid-talkspurt codec step-down (AMC demoted): applies at the next
  // frame boundary, flushing whatever is already encoded.
  void setCodec(VoiceCodec c);

  // Most mic PCM held while waiting to be encoded (oldest is dropped).
  static constexpr size_t kMaxPendingSamples = 30 * kVoiceSampleRate;

  void pushPcm(std::span<const int16_t> pcm);
  // Drop buffered (not yet encoded) mic PCM, keeping codec state and the
  // timestamp: called at talkspurt start/end so speech captured outside a
  // talkspurt is never replayed as stale audio.
  void discardPcm();
  // Next unit, or nullopt until a unit's worth of speech is buffered.
  // flush=true emits a short final unit at PTT end (the trailing partial
  // frame is padded with silence); call it until it returns nullopt to
  // drain the whole tail.
  std::optional<VoiceDataBody> nextUnit(size_t bytesBudget, bool flush);
  size_t pendingSamples() const { return pcm_.size(); }
  void reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  VoiceCodec codec_;
  std::vector<int16_t> pcm_;
  uint32_t nextTimestamp_ = 0;
};

// --- decoder ----------------------------------------------------------------
// Consumes VOICE_DATA bodies (possibly with gaps in the voice sequence
// counter) and produces PCM16. Concealment: repeat the last codec frame
// with 3 dB attenuation, up to 2 repeats, then silence.
class VoiceDecoder {
 public:
  VoiceDecoder();
  ~VoiceDecoder();
  VoiceDecoder(const VoiceDecoder&) = delete;
  VoiceDecoder& operator=(const VoiceDecoder&) = delete;
  VoiceDecoder(VoiceDecoder&&) noexcept;
  VoiceDecoder& operator=(VoiceDecoder&&) noexcept;

  // Longest stretch one onGap() call fills (see onGap).
  static constexpr int kMaxConcealMs = 640;

  void onVoiceData(const VoiceDataBody& body);
  // A detected gap in the voice sequence space: conceal `frames` frames of
  // missing speech (never a NAK - late voice is worthless). Clamped to
  // kMaxConcealMs, so any input is safe.
  void onGap(int frames);
  std::vector<int16_t> takePcm();
  uint32_t concealedFrames() const { return concealed_; }
  void reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<int16_t> pcm_;
  std::vector<uint8_t> lastFrame_;  // for concealment repeats
  VoiceCodec lastCodec_ = VoiceCodec::C700;
  int repeats_ = 0;
  uint32_t concealed_ = 0;
};

}  // namespace skw

#endif  // SKW_HAVE_VOICE
