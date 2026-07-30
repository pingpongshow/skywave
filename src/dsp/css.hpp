// CSS deep tier (WAVEFORM.md "Reserved CSS Tier", ROADMAP 6.1): LoRa-style
// chirp spread spectrum for modes M0-M3, restoring connectivity below the
// OFDM pilot floor.
//
//   symbol   : cyclically-shifted up-chirp across the family CSS band,
//              2^SF chips, carrying SF bits (gray-coded so an adjacent-bin
//              demod error costs one bit)
//   demod    : non-coherent - dechirp by the conjugate base chirp,
//              integrate per chip, 2^SF-point FFT, magnitude argmax
//   FEC      : Hamming(7,4) hard-decision + block interleaver spreading
//              each codeword across 7 symbols, so a whole lost symbol
//              costs each codeword only one bit
//
// SF by mode: M0=SF8 (deepest) ... M3=SF5 (kModes.spread holds the SF).
// The CSS band per family is the family bandwidth rounded to an integer
// number of baseband samples per chip (12 kHz domain).
#pragma once

#include <complex>
#include <cstdint>
#include <span>
#include <vector>

#include "core/constants.hpp"

namespace skw {

using cfloat = std::complex<float>;

// CSS band + chip geometry for a family.
struct CssPlan {
  double bandHz;       // chip rate == occupied bandwidth
  int samplesPerChip;  // kFsBaseband / bandHz (exact integer)
};
CssPlan cssPlan(Family f);

// Spreading factor for a CSS mode (M0..M3); 0 for non-CSS modes.
inline int cssSf(int mode) {
  return (mode >= 0 && mode <= 3) ? int(kModes[size_t(mode)].spread) : 0;
}

// CSS tier design floors (dB SNR, 3 kHz noise reference, AWGN): lowest
// SNR with one-shot FER <= ~15%, measured 2026-07-29 with the matched-
// filter demod (40 trials/point, sweep in bertool --css). Floors track
// the chip rate: a family's CSS band IS its noise bandwidth, so the
// narrow families reach far below the WAVEFORM.md "-14 dB" tier target
// while WIDE bottoms out near -13. kFloors keeps its reserved sentinels
// until 6.2 activates the tier for AMC.
inline constexpr double cssFloorDb(Family f, int mode) {
  constexpr double t[kNumFamilies][4] = {
      {-13.0, -10.5, -8.0, -5.0},    // WIDE   (2400 Hz chips)
      {-16.5, -14.0, -12.0, -9.0},   // MID    (1000 Hz)
      {-19.5, -17.5, -15.0, -12.0},  // NARROW (480 Hz)
      {-11.5, -9.5, -6.5, -4.0},     // ULTRA  (3000 Hz)
  };
  return (mode >= 0 && mode <= 3) ? t[size_t(f)][mode] : -99.0;
}

// Samples in one CSS symbol at 12 kHz baseband.
size_t cssSymbolSamples(Family f, int sf);

// --- symbol layer ------------------------------------------------------------
// Modulate raw symbol values (each < 2^sf) to unit-RMS baseband.
std::vector<cfloat> cssModulate(Family f, int sf,
                                std::span<const uint16_t> symbols);
// Demodulate one aligned symbol; returns the raw shift value and fills
// `peakRatio` (peak magnitude / mean magnitude - a decode-quality proxy).
uint16_t cssDemodSymbol(Family f, int sf, std::span<const cfloat> sym,
                        float* peakRatio = nullptr);

// --- bit layer (Hamming(7,4) + interleave + gray) ------------------------------
// Payload bytes -> CSS symbols. Symbol count is padded up to a multiple
// of 7 (the interleaver depth).
std::vector<uint16_t> cssEncode(std::span<const uint8_t> bytes, int sf);
// Symbols back to bytes; `byteCount` trims the padding. Hard-decision
// Hamming corrects one bit per codeword.
std::vector<uint8_t> cssDecode(std::span<const uint16_t> symbols, int sf,
                               size_t byteCount);
// Symbols required to carry `byteCount` payload bytes at `sf`.
size_t cssSymbolsForBytes(size_t byteCount, int sf);

// --- frame payload layer -------------------------------------------------------
// Modulate a byte payload into a CSS baseband section (encode + modulate);
// demodulate an aligned section back into bytes.
std::vector<cfloat> cssModulatePayload(Family f, int mode,
                                       std::span<const uint8_t> bytes);
std::vector<uint8_t> cssDemodPayload(Family f, int mode,
                                     std::span<const cfloat> section,
                                     size_t byteCount);

}  // namespace skw
