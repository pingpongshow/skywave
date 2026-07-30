// CSS deep tier (WAVEFORM.md "Reserved CSS Tier", ROADMAP 6.1): LoRa-style
// chirp spread spectrum for modes M0-M3, restoring connectivity below the
// OFDM pilot floor.
//
//   symbol   : cyclically-shifted up-chirp across the family CSS band,
//              2^SF chips, carrying SF bits (gray-coded so an adjacent-bin
//              demod error costs one bit)
//   demod    : non-coherent - dechirp by the conjugate base chirp,
//              integrate per chip, 2^SF-point FFT, magnitude argmax
//   FEC      : Hamming(7,4) hard-decision + full-message row-column
//              interleave spreading each codeword across the whole
//              section, so a burst loss (lost symbol, second-long fade)
//              costs each codeword ~1 correctable bit
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
// while WIDE bottoms out near -13. Stored in kFloors alongside the OFDM
// ladder (WAVEFORM.md "CSS Deep Tier" table).
inline constexpr double cssFloorDb(Family f, int mode) {
  return (mode >= 0 && mode <= 3) ? floorDb(f, mode) : -99.0;
}

// Samples in one CSS symbol at 12 kHz baseband.
size_t cssSymbolSamples(Family f, int sf);

// CSS frame-on-air layout (WAVEFORM.md "CSS Deep Tier"): chirp preamble
// (sig Css) + kCssSyncSymbols known shift-0 SF8 symbols (fine timing +
// multipath profile) + PHY header at fixed SF8 transport + payload at
// the header mode's SF with a trailing CRC-16 (payload integrity - CSS
// Hamming FEC can miscorrect silently, unlike LDPC).
inline constexpr int kCssHeaderSf = 8;
inline constexpr int kCssSyncSymbols = 2;
inline constexpr int kCssPayloadCrcBytes = 2;
// The 9-byte PHY header rides twice, as two independently interleaved
// sections ~2 s apart: a second-long fade takes at most one copy, and
// the header CRC-16 arbitrates (time diversity where the short section
// cannot interleave its way out of an outage).
inline constexpr int kCssHeaderCopies = 2;
// A known shift-0 SF8 pilot chirp precedes every kCssPilotInterval data
// symbols of both header and payload sections: the receiver re-anchors
// timing (sample-clock drift) and re-estimates the multipath tap profile
// (Rayleigh taps fade on ~1 s scales while deep frames run 10+ s) at
// each pilot. ~12% overhead.
inline constexpr int kCssPilotInterval = 8;

// A resolved multipath component for RAKE symbol scoring: `delay` in
// baseband samples relative to the demod window start, magnitude weight
// from the sync-symbol correlation profile.
struct CssTap {
  int delay = 0;
  float weight = 1.0f;
};

// --- symbol layer ------------------------------------------------------------
// Modulate raw symbol values (each < 2^sf) to unit-RMS baseband.
std::vector<cfloat> cssModulate(Family f, int sf,
                                std::span<const uint16_t> symbols);
// Demodulate one aligned symbol; returns the raw shift value and fills
// `peakRatio` (peak magnitude / mean magnitude - a decode-quality proxy).
uint16_t cssDemodSymbol(Family f, int sf, std::span<const cfloat> sym,
                        float* peakRatio = nullptr);
// RAKE variant: score each shift as the tap-weighted sum of correlation
// magnitudes at the taps' delays (non-coherent path combining; the tap
// list comes from the frame's sync-symbol correlation profile).
uint16_t cssDemodSymbolTaps(Family f, int sf, std::span<const cfloat> sym,
                            std::span<const CssTap> taps,
                            float* peakRatio = nullptr);
// The base up-chirp for one symbol (2^sf chips) - the frame receiver
// correlates against this for sync refinement.
std::vector<cfloat> cssBaseChirp(Family f, int sf);

// --- LDPC payload transport (frame payload sections) ---------------------------
// The frame payload rides LDPC r1/2 over CSS symbols with soft demod
// (WAVEFORM.md "CSS Deep Tier"): hard-decision Hamming hits a sharp
// 1-error-per-codeword cliff under second-long Rayleigh fades, several
// dB short of what soft LDPC recovers. The Hamming bit layer below
// remains the header transport (short, duplicated) and the aligned
// modem-layer API.
std::vector<uint16_t> cssMapCodedBits(std::span<const uint8_t> bits, int sf);
void cssSoftDemodSymbol(Family f, int sf, std::span<const cfloat> sym,
                        std::span<const CssTap> taps,
                        std::vector<float>& llr);

// --- bit layer (Hamming(7,4) + interleave + gray) ------------------------------
// Payload bytes -> CSS symbols (full-message row-column interleave).
std::vector<uint16_t> cssEncode(std::span<const uint8_t> bytes, int sf);
// Symbols back to bytes; `byteCount` trims the padding. Hard-decision
// Hamming corrects one bit per codeword.
std::vector<uint8_t> cssDecode(std::span<const uint16_t> symbols, int sf,
                               size_t byteCount);
// Symbols required to carry `byteCount` payload bytes at `sf`.
size_t cssSymbolsForBytes(size_t byteCount, int sf);

// --- frame payload layer -------------------------------------------------------
// Modulate a byte payload into a CSS baseband section (encode + modulate);
// demodulate an aligned section back into bytes. The SF-level pair is
// what the PHY framer uses (header transport is fixed SF8); the mode
// wrappers map through cssSf().
std::vector<cfloat> cssModulateBytes(Family f, int sf,
                                     std::span<const uint8_t> bytes);
std::vector<uint8_t> cssDemodBytes(Family f, int sf,
                                   std::span<const cfloat> section,
                                   size_t byteCount,
                                   std::span<const CssTap> taps = {});
std::vector<cfloat> cssModulatePayload(Family f, int mode,
                                       std::span<const uint8_t> bytes);
std::vector<uint8_t> cssDemodPayload(Family f, int mode,
                                     std::span<const cfloat> section,
                                     size_t byteCount);

}  // namespace skw
