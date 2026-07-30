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
  uint8_t mode = 8;        // payload ladder mode M4..M13
  CpMode cpMode = CpMode::Medium;
  uint8_t frameType = 0;   // 6 bits (PROTOCOL.md)
  uint16_t length = 0;     // total link payload bytes (12 bits)
  uint8_t seqLow = 0;
};

inline constexpr int kPhyHeaderBytes = 9;  // 50 field bits + pad + crc16

// Pack to 9 bytes (CRC16 over the first 7); parse returns nullopt on bad
// CRC/magic/version.
void packPhyHeader(const PhyHeader& h, uint8_t out[kPhyHeaderBytes]);
std::optional<PhyHeader> parsePhyHeader(std::span<const uint8_t> bytes);

// Header transport parameters by class (fixed, never negotiated).
inline int headerSpread(bool robust) {
  return robust ? kHeaderSpreadRobust : kHeaderSpreadFast;
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
  int mode = 8;                    // payload mode (M4..M13)
  CpMode cpMode = CpMode::Medium;  // payload CP
  uint8_t frameType = 0;
  uint8_t seqLow = 0;
  std::span<const uint8_t> payload;  // link frame bytes, <= 4095
  // Session carrier mask, PAYLOAD symbols only (LINK_ADAPTATION.md);
  // nullptr = all carriers usable.
  const CarrierMask* mask = nullptr;
};

// Frame geometry shared by TX and (later) RX.
struct FrameGeometry {
  int preambleSamples = 0;
  int zcSymbols = 0;
  int headerSymbols = 0;   // MEDIUM CP each
  int payloadSymbols = 0;  // params.cpMode each
  int payloadCodewords = 0;
  int totalSamples = 0;
};
FrameGeometry computeGeometry(const TxFrameParams& p);

// On-air duration of a frame (PROTOCOL.md timers are airtime-derived).
inline double frameAirtimeSec(const TxFrameParams& p) {
  return double(computeGeometry(p).totalSamples) / kFsBaseband;
}

// Build the complete frame at 12 kHz complex baseband, unit RMS, after
// PAPR soft-clipping and per-family band filtering.
std::vector<cfloat> buildFrameBaseband(const TxFrameParams& p);

// --- pieces exposed for tests and the RX chain -----------------------------

// Whiten payload, split into header-piggyback + N=1944 payload codewords,
// LDPC-encode + interleave each; returns coded-bit vectors (one bit/byte).
struct FrameBits {
  std::vector<uint8_t> headerCoded;               // 648 bits
  std::vector<std::vector<uint8_t>> payloadCoded;  // 1944 bits each
};
FrameBits buildFrameBits(const TxFrameParams& p);

// Assemble OFDM symbols carrying `coded` bits at the given parameters,
// starting at symbol number symIdx0 (pilot pattern phase). Appends
// baseband samples to out and returns the number of symbols emitted.
int appendOfdmSymbols(Family family, Constellation constellation, int spread,
                      CpMode cp, std::span<const uint8_t> codedBits,
                      int symIdx0, std::vector<cfloat>& out,
                      const CarrierMask* mask = nullptr);

}  // namespace skw
