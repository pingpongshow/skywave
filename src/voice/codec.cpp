#ifdef SKW_HAVE_VOICE

#include "voice/codec.hpp"

#include <codec2.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/bytepack.hpp"

namespace skw {
namespace {

int codec2Mode(VoiceCodec c) {
  switch (c) {
    case VoiceCodec::C700: return CODEC2_MODE_700C;
    case VoiceCodec::C1300: return CODEC2_MODE_1300;
    case VoiceCodec::C2400: return CODEC2_MODE_2400;
    default: return CODEC2_MODE_3200;
  }
}

// Streamed rate = codec rate + ~5% envelope/body overhead (VOICE.md).
double streamedBps(VoiceCodec c) { return voiceCodecBps(c) * 1.05; }

}  // namespace

int voiceCodecBps(VoiceCodec c) {
  switch (c) {
    case VoiceCodec::C700: return 700;
    case VoiceCodec::C1300: return 1300;
    case VoiceCodec::C2400: return 2400;
    default: return 3200;
  }
}

namespace {
// Frame geometry comes from the LIBRARY, not a local table: Codec2's
// frame durations are not uniform (700C and 1300 are 40 ms, 2400/3200 are
// 20 ms) and a stale constant here would silently corrupt the wire
// format. Queried once for all four codecs inside a function-local static
// initialiser, which C++ guarantees runs exactly once even when the
// engine and a test thread race on first use (the old lazy per-entry
// flags were a data race).
struct FrameGeom {
  int samples = 0;
  int bytes = 0;
};
struct GeomTable {
  FrameGeom g[4];
  GeomTable() {
    for (size_t i = 0; i < 4; ++i) {
      if (struct CODEC2* t = codec2_create(codec2Mode(VoiceCodec(i)))) {
        g[i].samples = codec2_samples_per_frame(t);
        g[i].bytes = codec2_bytes_per_frame(t);
        codec2_destroy(t);
      }
    }
  }
};
const FrameGeom& geomFor(VoiceCodec c) {
  static const GeomTable table;
  return table.g[size_t(c) & 3];
}
}  // namespace

int voiceCodecSamplesPerFrame(VoiceCodec c) { return geomFor(c).samples; }

int voiceCodecBytesPerFrame(VoiceCodec c) { return geomFor(c).bytes; }

std::optional<VoiceCodec> autoVoiceCodec(Family f, int mode) {
  // VOICE.md mapping table, expressed as the rule it encodes: highest
  // codec whose streamed rate fits the mode's NET rate with margin, and
  // never below the family's voice floor (unspread modes only, M8+).
  if (mode < 8) return std::nullopt;
  const double net = netRate(f, mode, kCpDefault);
  const VoiceCodec ladder[4] = {VoiceCodec::C3200, VoiceCodec::C2400,
                                VoiceCodec::C1300, VoiceCodec::C700};
  for (VoiceCodec c : ladder) {
    if (streamedBps(c) * 1.35 <= net) return c;  // 35% headroom
  }
  return std::nullopt;
}

std::optional<VoiceCodec> clampVoiceCodec(Family f, int mode,
                                          VoiceCodec want) {
  auto best = autoVoiceCodec(f, mode);
  if (!best) return std::nullopt;
  // Codec enum is ordered by rate, so a numerically larger value is a
  // higher rate: clamp the manual choice down to what fits.
  return uint8_t(want) > uint8_t(*best) ? *best : want;
}

std::vector<uint8_t> VoiceDataBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u8(uint8_t(codec));
  w.u8(frameCount);
  w.u32le(timestamp);
  w.bytes(frames);
  return out;
}

std::optional<VoiceDataBody> VoiceDataBody::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  VoiceDataBody v;
  const uint8_t cid = r.u8();
  if (cid > uint8_t(VoiceCodec::C3200)) return std::nullopt;
  v.codec = VoiceCodec(cid);
  v.frameCount = r.u8();
  v.timestamp = r.u32le();
  if (!r.ok()) return std::nullopt;
  const size_t need =
      size_t(v.frameCount) * size_t(voiceCodecBytesPerFrame(v.codec));
  if (r.remaining() != need) return std::nullopt;
  v.frames.resize(need);
  if (!r.bytes(v.frames)) return std::nullopt;
  return v;
}

// --- encoder -------------------------------------------------------------------

struct VoiceEncoder::Impl {
  struct CODEC2* c2 = nullptr;
  int samplesPerFrame = 0;
  int bytesPerFrame = 0;
  void open(VoiceCodec c) {
    close();
    c2 = codec2_create(codec2Mode(c));
    if (c2) {
      samplesPerFrame = codec2_samples_per_frame(c2);
      bytesPerFrame = codec2_bytes_per_frame(c2);
    }
  }
  void close() {
    if (c2) codec2_destroy(c2);
    c2 = nullptr;
  }
  ~Impl() { close(); }
};

VoiceEncoder::VoiceEncoder(VoiceCodec c)
    : impl_(std::make_unique<Impl>()), codec_(c) {
  impl_->open(c);
}

VoiceEncoder::~VoiceEncoder() = default;
VoiceEncoder::VoiceEncoder(VoiceEncoder&&) noexcept = default;
VoiceEncoder& VoiceEncoder::operator=(VoiceEncoder&&) noexcept = default;

void VoiceEncoder::setCodec(VoiceCodec c) {
  if (c == codec_) return;
  codec_ = c;
  impl_->open(c);  // takes effect at the next frame boundary
}

void VoiceEncoder::pushPcm(std::span<const int16_t> pcm) {
  pcm_.insert(pcm_.end(), pcm.begin(), pcm.end());
  // Bounded backlog: a mic stream the engine is not draining (grant never
  // came, link stalled) must not grow without limit. Keep the NEWEST
  // speech - stale audio is worse than lost audio.
  if (pcm_.size() > kMaxPendingSamples) {
    pcm_.erase(pcm_.begin(),
               pcm_.begin() + long(pcm_.size() - kMaxPendingSamples));
  }
}

void VoiceEncoder::discardPcm() { pcm_.clear(); }

std::optional<VoiceDataBody> VoiceEncoder::nextUnit(size_t bytesBudget,
                                                    bool flush) {
  if (!impl_->c2 || impl_->samplesPerFrame <= 0) return std::nullopt;
  const size_t bpf = size_t(impl_->bytesPerFrame);
  const size_t spf = size_t(impl_->samplesPerFrame);
  // Unit header is 6 bytes (codec, count, timestamp). frameCount is a u8
  // on the wire: a big VHF codeword at C700 holds ~300 frames, and
  // writing 306 as uint8_t(50) made every unit fail the receiver's length
  // check. Cap at what the field can say.
  const size_t maxFrames =
      std::min<size_t>(bytesBudget > 6 ? (bytesBudget - 6) / bpf : 0, 255);
  if (maxFrames == 0) return std::nullopt;
  // A flush pads the sub-frame remainder with silence so the last
  // fraction of a frame of speech goes out too instead of lingering into
  // the next talkspurt.
  if (flush && pcm_.size() % spf != 0) {
    pcm_.resize(pcm_.size() + (spf - pcm_.size() % spf), 0);
  }
  const size_t haveFrames = pcm_.size() / spf;
  if (haveFrames == 0) return std::nullopt;
  // Fill the unit unless flushing a short tail at PTT end.
  const size_t n = std::min(maxFrames, haveFrames);
  if (!flush && n < maxFrames) return std::nullopt;

  VoiceDataBody body;
  body.codec = codec_;
  body.timestamp = nextTimestamp_;
  body.frameCount = uint8_t(n);
  body.frames.resize(n * bpf);
  for (size_t i = 0; i < n; ++i) {
    codec2_encode(impl_->c2, body.frames.data() + i * bpf,
                  pcm_.data() + i * spf);
  }
  pcm_.erase(pcm_.begin(), pcm_.begin() + long(n * spf));
  nextTimestamp_ += uint32_t(n * spf);
  return body;
}

void VoiceEncoder::reset() {
  pcm_.clear();
  nextTimestamp_ = 0;
  impl_->open(codec_);
}

// --- decoder -------------------------------------------------------------------

struct VoiceDecoder::Impl {
  struct CODEC2* c2 = nullptr;
  VoiceCodec open = VoiceCodec::C700;
  int samplesPerFrame = 0;
  int bytesPerFrame = 0;
  void ensure(VoiceCodec c) {
    if (c2 && c == open) return;
    if (c2) codec2_destroy(c2);
    c2 = codec2_create(codec2Mode(c));
    open = c;
    if (c2) {
      samplesPerFrame = codec2_samples_per_frame(c2);
      bytesPerFrame = codec2_bytes_per_frame(c2);
    }
  }
  ~Impl() {
    if (c2) codec2_destroy(c2);
  }
};

VoiceDecoder::VoiceDecoder() : impl_(std::make_unique<Impl>()) {}
VoiceDecoder::~VoiceDecoder() = default;
VoiceDecoder::VoiceDecoder(VoiceDecoder&&) noexcept = default;
VoiceDecoder& VoiceDecoder::operator=(VoiceDecoder&&) noexcept = default;

void VoiceDecoder::onVoiceData(const VoiceDataBody& body) {
  impl_->ensure(body.codec);
  if (!impl_->c2) return;
  const size_t bpf = size_t(impl_->bytesPerFrame);
  const size_t spf = size_t(impl_->samplesPerFrame);
  if (body.frames.size() < size_t(body.frameCount) * bpf) return;
  std::vector<int16_t> out(spf);
  for (size_t i = 0; i < body.frameCount; ++i) {
    codec2_decode(impl_->c2, out.data(), body.frames.data() + i * bpf);
    pcm_.insert(pcm_.end(), out.begin(), out.end());
    lastFrame_.assign(body.frames.begin() + long(i * bpf),
                      body.frames.begin() + long((i + 1) * bpf));
  }
  // Only a unit that actually carried frames defines what concealment
  // repeats: a zero-frame unit under another codec must not pair the old
  // (shorter) lastFrame_ with the new codec's frame size - that read past
  // the buffer from RF input.
  if (body.frameCount > 0) {
    lastCodec_ = body.codec;
    repeats_ = 0;
  }
}

void VoiceDecoder::onGap(int frames) {
  // Concealment: repeat the last codec frame with 3 dB attenuation, up to
  // 2 repeats, then silence (VOICE.md).
  if (frames <= 0) return;
  impl_->ensure(lastCodec_);
  if (!impl_->c2) return;
  const size_t spf = size_t(impl_->samplesPerFrame);
  // The gap size is derived from an over-the-air sequence number, so one
  // corrupt counter could ask for billions of samples. Past two repeats
  // concealment is plain silence anyway; filling more than a few hundred
  // ms only adds latency, so the timeline is allowed to compress.
  const int cap = std::max(1, int(kMaxConcealMs * kVoiceSampleRate / 1000 /
                                  int(spf)));
  frames = std::min(frames, cap);
  // Belt and braces for the codec-switch case: never decode a stored
  // frame whose size does not match the codec it is decoded with.
  const bool frameOk =
      lastFrame_.size() == size_t(impl_->bytesPerFrame);
  std::vector<int16_t> out(spf);
  for (int f = 0; f < frames; ++f) {
    ++concealed_;
    if (!frameOk || repeats_ >= 2) {
      pcm_.insert(pcm_.end(), spf, 0);
      continue;
    }
    codec2_decode(impl_->c2, out.data(), lastFrame_.data());
    const float atten = std::pow(0.707f, float(repeats_ + 1));  // -3 dB/rep
    for (size_t i = 0; i < spf; ++i) {
      pcm_.push_back(int16_t(float(out[i]) * atten));
    }
    ++repeats_;
  }
}

std::vector<int16_t> VoiceDecoder::takePcm() {
  auto out = std::move(pcm_);
  pcm_.clear();
  return out;
}

void VoiceDecoder::reset() {
  pcm_.clear();
  lastFrame_.clear();
  repeats_ = 0;
  concealed_ = 0;
}

}  // namespace skw

#endif  // SKW_HAVE_VOICE
