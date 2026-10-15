// Shared TX/RX symbol layout: pilot/data carrier assignment and pilot
// values (WAVEFORM.md "Pilots"). Everything here must be bit-identical
// between transmitter and receiver.
#pragma once

#include <cstdlib>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "core/constants.hpp"

namespace skw {

using cfloat = std::complex<float>;

// Deterministic 64-bit mix (splitmix64 finalizer) for PRN phases.
inline uint64_t skwHash(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// Quarter-turn phase from a hash value.
inline cfloat prnPhase(uint64_t h) {
  switch (h & 3u) {
    case 0: return {1.0f, 0.0f};
    case 1: return {0.0f, 1.0f};
    case 2: return {-1.0f, 0.0f};
    default: return {0.0f, -1.0f};
  }
}

inline bool isPilotCarrier(int carrierIdx, int symIdx) {
  return (carrierIdx % kPilotSpacing) == (symIdx % kPilotSpacing);
}

// Pilot value for (carrier, symbol): +2 dB boost, PRN quarter-turn phase.
inline cfloat pilotValue(int carrierIdx, int symIdx) {
  const float boost = std::pow(10.0f, float(kPilotBoostDb) / 20.0f);
  uint64_t h = skwHash(kSeedPilotPhase ^ (uint64_t(carrierIdx) << 20) ^
                       uint64_t(symIdx));
  return boost * prnPhase(h);
}

// Spread-copy PRN phase for (source symbol index, copy k, OFDM symbol).
inline cfloat spreadPhase(int srcIdx, int copy, int symIdx) {
  uint64_t h = skwHash(kSeedSpreadPhase ^ (uint64_t(srcIdx) << 24) ^
                       (uint64_t(copy) << 16) ^ uint64_t(symIdx));
  return prnPhase(h);
}

struct SymbolLayout {
  std::vector<int> pilotCarriers;  // carrier indices (0-based in family)
  std::vector<int> dataCarriers;
};

// Carrier mask (LINK_ADAPTATION.md "Carrier Masking"): wire-compatible
// bitmap, bit set = carrier USABLE (LSB = lowest active carrier).
// Default: everything usable. Masked carriers carry no data and no
// pilots (skipped, not relocated - the delay-domain channel estimator
// interpolates across the gap). The mask applies to PAYLOAD symbols
// only; header symbols always use the full carrier set so any receiver
// (including monitors) can decode the self-describing header.
struct CarrierMask {
  // In-memory capacity covers the widest profile family (VHF-FM: 176
  // carriers -> 22 bytes). The 9-byte WIRE mask (MaskUpdateBody) is
  // unchanged: masking is negotiated only on HF, whose families stay
  // <= 72 carriers.
  std::array<uint8_t, 22> bits;
  CarrierMask() { bits.fill(0xFF); }
  bool usable(int c) const {
    return (bits[size_t(c >> 3)] >> (c & 7)) & 1;
  }
  void exclude(int c) { bits[size_t(c >> 3)] &= uint8_t(~(1u << (c & 7))); }
  int maskedCount(Family f) const {
    int m = 0;
    for (int c = 0; c < plan(f).carriers(); ++c) m += usable(c) ? 0 : 1;
    return m;
  }
  bool operator==(const CarrierMask&) const = default;
};

inline SymbolLayout symbolLayout(Family f, int symIdx,
                                 const CarrierMask* mask = nullptr) {
  SymbolLayout out;
  const int n = plan(f).carriers();
  for (int c = 0; c < n; ++c) {
    if (mask && !mask->usable(c)) continue;
    if (isPilotCarrier(c, symIdx)) out.pilotCarriers.push_back(c);
    else out.dataCarriers.push_back(c);
  }
  return out;
}

// Spreading operates on a GROUP of consecutive OFDM symbols whose pooled
// data carriers can hold at least one spread-S source symbol. For most
// family/spread combinations the group is a single symbol; NARROW at S16
// (11-12 data carriers) needs 2 symbols - frequency+time spreading.
inline int spreadGroupSymbols(Family f, int spread,
                              const CarrierMask* mask = nullptr) {
  // Minimum data carriers over the 8-symbol pilot cycle.
  int minNd = 1 << 30;
  for (int s = 0; s < kPilotSpacing; ++s) {
    minNd = std::min(minNd,
                     int(symbolLayout(f, s, mask).dataCarriers.size()));
  }
  minNd = std::max(minNd, 1);
  int g = 1;
  while (g * minNd < spread) ++g;
  return g;
}

// Source (pre-spreading) constellation symbols carried by one spread group
// starting at symIdx.
inline int sourceSymbolsPerGroup(Family f, int symIdx, int spread,
                                 const CarrierMask* mask = nullptr) {
  const int g = spreadGroupSymbols(f, spread, mask);
  int pool = 0;
  for (int s = 0; s < g; ++s) {
    pool += int(symbolLayout(f, symIdx + s, mask).dataCarriers.size());
  }
  return pool / spread;
}

// LEFTOVER CELLS. A spread group's pool rarely divides by the spreading
// factor; the remainder cells (up to 32% at M4) used to stay empty. When
// on, remainder cell j carries an extra copy of source (j % nSrc), copy
// index S + j / nSrc - more frequency diversity, same rate. Both ends
// must agree (on by default; SKW_LEFTOVER_COPIES=0 is the A/B).
// Spreads it applies to: S <= 8 (M5-M7). The frame is normalized to unit
// RMS, so filling the cells moves energy INTO the payload and away from
// the preamble/ZC/header: at M5/M6 the payload is the limit and gains
// (NARROW M5 at its floor 77 -> 13%), at M4 (S=16) the header is and
// lost (NARROW M4 47 -> 83%, header failures 20 -> 37).
inline bool leftoverCopiesForSpread(int spread) {
  return spread > 1 && spread <= 8;
}
inline bool leftoverCopiesEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("SKW_LEFTOVER_COPIES");
    return !(e && e[0] == '0');
  }();
  return on;
}

// Map a family carrier index to a baseband FFT bin (0..319).
inline int carrierToBasebandBin(Family f, int carrierIdx) {
  int b = plan(f).firstBin + carrierIdx - centerBin();
  return b >= 0 ? b : kFftBaseband + b;
}

// TX time-domain scale for OFDM symbols: raw ifft of n unit carriers has
// RMS sqrt(n)/N; scaling by N/sqrt(n) gives unit RMS, matching the unit-RMS
// ZC sync symbols and chirp so the whole frame is level-uniform. The RX
// divides FFT bins by this constant so pilot-measured H agrees with the
// ZC-seeded estimate (both then equal the physical channel gain times the
// frame normalization).
inline float ofdmTimeScale(Family f) {
  return float(kFftBaseband) / std::sqrt(float(plan(f).carriers()));
}

}  // namespace skw
