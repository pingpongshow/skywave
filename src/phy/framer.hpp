// PHY framer: self-describing header + codeword segmentation + frame
// assembly (WAVEFORM.md "PHY Header", "Header Codeword Payload Piggyback",
// "Frame-on-Air Layout").
#pragma once

#include <complex>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/constants.hpp"
#include "phy/layout.hpp"
#include "dsp/chirp.hpp"

namespace skw {

using cfloat = std::complex<float>;

struct PhyHeader {
  uint8_t version = kWireVersion;
  Waveform waveform = Waveform::Ofdm;
  Family family = Family::Wide;
  uint8_t mode = 8;        // payload ladder mode M0..M14
  CpMode cpMode = CpMode::Medium;
  uint8_t frameType = 0;   // 6 bits (PROTOCOL.md)
  uint16_t length = 0;     // total link payload bytes (12 bits)
  uint8_t seqLow = 0;
  // CSS frames only: number of header copies on air (1 or 2). Carried
  // in a former pad bit (0 = two copies, the v1 layout, so the wire is
  // unchanged unless a single-copy frame is deliberately sent). The
  // receiver learns the layout from copy 1, which it decodes before it
  // needs to know where the payload starts.
  uint8_t cssHeaderCopies = 2;
  // POLL: the sender stops after this burst and listens - the peer may
  // answer now. Lives in the PHY header so a burst whose payload failed
  // still tells the receiver whether another burst is following it.
  bool poll = false;
};

inline constexpr int kPhyHeaderBytes = 9;  // 52 field bits + pad + crc16
// Largest PHY payload: the header's 12-bit length field.
inline constexpr size_t kMaxPhyPayloadBytes = 4095;

// MSB-first bit (one per byte) <-> byte packing, shared by TX and RX.
inline std::vector<uint8_t> bytesToBits(std::span<const uint8_t> bytes) {
  std::vector<uint8_t> bits;
  bits.reserve(bytes.size() * 8);
  for (uint8_t b : bytes) {
    for (int i = 7; i >= 0; --i) bits.push_back((b >> i) & 1);
  }
  return bits;
}
inline std::vector<uint8_t> bitsToBytes(std::span<const uint8_t> bits) {
  std::vector<uint8_t> out((bits.size() + 7) / 8, 0);
  for (size_t i = 0; i < bits.size(); ++i) {
    out[i / 8] |= uint8_t((bits[i] & 1) << (7 - i % 8));
  }
  return out;
}

// Pack to 9 bytes (CRC16 over the first 7); parse returns nullopt on bad
// CRC/magic/version.
void packPhyHeader(const PhyHeader& h, uint8_t out[kPhyHeaderBytes]);
std::optional<PhyHeader> parsePhyHeader(std::span<const uint8_t> bytes);

// Header transport parameters by class (fixed, never negotiated).
inline int headerSpread(bool robust) {
  return robust ? kHeaderSpreadRobust : activeProfile().headerSpreadFast;
}
inline bool robustHeaderForSig(ChirpSig sig) {
  return sig != ChirpSig::Fast;
}

// Payload codeword sizing, deterministic from the remaining info bits and
// the mode's rate (TX and RX must agree): short frames use N=648 so robust
// modes don't pay a 1944-bit codeword for a few dozen payload bytes. r5/6
// has no N=648 matrix.
inline int payloadCodewordN(size_t remainingInfoBits, CodeRate r) {
  if (r.num == 5) return kLdpcNLong;
  size_t k648 = size_t(kLdpcNShort) * r.num / r.den;
  return remainingInfoBits <= k648 ? kLdpcNShort : kLdpcNLong;
}

// CSS frame payload transport (WAVEFORM.md "CSS Deep Tier"): whitened
// payload + CRC-16, LDPC r1/2 over soft-demodulated symbols. ALWAYS
// N=648 codewords: at deep-tier symbol rates a single N=1944 codeword
// is minutes of airtime, and the link keeps deep frames small anyway.
inline size_t cssPayloadCodewords(size_t phyBytes) {
  const size_t bits = (phyBytes + 2) * 8;  // + kCssPayloadCrcBytes
  return (bits + size_t(kLdpcNShort) / 2 - 1) / (size_t(kLdpcNShort) / 2);
}
inline size_t cssPayloadCodedBits(size_t phyBytes) {
  return cssPayloadCodewords(phyBytes) * size_t(kLdpcNShort);
}

struct TxFrameParams {
  Family family = Family::Wide;
  ChirpSig sig = ChirpSig::Fast;   // preamble signature / header class
  int mode = 8;                    // payload mode (M0..M14)
  CpMode cpMode = CpMode::Medium;  // payload CP
  uint8_t frameType = 0;
  uint8_t seqLow = 0;
  // CSS frames: header copies on air (2 = v1 time-diversity layout; 1 =
  // connected-session short layout, capability-gated). See PhyHeader.
  uint8_t cssHeaderCopies = 2;
  bool poll = false;  // see PhyHeader::poll
  std::span<const uint8_t> payload;  // link frame bytes, <= kMaxPhyPayloadBytes
  // Session carrier mask, PAYLOAD symbols only (LINK_ADAPTATION.md);
  // nullptr = all carriers usable.
  const CarrierMask* mask = nullptr;
};

// Frame geometry, computed from the parameters alone (nothing is built:
// the link layer asks for airtimes many times per frame).
//   OFDM: header/payload symbols are OFDM symbols (header at MEDIUM CP,
//         payload at params.cpMode).
//   CSS:  headerSymbols = sync + header-copy SF8 symbols incl. their
//         pilots, payloadSymbols = SF(mode) data symbols + SF8 pilots.
struct FrameGeometry {
  int headerSymbols = 0;
  int payloadSymbols = 0;
  int payloadCodewords = 0;  // LDPC codewords after the header piggyback
  int totalSamples = 0;
};
FrameGeometry computeGeometry(const TxFrameParams& p);

// Which modes honour the session carrier mask (LINK_ADAPTATION.md
// "Carrier Masking"): spread modes M4-M7 skip it - spreading already
// averages over interference. ONE predicate for the transmitter, the
// receiver's payload walk and the airtime estimate: the receiver once
// applied the mask at every mode while the transmitter did not, so after
// a mask was established every DATA frame below M8 failed to decode
// (control frames, which fit in the header piggyback, kept passing) -
// measured as the churn soak's "usable channel, no progress" stall.
inline bool modeUsesCarrierMask(int mode) { return mode >= 8; }

// The mask the payload of a `mode` frame actually uses - ONE rule for the
// transmitter's geometry/symbols and the receiver's walk, so they can never
// disagree. Spread modes skip masking; and a mask leaving fewer than half
// the family's carriers is not an interference mask (the detector proposes
// at most 30%): it is ignored rather than trusted - an all-zero mask from a
// peer used to make the symbol-count loops run forever.
inline const CarrierMask* effectiveMask(const CarrierMask* m, Family f,
                                        int mode) {
  if (!m || !modeUsesCarrierMask(mode)) return nullptr;
  return 2 * m->maskedCount(f) > plan(f).carriers() ? nullptr : m;
}

// On-air duration of a frame (PROTOCOL.md timers are airtime-derived).
inline double frameAirtimeSec(const TxFrameParams& p) {
  return double(computeGeometry(p).totalSamples) / kFsBaseband;
}

// Build the complete frame at 12 kHz complex baseband, unit RMS, after
// PAPR soft-clipping and per-family band filtering.
std::vector<cfloat> buildFrameBaseband(const TxFrameParams& p);

// --- pieces exposed for tests ------------------------------------------------

// OFDM frames: whiten payload, split into header piggyback + payload
// codewords (sized by payloadCodewordN), LDPC-encode + interleave each;
// returns coded-bit vectors (one bit/byte).
struct FrameBits {
  std::vector<uint8_t> headerCoded;                // 648 bits
  std::vector<std::vector<uint8_t>> payloadCoded;  // 648 or 1944 bits each
};
FrameBits buildFrameBits(const TxFrameParams& p);

// Assemble OFDM symbols carrying `coded` bits at the given parameters,
// starting at symbol number symIdx0 (pilot pattern phase). Appends
// baseband samples to out and returns the number of symbols emitted.
int appendOfdmSymbols(Family family, Constellation constellation, int spread,
                      CpMode cp, std::span<const uint8_t> codedBits,
                      int symIdx0, std::vector<cfloat>& out,
                      const CarrierMask* mask = nullptr,
                      bool leftoverCopies = false);

}  // namespace skw
