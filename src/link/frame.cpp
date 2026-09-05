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
  return v >= uint8_t(LinkType::Call) && v <= uint8_t(LinkType::TurnGrant);
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
  w.u8(cpMode);
  w.u8(maxMode);
  w.u8(macProfile);
  uint8_t present = (hasResumeToken ? 1 : 0) | (kexPublic.empty() ? 0 : 2);
  w.u8(present);
  if (hasResumeToken) w.bytes(resumeToken);
  if (!kexPublic.empty()) {
    w.u8(uint8_t(kexPublic.size()));
    w.bytes(kexPublic);
  }
  return out;
}

std::optional<StartBody> StartBody::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  StartBody v;
  v.caps = r.u16le();
  v.initialMode = r.u8();
  v.cpMode = r.u8();
  v.maxMode = r.u8();
  v.macProfile = r.u8();
  uint8_t present = r.u8();
  if (!r.ok() || (present & ~3u)) return std::nullopt;
  v.hasResumeToken = present & 1;
  if (v.hasResumeToken && !r.bytes(v.resumeToken)) return std::nullopt;
  if (present & 2) {
    uint8_t len = r.u8();
    if (!r.ok() || len == 0 || r.remaining() < len) return std::nullopt;
    v.kexPublic.resize(len);
    if (!r.bytes(v.kexPublic)) return std::nullopt;
  }
  if (!r.ok() || r.remaining() != 0) return std::nullopt;
  if (v.caps & kCapReservedMask) return std::nullopt;
  // M0-M3 are valid on the wire as of 6.2 (CSS tier); inactive table
  // rows are still rejected.
  if (!kModes[v.initialMode % kNumModes].active) v.initialMode = kFirstMode;
  if (!kModes[v.maxMode % kNumModes].active) v.maxMode = kFirstMode;
  if (v.initialMode >= kNumModes || v.maxMode >= kNumModes || v.cpMode > 2) {
    return std::nullopt;
  }
  return v;
}

std::vector<uint8_t> StartAckBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u8(finalMode);
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
  if (!kModes[v.finalMode % kNumModes].active) v.finalMode = kFirstMode;
  if (v.finalMode >= kNumModes) return std::nullopt;
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
  if (!kModes[v.targetMode % kNumModes].active) v.targetMode = kFirstMode;
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
  if (!kModes[v.probeMode % kNumModes].active) v.probeMode = kFirstMode;
  if (v.probeMode >= kNumModes) return std::nullopt;
  return v;
}

std::vector<uint8_t> ProbeAckBody::pack() const {
  return {probeId, uint8_t(snrHalfDb), decodedOk};
}

std::optional<ProbeAckBody> ProbeAckBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 3) return std::nullopt;
  return ProbeAckBody{b[0], int8_t(b[1]), b[2]};
}

std::vector<uint8_t> MaskUpdateBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u8(epoch);
  w.u8(family);
  w.bytes(mask);
  return out;
}

std::optional<MaskUpdateBody> MaskUpdateBody::parse(
    std::span<const uint8_t> b) {
  if (b.size() != 2 + 9) return std::nullopt;
  ByteReader r(b);
  MaskUpdateBody v;
  v.epoch = r.u8();
  v.family = r.u8();
  if (v.family >= kNumFamilies) return std::nullopt;
  if (!r.bytes(v.mask)) return std::nullopt;
  return v;
}

std::vector<uint8_t> MaskAckBody::pack() const { return {epoch}; }

std::optional<MaskAckBody> MaskAckBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 1) return std::nullopt;
  return MaskAckBody{b[0]};
}

std::vector<uint8_t> KeepaliveBody::pack() const { return {remoteSnr}; }

std::optional<KeepaliveBody> KeepaliveBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 1) return std::nullopt;
  return KeepaliveBody{b[0]};
}

namespace {
void packFileCkpt(ByteWriter& w, bool has, uint32_t id, uint64_t off) {
  w.u8(has ? 1 : 0);
  if (has) {
    w.u32le(id);
    w.u64le(off);
  }
}
bool parseFileCkpt(ByteReader& r, bool& has, uint32_t& id, uint64_t& off) {
  const uint8_t flag = r.u8();
  if (!r.ok() || flag > 1) return false;
  has = flag == 1;
  if (has) {
    id = r.u32le();
    off = r.u64le();
  }
  return r.ok() && r.remaining() == 0;
}
}  // namespace

std::vector<uint8_t> ResumeReqBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.bytes(token);
  w.u16le(lastRxSeq);
  packFileCkpt(w, hasFileCkpt, fileId, fileOffset);
  return out;
}

std::optional<ResumeReqBody> ResumeReqBody::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  ResumeReqBody v;
  if (!r.bytes(v.token)) return std::nullopt;
  v.lastRxSeq = r.u16le();
  if (!parseFileCkpt(r, v.hasFileCkpt, v.fileId, v.fileOffset)) {
    return std::nullopt;
  }
  return v;
}

std::vector<uint8_t> ResumeAckBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u16le(lastRxSeq);
  packFileCkpt(w, hasFileCkpt, fileId, fileOffset);
  return out;
}

std::optional<ResumeAckBody> ResumeAckBody::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  ResumeAckBody v;
  v.lastRxSeq = r.u16le();
  if (!parseFileCkpt(r, v.hasFileCkpt, v.fileId, v.fileOffset)) {
    return std::nullopt;
  }
  return v;
}

std::vector<uint8_t> DiscBody::pack() const { return {uint8_t(reason)}; }

std::optional<DiscBody> DiscBody::parse(std::span<const uint8_t> b) {
  if (b.size() != 1) return std::nullopt;
  if (b[0] > uint8_t(DiscReason::Fault)) return std::nullopt;
  return DiscBody{DiscReason(b[0])};
}

std::vector<uint8_t> BeaconBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  putCall(w, callsign);
  w.u8(snrFloorPlus20);
  w.u8(family);
  w.u8(swVersion);
  return out;
}

std::optional<BeaconBody> BeaconBody::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  BeaconBody v;
  auto call = getCall(r);
  if (!call) return std::nullopt;
  v.callsign = *call;
  v.snrFloorPlus20 = r.u8();
  v.family = r.u8();
  v.swVersion = r.u8();
  if (!r.ok() || r.remaining() != 0) return std::nullopt;
  if (v.family >= kNumFamilies) return std::nullopt;
  return v;
}

std::vector<uint8_t> CqBody::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  putCall(w, callsign);
  w.u16le(caps);
  return out;
}

std::optional<CqBody> CqBody::parse(std::span<const uint8_t> b) {
  ByteReader r(b);
  CqBody v;
  auto call = getCall(r);
  if (!call) return std::nullopt;
  v.callsign = *call;
  v.caps = r.u16le();
  if (!r.ok() || r.remaining() != 0) return std::nullopt;
  if (v.caps & kCapReservedMask) return std::nullopt;
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
