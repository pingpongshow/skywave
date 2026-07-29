// Skywave band-profile constants.
//
// Design rule (ARCHITECTURE.md): every numerology value lives HERE or in the
// mode tables below - never as a literal in logic. The HF profile is the only
// v1 profile; a future VHF-FM profile is a second BandProfile instance.
#pragma once

#include <array>
#include <cstdint>

namespace skw {

// ---------------------------------------------------------------- sample domain
inline constexpr int    kFsAudio        = 48000;  // external real passband rate
inline constexpr int    kFsBaseband     = 12000;  // internal complex rate
inline constexpr int    kDecimation     = 4;      // 48k -> 12k
inline constexpr double kCenterHz       = 1500.0; // waveform center (audio)

// ---------------------------------------------------------------------- OFDM grid
inline constexpr int    kFftAudio       = 1280;   // at 48 kHz
inline constexpr int    kFftBaseband    = kFftAudio / kDecimation;  // 320
inline constexpr double kSubcarrierHz   = double(kFsAudio) / kFftAudio; // 37.5
inline constexpr int    kCenterBin      = 40;     // 40 * 37.5 = 1500 Hz
static_assert(kFftBaseband == 320);
static_assert(kCenterBin * kSubcarrierHz == kCenterHz);

enum class CpMode : uint8_t { Short = 0, Medium = 1, Long = 2 };
// CP lengths in audio-rate samples (divide by kDecimation for baseband).
inline constexpr std::array<int, 3> kCpSamplesAudio = {96, 192, 288};
static_assert(kCpSamplesAudio[0] % kDecimation == 0 &&
              kCpSamplesAudio[1] % kDecimation == 0 &&
              kCpSamplesAudio[2] % kDecimation == 0);
inline constexpr CpMode kCpDefault = CpMode::Medium;

inline constexpr double symbolRate(CpMode cp) {
  return double(kFsAudio) / (kFftAudio + kCpSamplesAudio[size_t(cp)]);
}

// ------------------------------------------------------------------- families
enum class Family : uint8_t { Wide = 0, Mid = 1, Narrow = 2, Ultra = 3 };
inline constexpr int kNumFamilies = 4;

struct FamilyPlan {
  const char* name;
  int firstBin;        // audio-grid bin index (bin * 37.5 Hz)
  int lastBin;         // inclusive
  double chirpStartHz; // acquisition chirp sweep
  double chirpEndHz;
  bool optIn;          // requires explicit enable (ULTRA)
  constexpr int carriers() const { return lastBin - firstBin + 1; }
};

inline constexpr std::array<FamilyPlan, kNumFamilies> kFamilies = {{
    // name       first last  chirpStart chirpEnd  optIn
    {"WIDE",        8,   71,    300.0,    2700.0,  false},
    {"MID",        27,   53,   1000.0,    2000.0,  false},
    {"NARROW",     34,   46,   1275.0,    1725.0,  false},
    {"ULTRA",       5,   76,    200.0,    2850.0,  true},
}};
inline constexpr const FamilyPlan& plan(Family f) {
  return kFamilies[size_t(f)];
}
// Carrier-count invariants from WAVEFORM.md.
static_assert(plan(Family::Wide).carriers() == 64);
static_assert(plan(Family::Mid).carriers() == 27);
static_assert(plan(Family::Narrow).carriers() == 13);
static_assert(plan(Family::Ultra).carriers() == 72);
// Odd-count families center exactly on bin 40 (1500 Hz).
static_assert((plan(Family::Mid).firstBin + plan(Family::Mid).lastBin) / 2 ==
              kCenterBin);
static_assert((plan(Family::Narrow).firstBin + plan(Family::Narrow).lastBin) /
                  2 == kCenterBin);

// --------------------------------------------------------------------- pilots
inline constexpr int    kPilotSpacing   = 8;    // 1-in-8 carriers per symbol
inline constexpr double kPilotBoostDb   = 2.0;

// ------------------------------------------------------------------- preamble
inline constexpr double kChirpRepSec        = 0.160;
inline constexpr int    kChirpRepsFast      = 2;  // up-up
inline constexpr int    kChirpRepsRobust    = 4;  // up-down-up-down
inline constexpr int    kChirpRepsCalling   = 4;  // up-up-up-up
inline constexpr int    kZcSymbolsFast      = 3;
inline constexpr int    kZcSymbolsRobust    = 6;
inline constexpr double kCfoCaptureConnHz   = 5.0;   // connected preambles
inline constexpr double kCfoCaptureCallHz   = 50.0;  // calling, stepped search
inline constexpr double kCfoSearchStepHz    = 12.5;

// ------------------------------------------------------------------ mode ladder
enum class Waveform : uint8_t { Ofdm = 0, Css = 1 /* reserved */ };
enum class Constellation : uint8_t { Qpsk = 0, Qam16 = 1, Qam64 = 2 };
inline constexpr int bitsPerSymbol(Constellation c) {
  return c == Constellation::Qpsk ? 2 : c == Constellation::Qam16 ? 4 : 6;
}

struct CodeRate {
  uint8_t num, den;
  constexpr double ratio() const { return double(num) / den; }
};

struct ModeDef {
  Waveform waveform;
  Constellation constellation;
  CodeRate rate;
  uint8_t spread;   // frequency-domain spreading factor S
  bool active;      // false = reserved (CSS tier, v1)
};

inline constexpr int kNumModes  = 14;   // M0..M13
inline constexpr int kFirstMode = 4;    // lowest active mode in v1
inline constexpr std::array<ModeDef, kNumModes> kModes = {{
    // M0-M3: reserved CSS tier
    {Waveform::Css, Constellation::Qpsk, {1, 2}, 1, false},
    {Waveform::Css, Constellation::Qpsk, {1, 2}, 1, false},
    {Waveform::Css, Constellation::Qpsk, {1, 2}, 1, false},
    {Waveform::Css, Constellation::Qpsk, {1, 2}, 1, false},
    // M4-M13: unified spread-OFDM ladder
    {Waveform::Ofdm, Constellation::Qpsk,  {1, 2}, 16, true},  // M4
    {Waveform::Ofdm, Constellation::Qpsk,  {1, 2},  8, true},  // M5
    {Waveform::Ofdm, Constellation::Qpsk,  {1, 2},  4, true},  // M6
    {Waveform::Ofdm, Constellation::Qpsk,  {1, 2},  2, true},  // M7
    {Waveform::Ofdm, Constellation::Qpsk,  {1, 2},  1, true},  // M8
    {Waveform::Ofdm, Constellation::Qpsk,  {3, 4},  1, true},  // M9
    {Waveform::Ofdm, Constellation::Qam16, {1, 2},  1, true},  // M10
    {Waveform::Ofdm, Constellation::Qam16, {3, 4},  1, true},  // M11
    {Waveform::Ofdm, Constellation::Qam64, {2, 3},  1, true},  // M12
    {Waveform::Ofdm, Constellation::Qam64, {5, 6},  1, true},  // M13
}};

// Average data carriers per symbol = carriers * (spacing-1)/spacing.
inline constexpr double avgDataCarriers(Family f) {
  return plan(f).carriers() * double(kPilotSpacing - 1) / kPilotSpacing;
}
// Gross (uncoded) waveform bit rate for a mode, bits/s, at a given CP.
inline constexpr double grossRate(Family f, int mode, CpMode cp) {
  const ModeDef& m = kModes[size_t(mode)];
  return avgDataCarriers(f) * bitsPerSymbol(m.constellation) *
         symbolRate(cp) / m.spread;
}
// Net (post-FEC) payload rate, bits/s.
inline constexpr double netRate(Family f, int mode, CpMode cp) {
  return grossRate(f, mode, cp) * kModes[size_t(mode)].rate.ratio();
}

// AMC design floors (dB SNR, 3 kHz noise reference, CCIR Moderate), per
// family per mode; reserved modes carry a sentinel. WAVEFORM.md tables.
inline constexpr double kFloorReserved = -99.0;
using FloorTable = std::array<double, kNumModes>;
inline constexpr std::array<FloorTable, kNumFamilies> kFloors = {{
    // WIDE
    {{kFloorReserved, kFloorReserved, kFloorReserved, kFloorReserved,
      -8.5, -6.0, -3.5, -1.0, 2.0, 5.0, 8.0, 12.0, 16.0, 20.0}},
    // MID
    {{kFloorReserved, kFloorReserved, kFloorReserved, kFloorReserved,
      -12.0, -9.5, -7.0, -4.5, -1.5, 1.5, 4.5, 8.5, 12.5, 16.5}},
    // NARROW
    {{kFloorReserved, kFloorReserved, kFloorReserved, kFloorReserved,
      -15.0, -12.5, -10.0, -7.5, -4.5, -1.5, 1.5, 5.5, 9.5, 13.5}},
    // ULTRA
    {{kFloorReserved, kFloorReserved, kFloorReserved, kFloorReserved,
      -8.0, -5.5, -3.0, -0.5, 2.5, 5.5, 8.5, 12.5, 16.5, 20.5}},
}};
inline constexpr double floorDb(Family f, int mode) {
  return kFloors[size_t(f)][size_t(mode)];
}

// Sentinel: no fading conformance point exists for this mode (near-flat
// outage tail); the mode is AWGN/near-static only, gated by AMC.
inline constexpr double kFadingNA = 99.0;

// Measured FADING conformance floors (dB SNR, 3 kHz ref): lowest SNR
// with one-shot FER <= 10% on the mode's DESIGNATED channel - CCIR
// Moderate for the robust tier (M4-M7), CCIR Good for the fast tier
// (M8-M13) and for ALL NARROW modes (a 500 Hz band sees a 2-path
// channel as flat Rayleigh: no frequency diversity, so Moderate-Doppler
// operation relies on the link layer's ARQ). These sit well above the
// kFloors AWGN references because a one-shot frame shorter than the
// fade dwell faces Rayleigh OUTAGE: FER ~= P(fade < threshold), and 10%
// outage costs ~8 dB at diversity ~chi2_2-4. ARQ + soft-combining
// (Phase 2) is the design response; AMC (Phase 3) keys off these values
// plus channel-quality gates. Measured 2026-07-29, 20-40 trials/point,
// bertool ladders archived in docs/conformance/.
inline constexpr std::array<FloorTable, kNumFamilies> kFadingFloors = {{
    // WIDE (M9 > M10 is real: r3/4 QPSK is more burst-fragile than
    // r1/2 16QAM under fading; M13 = kFadingNA like NARROW's)
    {{kFloorReserved, kFloorReserved, kFloorReserved, kFloorReserved,
      1.5, 1.5, 4.5, 11.0, 10.0, 15.0, 14.0, 16.0, 26.0, kFadingNA}},
    // MID
    {{kFloorReserved, kFloorReserved, kFloorReserved, kFloorReserved,
      -4.0, -5.5, -3.0, -0.5, 8.5, 13.5, 14.5, 16.5, 24.5, 30.5}},
    // NARROW (M13 = kFadingNA: 64QAM r5/6 over a flat-Rayleigh one-shot
    // frame has a near-flat outage tail - no 10% point exists at sane
    // SNR; AMC admits it only on near-static channels)
    {{kFloorReserved, kFloorReserved, kFloorReserved, kFloorReserved,
      -7.0, -4.5, -5.0, 0.5, 11.5, 12.5, 12.5, 19.5, 23.5, 99.0}},
    // ULTRA (robust tier ~3 dB worse than WIDE: outermost carriers ride
    // the TX band-FIR transition - candidate for a wider FIR margin.
    // M6/M7 provisional: slow outage tails, see ROADMAP 1.13 known
    // issues)
    {{kFloorReserved, kFloorReserved, kFloorReserved, kFloorReserved,
      4.0, 6.5, 16.0, 12.5, 8.5, 13.5, 14.5, 18.5, 24.5, 28.5}},
}};
inline constexpr double fadingFloorDb(Family f, int mode) {
  return kFadingFloors[size_t(f)][size_t(mode)];
}

// --------------------------------------------------------------------- LDPC
inline constexpr int kLdpcNShort = 648;    // control frames, PHY header
inline constexpr int kLdpcNLong  = 1944;   // data frames

// ----------------------------------------------------------------- PHY header
inline constexpr uint8_t kPhyMagic       = 0x5C;
inline constexpr uint8_t kWireVersion    = 1;
inline constexpr int     kHeaderSpreadFast   = 2;
inline constexpr int     kHeaderSpreadRobust = 16;
// Header codeword info capacity (N=648 r1/2) minus header bits ~= piggyback.
inline constexpr int     kHeaderPiggybackBytes = 30;

// ---------------------------------------------------------------------- PRN
// Fixed seeds shared TX/RX (DSP.md: one constants header, deterministic).
inline constexpr uint32_t kSeedWhitener     = 0x1D2C5A5Bu;
inline constexpr uint32_t kSeedSpreadPhase  = 0x5EA5CAFEu;
inline constexpr uint32_t kSeedPilotPhase   = 0x0FD1E5EDu;

// ------------------------------------------------------------------ TX levels
inline constexpr double kTxTargetRms       = 0.22;
inline constexpr double kTxScaleMin        = 0.30;
inline constexpr double kTxScaleMax        = 1.15;
inline constexpr double kTxPeakPreHeadroom = 1.90;
inline constexpr double kTxFinalHeadroom   = 0.50;
inline constexpr double kTxPaprCeilingDb   = 8.0;

// ----------------------------------------------------------------- link timers
inline constexpr double kHandshakeTurnaroundSec = 6.0;
inline constexpr double kHandshakeTimeoutMinSec = 12.0;
inline constexpr double kHandshakeTimeoutMaxSec = 45.0;
inline constexpr int    kHandshakeAttempts      = 4;
inline constexpr double kAckTimeoutMinSec       = 4.0;
inline constexpr double kAckTimeoutMaxSec       = 45.0;
inline constexpr double kIdleDisconnectSec      = 120.0;
inline constexpr double kKeepaliveSec           = 20.0;

// --------------------------------------------------------------- TCP defaults
inline constexpr uint16_t kPortData  = 9100;
inline constexpr uint16_t kPortCtrl  = 9101;
inline constexpr uint16_t kPortVoice = 9102;

// -------------------------------------------------------------------- safety
inline constexpr double kTxWatchdogDefaultSec = 120.0;
inline constexpr double kTxTestMaxSec         = 30.0;

}  // namespace skw
