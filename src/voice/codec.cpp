#ifdef SKW_HAVE_VOICE

#include "voice/codec.hpp"

#include <codec2.h>

#include <algorithm>
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
// format. Queried once per codec and cached.
struct FrameGeom {
  int samples = 0;
  int bytes = 0;
};
const FrameGeom& geomFor(VoiceCodec c) {
  static FrameGeom cache[4];
  static bool init[4] = {false, false, false, false};
  const size_t idx = size_t(c) & 3;
  if (!init[idx]) {
    if (struct CODEC2* t = codec2_create(codec2Mode(c))) {
      cache[idx].samples = codec2_samples_per_frame(t);
      cache[idx].bytes = codec2_bytes_per_frame(t);
      codec2_destroy(t);
    }
    init[idx] = true;
  }
  return cache[idx];
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
}

std::optional<VoiceDataBody> VoiceEncoder::nextUnit(size_t bytesBudget,
                                                    bool flush) {
  if (!impl_->c2 || impl_->samplesPerFrame <= 0) return std::nullopt;
  const size_t bpf = size_t(impl_->bytesPerFrame);
  const size_t spf = size_t(impl_->samplesPerFrame);
  // Unit header is 6 bytes (codec, count, timestamp).
  const size_t maxFrames = bytesBudget > 6 ? (bytesBudget - 6) / bpf : 0;
  if (maxFrames == 0) return std::nullopt;
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
  lastCodec_ = body.codec;
  repeats_ = 0;
}

void VoiceDecoder::onGap(int frames) {
  // Concealment: repeat the last codec frame with 3 dB attenuation, up to
  // 2 repeats, then silence (VOICE.md).
  if (frames <= 0) return;
  impl_->ensure(lastCodec_);
  if (!impl_->c2) return;
  const size_t spf = size_t(impl_->samplesPerFrame);
  std::vector<int16_t> out(spf);
  for (int f = 0; f < frames; ++f) {
    ++concealed_;
    if (lastFrame_.empty() || repeats_ >= 2) {
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
