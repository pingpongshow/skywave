#include "link/frame.hpp"

#include <cctype>

#include "core/bytepack.hpp"
#include "core/constants.hpp"
#include "fec/crc.hpp"

namespace skw {
namespace {

// Length-prefixed callsign field: u8 length + bytes. Parse validates.
void putCall(ByteWriter& w, const std::string& c) {
  w.u8(uint8_t(c.size()));
  w.bytes({reinterpret_cast<const uint8_t*>(c.data()), c.size()});
}

std::optional<std::string> getCall(ByteReader& r) {
  uint8_t len = r.u8();
  if (!r.ok() || len < 3 || len > 12 || r.remaining() < len) {
    return std::nullopt;
  }
  std::string s(size_t(len), '\0');
  if (!r.bytes({reinterpret_cast<uint8_t*>(s.data()), s.size()})) {
    return std::nullopt;
  }
  return validateCallsign(s);  // wire form must already be canonical
}

}  // namespace

bool isKnownLinkType(uint8_t v) {
  return v >= uint8_t(LinkType::Call) && v <= uint8_t(LinkType::BcastVoice);
}

// ----------------------------------------------------------------- envelope

std::vector<uint8_t> packFrame(const LinkEnvelope& env,
                               std::span<const uint8_t> body) {
  std::vector<uint8_t> out;
  out.reserve(kFrameOverhead + body.size());
  ByteWriter w(out);
  w.u8(uint8_t(env.type));
  w.u8(env.flags);
  w.u16le(env.sessionId);
  w.u16le(env.sequence);
  w.u16le(env.ackBase);
  w.bytes(body);
  w.u32le(crc32({out.data(), out.size()}));
  return out;
}

std::optional<ParsedFrame> parseFrame(std::span<const uint8_t> raw) {
  if (raw.size() < kFrameOverhead) return std::nullopt;
  const size_t crcOff = raw.size() - 4;
  ByteReader cr(raw.subspan(crcOff));
  if (cr.u32le() != crc32(raw.first(crcOff))) return std::nullopt;

  ByteReader r(raw.first(crcOff));
  ParsedFrame f;
  uint8_t type = r.u8();
  if (!isKnownLinkType(type)) return std::nullopt;
  f.env.type = LinkType(type);
  f.env.flags = r.u8();
  if (f.env.flags & kFlagsReservedMask) return std::nullopt;
  f.env.sessionId = r.u16le();
  f.env.sequence = r.u16le();
  f.env.ackBase = r.u16le();
  f.body.assign(raw.begin() + long(kEnvelopeBytes), raw.begin() + long(crcOff));
  return f;
}

// ----------------------------------------------------------------- identity

std::optional<std::string> validateCallsign(const std::string& raw) {
  if (raw.size() < 3 || raw.size() > 12) return std::nullopt;
  std::string out;
  out.reserve(raw.size());
  for (char ch : raw) {
    char u = char(std::toupper(static_cast<unsigned char>(ch)));
    const bool okc =
        (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '/';
    if (!okc) return std::nullopt;
    out.push_back(u);
  }
  return out;
}

uint32_t callsignHash24(const std::string& canonical) {
  uint32_t h = 2166136261u;  // FNV-1a 32
  for (char c : canonical) {
    h ^= uint8_t(c);
    h *= 16777619u;
  }
  return (h ^ (h >> 24)) & 0xFFFFFFu;  // xor-fold to 24 bits
}

// ------------------------------------------------------------------- bodies

std::vector<uint8_t> CallBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  putCall(w, srcCall);
  putCall(w, dstCall);
  w.u8(version);
  w.u8(family);
  w.u16le(caps);
  return out;
}

std::optional<CallBody> CallBody::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  CallBody v;
  auto src = getCall(r);
  auto dst = getCall(r);
  if (!src || !dst) return std::nullopt;
  v.srcCall = *src;
  v.dstCall = *dst;
  v.version = r.u8();
  v.family = r.u8();
  v.caps = r.u16le();
  if (!r.ok() || r.remaining() != 0) return std::nullopt;
  if (v.family >= kNumFamilies) return std::nullopt;
  if (v.caps & kCapReservedMask) return std::nullopt;
  return v;
}

std::vector<uint8_t> CallAckBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u8(uint8_t(snrHalfDb));
  w.u8(selectivity);
  w.u8(uint8_t(cfoHalfHz));
  w.u16le(caps);
  return out;
}

std::optional<CallAckBody> CallAckBody::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  CallAckBody v;
  v.snrHalfDb = int8_t(r.u8());
  v.selectivity = r.u8();
  v.cfoHalfHz = int8_t(r.u8());
  v.caps = r.u16le();
  if (!r.ok() || r.remaining() != 0) return std::nullopt;
  if (v.caps & kCapReservedMask) return std::nullopt;
  return v;
}

std::vector<uint8_t> StartBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u16le(caps);
  w.u8(initialMode);
  w.u8(maxMode);
  // KEX public key: u8 length (0 = absent) + bytes.
  w.u8(uint8_t(kexPublic.size()));
  w.bytes(kexPublic);
  return out;
}

std::optional<StartBody> StartBody::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  StartBody v;
  v.caps = r.u16le();
  v.initialMode = r.u8();
  v.maxMode = r.u8();
  const uint8_t len = r.u8();
  if (!r.ok() || r.remaining() != len) return std::nullopt;
  v.kexPublic.resize(len);
  if (len && !r.bytes(v.kexPublic)) return std::nullopt;
  if (v.caps & kCapReservedMask) return std::nullopt;
  if (v.initialMode >= kNumModes || v.maxMode >= kNumModes) {
    return std::nullopt;
  }
  return v;
}

std::vector<uint8_t> StartAckBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u8(finalMode);
  w.u8(maxMode);
  const uint8_t present =
      uint8_t((kexResponse.empty() ? 0 : 1) | (hasResumeToken ? 2 : 0));
  w.u8(present);
  if (!kexResponse.empty()) {
    w.u8(uint8_t(kexResponse.size()));
    w.bytes(kexResponse);
  }
  if (hasResumeToken) w.bytes(resumeToken);
  return out;
}

std::optional<StartAckBody> StartAckBody::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  StartAckBody v;
  v.finalMode = r.u8();
  v.maxMode = r.u8();
  uint8_t present = r.u8();
  if (!r.ok() || (present & ~3u)) return std::nullopt;
  if (present & 1) {
    uint8_t len = r.u8();
    if (!r.ok() || len == 0 || r.remaining() < len) return std::nullopt;
    v.kexResponse.resize(len);
    if (!r.bytes(v.kexResponse)) return std::nullopt;
  }
  if (present & 2) {
    v.hasResumeToken = true;
    if (!r.bytes(v.resumeToken)) return std::nullopt;
  }
  if (!r.ok() || r.remaining() != 0) return std::nullopt;
  if (v.finalMode >= kNumModes || v.maxMode >= kNumModes) return std::nullopt;
  return v;
}

std::vector<uint8_t> StartNakBody::pack() const {
  return {uint8_t(reason)};
}

std::optional<StartNakBody> StartNakBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 1) return std::nullopt;
  if (b[0] < uint8_t(StartNakReason::FamilyMismatch) ||
      b[0] > uint8_t(StartNakReason::ResumeInvalid)) {
    return std::nullopt;
  }
  return StartNakBody{StartNakReason(b[0])};
}

std::vector<uint8_t> DataBody::pack() const {
  std::vector<uint8_t> out;
  out.reserve(1 + payload.size());
  out.push_back(rv);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::optional<DataBody> DataBody::parse(std::span<const uint8_t> b) {
  if (b.empty()) return std::nullopt;
  DataBody v;
  v.rv = b[0];
  v.payload.assign(b.begin() + 1, b.end());
  return v;
}

std::vector<uint8_t> AckBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u8(uint8_t(snrHalfDb));
  w.u8(rxFlags);
  w.u16le(ackBase);
  w.u16le(gapBitmap);
  w.u8(uint8_t(cfoHalfHz));
  return out;
}

std::optional<AckBody> AckBody::parse(std::span<const uint8_t> b) {
  if (b.size() != kAckBodyBytes) return std::nullopt;
  ByteReader r(b);
  AckBody v;
  v.snrHalfDb = int8_t(r.u8());
  v.rxFlags = r.u8();
  v.ackBase = r.u16le();
  v.gapBitmap = r.u16le();
  v.cfoHalfHz = int8_t(r.u8());
  return v;
}

std::vector<uint8_t> ModeChangeBody::pack() const {
  return {targetMode, cpMode, epoch};
}

std::optional<ModeChangeBody> ModeChangeBody::parse(
    std::span<const uint8_t> b) {
  if (b.size() != 3) return std::nullopt;
  ModeChangeBody v{b[0], b[1], b[2]};
  if (v.targetMode >= kNumModes || v.cpMode > 2) return std::nullopt;
  return v;
}

std::vector<uint8_t> ModeAckBody::pack() const { return {epoch}; }

std::optional<ModeAckBody> ModeAckBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 1) return std::nullopt;
  return ModeAckBody{b[0]};
}

std::vector<uint8_t> ProbeBody::pack() const { return {probeId, probeMode}; }

std::optional<ProbeBody> ProbeBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 2) return std::nullopt;
  ProbeBody v{b[0], b[1]};
  if (v.probeMode >= kNumModes) return std::nullopt;
  return v;
}

std::vector<uint8_t> ProbeAckBody::pack() const {
  return {probeId, decodedOk};
}

std::optional<ProbeAckBody> ProbeAckBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 2) return std::nullopt;
  return ProbeAckBody{b[0], b[1]};
}

std::vector<uint8_t> MaskUpdateBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u8(epoch);
  w.bytes(mask);
  return out;
}

std::optional<MaskUpdateBody> MaskUpdateBody::parse(
    std::span<const uint8_t> b) {
  if (b.size() != 1 + 9) return std::nullopt;
  ByteReader r(b);
  MaskUpdateBody v;
  v.epoch = r.u8();
  if (!r.bytes(v.mask)) return std::nullopt;
  return v;
}

std::vector<uint8_t> MaskAckBody::pack() const { return {epoch}; }

std::optional<MaskAckBody> MaskAckBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 1) return std::nullopt;
  return MaskAckBody{b[0]};
}

std::vector<uint8_t> KeepaliveBody::pack() const {
  return {uint8_t(snrHalfDb)};
}

std::optional<KeepaliveBody> KeepaliveBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 1) return std::nullopt;
  return KeepaliveBody{int8_t(b[0])};
}

std::vector<uint8_t> ResumeReqBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.bytes(token);
  w.u16le(lastRxSeq);
  return out;
}

std::optional<ResumeReqBody> ResumeReqBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 10) return std::nullopt;
  ByteReader r(b);
  ResumeReqBody v;
  if (!r.bytes(v.token)) return std::nullopt;
  v.lastRxSeq = r.u16le();
  return v;
}

std::vector<uint8_t> ResumeAckBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u16le(lastRxSeq);
  return out;
}

std::optional<ResumeAckBody> ResumeAckBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 2) return std::nullopt;
  ByteReader r(b);
  ResumeAckBody v;
  v.lastRxSeq = r.u16le();
  return v;
}

std::vector<uint8_t> DiscBody::pack() const { return {uint8_t(reason)}; }

std::optional<DiscBody> DiscBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 1) return std::nullopt;
  if (b[0] > uint8_t(DiscReason::RetryExhausted)) return std::nullopt;
  return DiscBody{DiscReason(b[0])};
}

std::vector<uint8_t> BeaconBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  putCall(w, callsign);
  w.u8(snrFloorPlus20);
  return out;
}

std::optional<BeaconBody> BeaconBody::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  BeaconBody v;
  auto call = getCall(r);
  if (!call) return std::nullopt;
  v.callsign = *call;
  v.snrFloorPlus20 = r.u8();
  if (!r.ok() || r.remaining() != 0) return std::nullopt;
  return v;
}

std::vector<uint8_t> CqBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  putCall(w, callsign);
  return out;
}

std::optional<CqBody> CqBody::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  CqBody v;
  auto call = getCall(r);
  if (!call || r.remaining() != 0) return std::nullopt;
  v.callsign = *call;
  return v;
}

std::vector<uint8_t> BcastDataBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u8(0x5B);
  w.u8(0x01);
  w.u16le(uint16_t(payload.size()));
  w.u8(uint8_t((fragIndex << 4) | ((fragCount - 1) & 0x0F)));
  w.bytes(payload);
  return out;
}

std::optional<BcastDataBody> BcastDataBody::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  if (r.u8() != 0x5B || r.u8() != 0x01) return std::nullopt;
  uint16_t len = r.u16le();
  uint8_t frag = r.u8();
  if (!r.ok() || len > kBcastMaxPayload || r.remaining() != len) {
    return std::nullopt;
  }
  BcastDataBody v;
  v.fragIndex = frag >> 4;
  v.fragCount = uint8_t((frag & 0x0F) + 1);
  if (v.fragIndex >= v.fragCount) return std::nullopt;
  v.payload.resize(len);
  if (len && !r.bytes(v.payload)) return std::nullopt;
  return v;
}

std::vector<uint8_t> VoiceOpenBody::pack() const { return {uint8_t(codec)}; }

std::optional<VoiceOpenBody> VoiceOpenBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 1 || b[0] > uint8_t(VoiceCodec::C3200)) return std::nullopt;
  return VoiceOpenBody{VoiceCodec(b[0])};
}

std::vector<uint8_t> KeyUpdateBody::pack() const { return {newEpoch}; }

std::optional<KeyUpdateBody> KeyUpdateBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 1) return std::nullopt;
  return KeyUpdateBody{b[0]};
}

std::vector<uint8_t> TurnGrantBody::pack() const { return {epoch}; }

std::optional<TurnGrantBody> TurnGrantBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 1) return std::nullopt;
  return TurnGrantBody{b[0]};
}

}  // namespace skw
