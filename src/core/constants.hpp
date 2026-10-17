// Skywave band-profile constants.
//
// Design rule (ARCHITECTURE.md): every numerology value lives HERE or in the
// mode tables below - never as a literal in logic. The HF profile is the only
// v1 profile; a future VHF-FM profile is a second BandProfile instance.
#pragma once

#include <cstdlib>

#include <array>
#include <cstdint>

namespace skw {

// Band profiles (WAVEFORM.md): `hf` (v1) and `vhf-fm` (Phase 7) share
// every numerology value below - sample domain, OFDM grid, CP set,
// preamble geometry - and differ only in the PROFILE TABLE at the end
// of this header (family plans, center bin, floor tables, capability
// gates). The active profile is selected once at startup.
enum class Band : uint8_t { Hf = 0, VhfFm = 1 };

// ---------------------------------------------------------------- sample domain
inline constexpr int    kFsAudio        = 48000;  // external real passband rate
inline constexpr int    kFsBaseband     = 12000;  // internal complex rate
inline constexpr int    kDecimation     = 4;      // 48k -> 12k

// ---------------------------------------------------------------------- OFDM grid
inline constexpr int    kFftAudio       = 1280;   // at 48 kHz
inline constexpr int    kFftBaseband    = kFftAudio / kDecimation;  // 320
inline constexpr double kSubcarrierHz   = double(kFsAudio) / kFftAudio; // 37.5
static_assert(kFftBaseband == 320);
// HF waveform center: bin 40 = 1500 Hz (the profile table holds it; the
// named constant remains for HF-pinned static_asserts only).
inline constexpr int kHfCenterBin = 40;

enum class CpMode : uint8_t { Short = 0, Medium = 1, Long = 2 };
// CP length sets in audio-rate samples (divide by kDecimation for
// baseband); PER BAND PROFILE - HF spans 2/4/6 ms for millisecond delay
// spreads, VHF-FM 0.67/2/4 ms (its multipath is microseconds; the CP is
// sync-slack, and the tighter SHORT buys ~5% symbol rate at the top).
inline constexpr std::array<int, 3> kHfCpSamplesAudio = {96, 192, 288};
inline constexpr std::array<int, 3> kVhfCpSamplesAudio = {32, 96, 192};
inline constexpr CpMode kCpDefault = CpMode::Medium;

// Profile accessors defined after the BandProfile block below.
inline int cpSamplesAudio(CpMode cp);
inline double symbolRate(CpMode cp);

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
// VHF-FM profile (WAVEFORM.md, Phase 7): ONE wide family on the flat
// 9600-capable data-port path - 176 carriers, ~300-6900 Hz, centered
// 3.6 kHz. Every Family index resolves to the same plan (the 2-bit wire
// family field stays valid; bands never interoperate on one channel).
inline constexpr std::array<FamilyPlan, kNumFamilies> kVhfFamilies = {{
    {"VHF", 8, 183, 400.0, 6800.0, false},
    {"VHF", 8, 183, 400.0, 6800.0, false},
    {"VHF", 8, 183, 400.0, 6800.0, false},
    {"VHF", 8, 183, 400.0, 6800.0, false},
}};
// Carrier-count invariants from WAVEFORM.md (pinned to the HF table).
static_assert(kFamilies[0].carriers() == 64);
static_assert(kFamilies[1].carriers() == 27);
static_assert(kFamilies[2].carriers() == 13);
static_assert(kFamilies[3].carriers() == 72);
static_assert(kVhfFamilies[0].carriers() == 176);
// Odd-count HF families center exactly on bin 40 (1500 Hz).
static_assert((kFamilies[1].firstBin + kFamilies[1].lastBin) / 2 ==
              kHfCenterBin);
static_assert((kFamilies[2].firstBin + kFamilies[2].lastBin) / 2 ==
              kHfCenterBin);

// --------------------------------------------------------------------- pilots
inline constexpr int    kPilotSpacing   = 8;    // 1-in-8 carriers per symbol
inline constexpr double kPilotBoostDb   = 2.0;

// ------------------------------------------------------------------- preamble
inline constexpr int    kChirpRepsFast      = 2;  // up-up
inline constexpr int    kChirpRepsRobust    = 4;  // up-down-up-down
inline constexpr int    kChirpRepsCalling   = 4;  // up-up-up-up
inline constexpr int    kZcSymbolsFast      = 3;
inline constexpr int    kZcSymbolsRobust    = 6;
// CFO capture is +-55 Hz for every signature (chirp.cpp
// chirpCfoCaptureMaxHz; the receiver resolves the fine estimate's branch,
// GAP_CLOSURE_PLAN.md R1). The former kCfoCapture*/kCfoSearchStepHz
// constants were never read by any code path.

// ------------------------------------------------------------------ mode ladder
enum class Waveform : uint8_t { Ofdm = 0, Css = 1 /* reserved */ };
enum class Constellation : uint8_t { Qpsk = 0, Qam16 = 1, Qam64 = 2, Qam256 = 3 };
inline constexpr int bitsPerSymbol(Constellation c) {
  switch (c) {
    case Constellation::Qpsk: return 2;
    case Constellation::Qam16: return 4;
    case Constellation::Qam64: return 6;
    case Constellation::Qam256: return 8;
  }
  return 2;
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

inline constexpr int kNumModes  = 15;   // M0..M14 (M14: VHF-FM 256QAM)
inline constexpr int kFirstMode = 4;    // lowest active mode in v1
inline constexpr std::array<ModeDef, kNumModes> kModes = {{
    // M0-M3: CSS deep tier. `spread` holds the spreading factor SF
    // (2^SF chips per symbol); M0 is the deepest; payload transport is
    // LDPC r1/2 over soft-demodulated CSS symbols. Active as of 6.2;
    // session use is still gated on CAP_CSS_TIER negotiation.
    {Waveform::Css, Constellation::Qpsk, {1, 2}, 8, true},  // M0 SF8
    {Waveform::Css, Constellation::Qpsk, {1, 2}, 7, true},  // M1 SF7
    {Waveform::Css, Constellation::Qpsk, {1, 2}, 6, true},  // M2 SF6
    {Waveform::Css, Constellation::Qpsk, {1, 2}, 5, true},  // M3 SF5
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
    // M14: 256QAM r5/6 - the VHF-FM top rung (WAVEFORM.md Phase 7).
    // Active in the table; unreachable on HF (floor sentinels below)
    // and above kInitialModeCap, so only deliberate AMC promotion on a
    // rich flat channel ever lands here.
    {Waveform::Ofdm, Constellation::Qam256, {5, 6}, 1, true},  // M14
}};

// Average data carriers per symbol = carriers * (spacing-1)/spacing.
inline double avgDataCarriers(Family f);
// Gross (uncoded) waveform bit rate for a mode, bits/s, at a given CP.
inline double grossRate(Family f, int mode, CpMode cp);
// Net (post-FEC) payload rate, bits/s.
inline double netRate(Family f, int mode, CpMode cp);

// AMC design floors (dB SNR, 3 kHz noise reference), per family per
// mode. M4-M13 per WAVEFORM.md tables; M0-M3 are the measured CSS AWGN
// floors (WAVEFORM.md "CSS Deep Tier", 2026-07-29).
inline constexpr double kFloorReserved = -99.0;
using FloorTable = std::array<double, kNumModes>;
// AWGN reference floors: lowest SNR (3 kHz ref) one ~dB below the
// mode's 10% one-shot FER point (conformance tests floor+1 <= 10%).
// OFDM M4-M13 RE-MEASURED 2026-10 for v1.2.0 (tools/floor_campaign.sh:
// 200 trials per 0.5 dB, floor = SNR(10%) - 1 rounded up to 0.5;
// archived docs/conformance/floors_v1.2/). The receiver gained 1.5-3 dB
// across every family (RTS smoothing and Q normalisation at deep SNR,
// branch-resolved CFO; GAP_CLOSURE_PLAN.md). CSS M0-M3 unchanged (the
// CSS path did not change). The previous per-entry history (e.g. NARROW
// M9's 2026-09-07 re-measure) is in git and the ROADMAP.
// NARROW CSS M0/M1 RAISED 2026-10-12 (v1.2.1) to FULL-FRAME floors: the
// payload reaches -19.5/-17.5 (bertool --css) but the chirp preamble,
// sync and header do not follow it - full frames (bertool --css-frames,
// AWGN, 40-60 trials) run M0 30% at -16.5, 10% at -15.5, 0% at -14.5 and
// M1 23% at -16.5, 12% at -15.5, 5% at -14.5. The AMC steers on these
// entries, so they now describe what a frame achieves; acquisition, not
// the payload, bounds NARROW's deepest modes (ROADMAP).
// NARROW CSS M0/M1 LOWERED 2026-10-14 to -18.5/-17.5 with the 12-rep CSS
// preamble and the sync CFO-branch check (full frames, AWGN, 100 trials
// per 0.5 dB): M0 12% at -18.5, 7% at -18.0, 3% at -17.5; M1 13% at
// -17.5, 5% at -17.0, 1% at -16.5 (now payload-limited). Same rule as
// cssFloorDb (lowest SNR with FER <= ~15%, rounded up to 0.5 dB).
// MID CSS M0 LOWERED -16.5 -> -17.5 with the same 12-rep preamble and the
// CSS CFO-branch retry (100 trials: 12% at -17.5, 6-7% at -17/-16.5).
// ALL CSS M0-M3 are FULL-FRAME floors since 2026-10-14 (bertool
// --css-frames, AWGN, 100 trials per 0.5 dB; floor = lowest point with
// FER <= 15% and every point above it too): WIDE -14.5/-12.0/-10.0/-7.5,
// MID -17.5/-16.0/-14.0/-11.5, NARROW -18.5/-17.5/-17.0/-15.0, ULTRA
// -14.0/-12.0/-10.0/-7.5 (M0/M1 one step up: exactly 15% at -14.5/
// -12.5, and the 40-trial gate seed read 17.5/20%) - 1.5-3 dB below the 2026-07-29 values, which
// were measured on the legacy Hamming payload layer (`bertool --css`,
// since removed with that layer; frames carry LDPC). Curves archived in
// docs/conformance/css_frames_floors_2026-10-14/.
// Then the CSS branch retry was fixed (it had fought the sync branch
// check and covered only +-1 branch) and every floor re-measured (100
// trials per 0.5 dB): WIDE M1 -12.0 -> -12.5, NARROW M1 -17.5 -> -18.0
// (it was branch errors, not the payload, that limited it), ULTRA M1
// -12.0 -> -12.5, NARROW M3 -15.0 -> -14.5 (raised: 19% at -15.0 here,
// 12% before - too close to the line).
// M5/M6 on MID/NARROW/ULTRA RE-MEASURED 2026-10-14 for the leftover-cell
// copies (layout.hpp; floor_campaign subset, 200/100 trials, same
// derivation): AWGN MID M6 -10.0 -> -10.5, NARROW M5/M6 -16.0/-13.0 ->
// -16.5/-13.5, ULTRA M5/M6 -8.5/-5.5 -> -9.0/-6.0; fading tables likewise
// 0.5-1.5 dB lower (NARROW M6 Moderate -6.5 -> -8.0). M7 (<= 5.5%
// leftover) measured unchanged with the copies on/off and keeps its
// values. Curves: docs/conformance/leftover_m5m7_2026-10-14/.
inline constexpr std::array<FloorTable, kNumFamilies> kFloors = {{
    // WIDE
    {{-14.5, -12.5, -10.0, -7.5,
      -11.0, -8.5, -6.0, -3.0, 0.5, 3.0, 5.5, 9.5, 13.5, 17.0, 99.0}},
    // MID
    {{-17.5, -16.0, -14.0, -11.5,
      -15.0, -13.0, -10.5, -7.5, -3.5, -0.5, 2.0, 6.0, 9.5, 13.5, 99.0}},
    // NARROW
    {{-18.5, -18.0, -17.0, -14.5,
      -17.5, -16.5, -13.5, -10.5, -6.5, -3.5, -1.0, 2.5, 6.5, 10.5, 99.0}},
    // ULTRA
    {{-14.0, -12.5, -10.0, -7.5,
      -11.0, -9.0, -6.0, -3.0, 1.0, 3.5, 6.0, 10.0, 14.0, 17.5, 99.0}},
}};
inline double floorDb(Family f, int mode);  // reads the active profile

// Sentinel: no fading conformance point exists for this mode (near-flat
// outage tail); the mode is AWGN/near-static only, gated by AMC.
inline constexpr double kFadingNA = 99.0;

// Measured FADING conformance floors (dB SNR, 3 kHz ref): lowest SNR
// with one-shot FER <= ~10-15% on the mode's DESIGNATED channel - CCIR
// Moderate for the robust tier (M4-M7), CCIR Good for the fast tier
// (M8-M13) and for ALL NARROW modes (a 500 Hz band sees a 2-path
// channel as flat Rayleigh: no frequency diversity, so Moderate-Doppler
// operation relies on the link layer's ARQ). These sit well above the
// kFloors AWGN references because a one-shot frame shorter than the
// fade dwell faces Rayleigh OUTAGE: FER ~= P(fade < threshold), and 10%
// outage costs ~8 dB at diversity ~chi2_2-4. ARQ + soft-combining
// (Phase 2) is the design response; AMC (Phase 3) keys off these values
// plus channel-quality gates. First measured 2026-07-29 (20-40 trials).
// RE-MEASURED 2026-10 for v1.2.0 (tools/floor_campaign.sh, 100 trials
// per 1.5 dB, floor = SNR(15%) rounded up to 0.5; floors_v1.2/): 2-7 dB
// lower across the ladder, and WIDE/ULTRA/MID M13 now have a fading
// floor. NARROW M6/M7 sit higher on Good than on Moderate: a 20-40 s
// NARROW frame averages Moderate's faster fades but sits inside a Good
// fade.
// CSS M0-M3 (all three fading tables) RE-MEASURED 2026-10-14 on full
// frames (bertool --css-frames --base awgn, 100 trials per 1.5 dB, same
// derivation via tools/floor_tables.py): 1-6 dB lower than the 2026-07
// values, WIDE/ULTRA M2/M3 gain a fading floor, and the Poor table -
// which had only copied the designated CSS values - holds what CSS does
// on Poor (WIDE/ULTRA M2/M3 never close there; M0/M1 only after the RAKE
// tap span fix, receiver.cpp). Archived
// docs/conformance/css_fading_floors_2026-10-14/ - RE-RUN after the CSS
// branch-retry fix (same campaign): about ten entries moved by 0.5-3 dB,
// mostly down (Poor WIDE M1 2.0 -> -1.0, ULTRA M1 4.0 -> 1.5).
inline constexpr std::array<FloorTable, kNumFamilies> kFadingFloors = {{
    // WIDE
    {{-8.5, -5.5, -4.0, -1.5,
      -5.5, -3.0, -0.5, 4.0, 7.5, 11.0, 13.0, 17.5, 21.0, 25.0, kFadingNA}},
    // MID
    {{-11.0, -10.0, -8.5, -6.5,
      -9.0, -8.0, -5.5, -2.5, 6.0, 10.5, 10.5, 14.5, 17.5, 23.0, kFadingNA}},
    // NARROW (all modes on CCIR Good - see above). M5 RAISED -8.5 -> -8.0
    // for v1.2.1: 400 trials gave 15.2% at the old floor+1 (the 15% gate
    // passed on slack alone) and 12.2% at -7.0; the fixed-seed 40-trial
    // release sweep failed the point identically at v1.2.0 and v1.2.1.
    {{-11.5, -11.0, -10.0, -7.5,
      -10.5, -8.0, -5.5, -3.0, 5.0, 8.0, 10.0, 13.5, 16.5, 21.5, kFadingNA}},
    // ULTRA
    {{-7.5, -6.5, -4.0, -1.5,
      -5.0, -3.0, 0.0, 2.5, 7.5, 11.5, 13.0, 18.0, 21.5, 26.0, kFadingNA}},
}};

// Per-channel-class fading floors (v1.2.0, GAP_CLOSURE_PLAN.md 8.9): the
// AMC uses the table of the channel the classifier sees - MODERATE ->
// every mode measured on CCIR Moderate, SEVERE -> on CCIR Poor - instead
// of capping the ladder (the v1.1.0 caps, M8 on Moderate and M7 on
// Severe, encoded "the fast tier fails there at any SNR", no longer
// true: Moderate WIDE M11 closes at 22 dB, Poor WIDE M10 at 22.5).
// Same definition as kFadingFloors (FER <= 15%, 100 trials per 1.5 dB,
// rounded up to 0.5), then moved up until the campaign curve meets
// bertool's gate levels at floor+1 / floor+3 WITHOUT the binomial slack
// (tools/floor_tables.py; independent fresh-seed sweeps keep the slack).
// M10 on Poor sits on a 13-25% outage plateau and has no floor: WIDE's
// first 15% crossing was a lucky dip at 23 dB that failed conformance.
// One manual override: Moderate NARROW M13 (derived 36.0) is kFadingNA -
// FER plateaus at 10-23% from 27 to 46 dB (64QAM r5/6 on a flat-Rayleigh
// 500 Hz band), so it failed a fresh-seed sweep at floor+3 (23%). kFadingNA here means "never closes on this
// channel" and makes the mode INELIGIBLE in that class. CSS M0-M3 were
// measured on each channel too (2026-10-14; see kFadingFloors).
inline constexpr std::array<FloorTable, kNumFamilies> kModerateFloors = {{
    // WIDE
    {{-8.5, -5.5, -4.0, -1.5,
      -5.5, -3.0, -0.5, 4.0, 10.0, 13.5, 14.0, 19.0, 22.0, 28.5, kFadingNA}},
    // MID
    {{-11.0, -10.0, -8.5, -6.5,
      -9.0, -8.0, -5.5, -2.5, 4.5, 7.5, 10.5, 14.5, 17.0, 24.5, kFadingNA}},
    // NARROW
    {{-14.0, -13.5, -11.5, -9.0,
      -10.0, -10.0, -8.0, -5.0, 3.5, 6.5, 8.5, 13.0, 16.0, kFadingNA, kFadingNA}},
    // ULTRA
    {{-7.5, -6.5, -4.0, -1.5,
      -5.0, -3.0, 0.0, 2.5, 8.5, 12.5, 15.0, 19.0, 22.5, 26.5, kFadingNA}},
}};
inline constexpr std::array<FloorTable, kNumFamilies> kPoorFloors = {{
    // WIDE
    {{-3.5, -1.0, kFadingNA, kFadingNA,
      -4.0, -2.5, 0.5, 2.0, 11.5, 19.0, kFadingNA, kFadingNA, kFadingNA, kFadingNA, kFadingNA}},
    // MID
    {{-9.0, -5.0, -5.0, 2.5,
      -8.0, -7.0, -4.5, 0.0, kFadingNA, kFadingNA, kFadingNA, kFadingNA, kFadingNA, kFadingNA, kFadingNA}},
    // NARROW
    {{-13.0, -12.0, -9.5, -6.0,
      -10.0, -9.5, -8.0, -5.0, 10.5, kFadingNA, kFadingNA, kFadingNA, kFadingNA, kFadingNA, kFadingNA}},
    // ULTRA
    {{-3.0, 1.5, kFadingNA, kFadingNA,
      -4.0, -2.5, 0.5, 3.5, 12.5, 20.0, kFadingNA, kFadingNA, kFadingNA, kFadingNA, kFadingNA}},
}};
inline double fadingFloorDb(Family f, int mode);  // active profile

// OFDM SNR CALIBRATION (HF). The floor tables above are in the 3 kHz-
// noise-bandwidth convention (bertool's set point, HF practice, and the
// CSS path's snr3k), but the OFDM tracker reports mean per-carrier SNR
// inside the family's occupied band, which sits ABOVE that reference by
// the bandwidth term 10log10(3000 / occupied Hz) plus a ~3 dB
// pilot-fit bias (the Kalman residual absorbs part of the noise).
// MEASURED 2026-09-06 with `bertool --preset awgn` (estimate minus set
// point, M4-M10, 12 trials per point):
//     WIDE   +2.8..+4.6   (2400 Hz: +1.0 bandwidth, ~+3 fit)
//     MID    +7.0..+9.9   (1000 Hz: +4.8 bandwidth, ~+4 fit)
//     NARROW +9.2..+12.7  ( 450 Hz: +8.2 bandwidth, ~+3 fit)
//     ULTRA  +2.1..+5.5   (2650 Hz: +0.5 bandwidth, ~+3 fit)
// The offset grows ~2 dB across the top of each ladder (the residual
// floor), so the constants are set for the floor region, where the AMC
// decides. Uncorrected, the AMC promoted ~4 dB early on WIDE relative
// to the floors it compares against - measured on the churn soak as
// M11 at "22-24 dB" (really ~18-20, floor+2..+4, where bertool shows
// 10-23% FER on CCIR Good) retrying 25-38% and cascading down five
// rungs. Under CCIR Moderate the estimate additionally UNDER-reads by
// 4-6 dB at M8 (channel-motion innovation counted as noise); that is
// left as is - it keeps the fast tier out of a channel its floors were
// never measured on (WAVEFORM.md fading annex).
// RE-MEASURED 2026-10 on the v1.2.0 receiver (floor campaign, AWGN,
// points 0-3 dB above each M4-M10 10% point): with the old constants the
// estimate read LOW by a median 0.6 (WIDE/NARROW/ULTRA) and 0.8 (MID) dB.
// The constants moved down by that much, rounded so the estimate never
// over-reads (an over-read promotes early; an under-read only late).
inline constexpr std::array<double, kNumFamilies> kOfdmSnrCalDb = {
    {3.5, 7.5, 10.5, 3.0}};
// VHF-FM: its floor tables were calibrated against the uncorrected
// estimator (bertool --band vhf-fm), so no offset there.
inline double ofdmSnrCalDb(Family f);

// VHF-FM floor tables: the data-port channel is flat and near-static
// (Rician K -> inf approximation), so the fading table IS the AWGN
// table. Values scale from the WIDE ladder by the wider noise
// collection (+10log10(6600/2400) ~ +4.4 dB against the 3 kHz
// reference); calibrated with bertool --band vhf-fm (7.2/7.3). M0-M3
// reserved: the CSS deep tier is HF-only.
inline constexpr FloorTable kVhfFloorRow = {
    kFloorReserved, kFloorReserved, kFloorReserved, kFloorReserved,
    -4.0, -1.5, 1.0, 3.5, 6.5, 9.5, 12.5, 16.5, 20.5, 24.5, 33.0};
inline constexpr std::array<FloorTable, kNumFamilies> kVhfFloors = {
    {kVhfFloorRow, kVhfFloorRow, kVhfFloorRow, kVhfFloorRow}};

// ------------------------------------------------------------- band profiles
// The active profile is set ONCE at startup (before any DSP object is
// constructed) from `[modem] band`; everything numerology-shaped that
// differs between bands routes through it.
struct BandProfile {
  const char* name;
  const std::array<FamilyPlan, kNumFamilies>* families;
  int centerBin;  // audio-grid bin the converters shift to DC
  const std::array<FloorTable, kNumFamilies>* floors;        // AWGN
  const std::array<FloorTable, kNumFamilies>* fadingFloors;  // designated
  const std::array<FloorTable, kNumFamilies>* moderateFloors;  // CCIR Moderate
  const std::array<FloorTable, kNumFamilies>* poorFloors;      // CCIR Poor
  bool cssTier;   // CSS deep tier available on this band
  bool masking;   // carrier masking available (wire bitmap: <= 72 carriers)
  // Chirp rep duration: HF needs 160 ms of integration to detect at the
  // deep floors; the SNR-rich VHF path uses 40 ms reps, cutting the
  // per-frame preamble overhead 4x (throughput lives on it at M13/M14).
  double chirpRepSec;
  CpMode cpDefault;  // SHORT on the microsecond-spread VHF channel
  const std::array<int, 3>* cpSamplesAudio;
  int zcSymbolsFast;    // sync-block lengths (SNR-rich VHF trims both)
  int zcSymbolsRobust;
  // TX PAPR soft-clip ceiling over frame RMS. 8 dB protects HF audio
  // chains and is EVM-safe through 64QAM; 256QAM (M14) needs ~11 dB or
  // content-dependent clip noise tips whole frames over - and every
  // identical retransmission with them.
  double txPaprCeilingDb;
  // Fast-header spreading: HF needs x2 (decode to ~-1 dB); the VHF fast
  // tier starts at +6.5 dB, so x1 halves the header airtime safely.
  int headerSpreadFast;
  // Multipath margin before the earliest significant ZC tap (baseband
  // samples), and the tracker's delay-grid taps ahead of the FFT window.
  // HF: an early path that is weak at ZC time (fading) re-emerges up to
  // the full delay spread early; 16 samples (2/3 of the 2 ms Short CP)
  // and 5 grid taps keep it inside the window and on the grid
  // (GAP_CLOSURE_PLAN.md R4). VHF's microsecond spreads keep the old 2/2
  // (its 0.67 ms Short CP is 8 samples).
  int zcWindowBackoff;
  int trackerPreTaps;
  // Passband RMS of a burst at output_gain_db = 0. HF's 8 dB ceiling
  // upconverts to peaks near -1 dBFS at kTxTargetRms; VHF's 11 dB ceiling
  // (for 256QAM) put up to 1 frame in 5 over full scale there (worst peak
  // 1.21), hard-clipped by the worker, so VHF runs ~2.3 dB lower.
  double txTargetRms;
  // Chirp detector correlation grid (baseband samples). The correlation
  // peak is ~1/B wide: HF's ~2.4 kHz sweeps tolerate a 4-sample grid
  // (worst-case -2.4 dB), but VHF's ~6.6 kHz sweep put the peak between
  // grid points at some sample phases and frames were missed outright
  // (FM radio sweep 2026-10-17). VHF's reps are 4x shorter, so a 1-sample
  // grid costs the same as HF's.
  int chirpGridStep;
};
inline constexpr BandProfile kHfProfile = {
    "hf",     &kFamilies,    kHfCenterBin,    &kFloors,   &kFadingFloors,
    &kModerateFloors, &kPoorFloors,
    true,     true,          0.160,           CpMode::Medium,
    &kHfCpSamplesAudio,      3,               6,             8.0,  2,
    16,       5,             0.22,        4};
inline constexpr BandProfile kVhfFmProfile = {
    "vhf-fm", &kVhfFamilies, 96 /* 3600 Hz */, &kVhfFloors, &kVhfFloors,
    &kVhfFloors, &kVhfFloors,
    false,    false,         0.040,           CpMode::Short,
    &kVhfCpSamplesAudio,     2,               4,             11.0, 1,
    2,        2,             0.17,        1};

inline const BandProfile*& activeProfileSlot() {
  static const BandProfile* p = &kHfProfile;
  return p;
}
inline const BandProfile& activeProfile() { return *activeProfileSlot(); }
inline void setActiveBand(Band b) {
  activeProfileSlot() = b == Band::VhfFm ? &kVhfFmProfile : &kHfProfile;
}
inline Band activeBand() {
  return activeProfileSlot() == &kVhfFmProfile ? Band::VhfFm : Band::Hf;
}

// R4 multipath margins (zcWindowBackoff / trackerPreTaps), or the v1.1.0
// values 2/2 with SKW_MULTIPATH_MARGIN=0 for A/B measurement.
inline bool multipathMarginEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("SKW_MULTIPATH_MARGIN");
    return !(e && std::atoi(e) == 0);
  }();
  return on;
}
inline int zcWindowBackoff() {
  return multipathMarginEnabled() ? activeProfile().zcWindowBackoff : 2;
}
inline int trackerPreTaps() {
  return multipathMarginEnabled() ? activeProfile().trackerPreTaps : 2;
}

inline const FamilyPlan& plan(Family f) {
  return (*activeProfile().families)[size_t(f)];
}
inline int cpSamplesAudio(CpMode cp) {
  return (*activeProfile().cpSamplesAudio)[size_t(cp)];
}
inline double symbolRate(CpMode cp) {
  return double(kFsAudio) / (kFftAudio + cpSamplesAudio(cp));
}
inline int zcSymbols(bool robust) {
  return robust ? activeProfile().zcSymbolsRobust
                : activeProfile().zcSymbolsFast;
}
inline int centerBin() { return activeProfile().centerBin; }
inline double centerHz() { return centerBin() * kSubcarrierHz; }
inline double floorDb(Family f, int mode) {
  return (*activeProfile().floors)[size_t(f)][size_t(mode)];
}
inline double fadingFloorDb(Family f, int mode) {
  return (*activeProfile().fadingFloors)[size_t(f)][size_t(mode)];
}
inline double moderateFloorDb(Family f, int mode) {
  return (*activeProfile().moderateFloors)[size_t(f)][size_t(mode)];
}
inline double poorFloorDb(Family f, int mode) {
  return (*activeProfile().poorFloors)[size_t(f)][size_t(mode)];
}
inline double ofdmSnrCalDb(Family f) {
  return activeBand() == Band::Hf ? kOfdmSnrCalDb[size_t(f)] : 0.0;
}
inline double avgDataCarriers(Family f) {
  return plan(f).carriers() * double(kPilotSpacing - 1) / kPilotSpacing;
}
inline double grossRate(Family f, int mode, CpMode cp) {
  const ModeDef& m = kModes[size_t(mode)];
  return avgDataCarriers(f) * bitsPerSymbol(m.constellation) *
         symbolRate(cp) / m.spread;
}
inline double netRate(Family f, int mode, CpMode cp) {
  return grossRate(f, mode, cp) * kModes[size_t(mode)].rate.ratio();
}

// PHY frame_type value marking a MAC burst container (PROTOCOL.md "MAC
// Burst Profiles"): the PHY payload holds multiple link frames packed as
// [u16le length][frame bytes] entries, parsed greedily. Containers are
// excluded from HARQ chase (retransmissions repack, so the frame-identity
// key cannot guarantee identical codewords).
inline constexpr uint8_t kBurstContainerFrameType = 62;

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
// HF passband RMS of a data burst at output_gain_db = 0 (-13.2 dBFS); the
// active band's level is activeProfile().txTargetRms. The PHY
// emits unit-RMS baseband with an 8 dB PAPR allowance; its real upconverted
// form has ~12 dB crest, so 0.22 RMS peaks near -1 dBFS. (Until 2026-10
// this was defined but unused: bursts went out at ~0.7 RMS, +9 dBFS peaks,
// which real interfaces clip and virtual cables pass untouched.)
inline constexpr double kTxTargetRms       = 0.22;
inline constexpr double kTxScaleMin        = 0.30;
inline constexpr double kTxScaleMax        = 1.15;
inline constexpr double kTxPeakPreHeadroom = 1.90;
inline constexpr double kTxFinalHeadroom   = 0.50;

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
// Companion-app PTT server (INTERFACES.md `PTTMETHOD TCP`) when `PTTPORT`
// names a host without an explicit port. Matches Hamlib's rigctld default,
// which is what most companion apps front.
inline constexpr uint16_t kPttTcpDefaultPort = 4532;

// -------------------------------------------------------------------- safety
inline constexpr double kTxWatchdogDefaultSec = 120.0;
inline constexpr double kTxTestMaxSec         = 30.0;

// ------------------------------------------------- half-duplex MAC timing
// Ceiling on the airtime scale that derives every handoff constant from
// the session mode's frame airtime (LinkEngine::airScale_).
//
// The purpose of the scale is that a courtesy gap or a busy-hold must
// outlast ONE peer burst - not many. At 24 the deep-tier control hold
// became 720 s, long enough to look like a stall (test_st2 ran 10 min
// against its usual 2). 6x covers a 35-55 s CSS frame against a robust
// OFDM reference of a few seconds with margin to spare.
inline constexpr double kAirtimeScaleMax = 6.0;

// Deep-tier (CSS) adaptive frame length, in LDPC codewords per frame.
// The floor is the fade-safe shape the tier shipped with; the ceiling is
// bounded by the 4095-byte PHY length field and by how much airtime one
// unacknowledged frame may risk.
inline constexpr int kCssCwMin = 2;
inline constexpr int kCssCwMax = 16;
inline constexpr int kCssGrowRun = 2;   // clean deliveries before growing

// OFDM ROBUST TIER (M4-M7) frame length, same trade as the deep tier and
// the same adaptive answer. MEASURED 2026-09-07 with `bertool --payload`
// at the WIDE fading floors (goodput = payload x (1-FER) / airtime, the
// expected rate under ARQ where a failed frame costs its whole airtime):
//     CCIR Moderate   140 B    280 B    560 B   1120 B
//       M5             16.7     19.9     22.0     26.4
//       M6             20.9     26.9     33.1     42.1
//       M7             23.5     34.4     40.6     51.0
//     CCIR Good        M7 26.9 -> 42.3 -> 59.1
// Goodput rises monotonically because overhead falls (M7 79% -> 30% of
// airtime) faster than outage FER rises. One codeword was never a
// measured choice - it is the fast tier's rule applied downward, where
// preamble + sync + header cost the same seconds but the payload is a
// fraction of the bytes. (The 2026-09-05 "frame-length adaptation NOT
// adopted" verdict was measured at M8/M9, which already carry 2
// codewords per frame and sit at 30-44% overhead.)
inline constexpr int kRobustCwMin = 1;
inline constexpr int kRobustCwMax = 8;
// Airtime bound on a robust-tier frame. Longer frames keep paying in the
// bertool sweep, but a frame is also the unit of retransmission, of the
// ARQ timer, of a turn's minimum hold, and of the AMC's evidence - and
// the sweep cannot see any of those. That last one binds first: the
// AMC's retry window is 16 FRAMES, so quadrupling frame length quarters
// the sample rate a ramp is tracked with. MEASURED against the M3 ramp
// gate (>= 0.75 of a genie oracle) and the churn seed spread:
//     cap    ramp   seed 11   seed 12   seed 13   coupled
//     off    0.98    32,336    65,662    89,473   196,667
//     6 s    0.90         -         -         -         -
//     10 s   0.87    37,006    73,028    89,473   196,667
//     14 s   0.75         -         -         -         -
//     20 s   0.75    37,006    63,043    89,473   196,667
// 10 s takes the whole throughput win (+14% and +11% on the two seeds
// that spend time in the robust tier) and keeps ramp margin; beyond it
// the ramp collapses to the gate and seed 12 regresses. The coupled
// schedule is identical in every cell - it never leaves the fast tier.
inline constexpr double kRobustFrameAirCapSec = 10.0;

// Target airtime of one coalesced proto message (engine applySealCap),
// 0 = no airtime bound. Proto assembly is head-of-line, so a message's
// airtime looked like the app-visible delivery interval - and it is not
// the binding term. MEASURED 2026-09-07 (SKW_DELIV_DEBUG, churn):
// delivery gaps 89-132 s median at M6-M9, while the median ISS turn is
// 42 s, i.e. an ~85 s turn CYCLE. A station can only deliver what
// arrived while the peer held the turn, so the cycle floors the gap and
// shrinking messages under it buys nothing:
//              coupled    seed 11   seed 12   seed 13
//   off       196,667     37,006    63,344    89,473
//   45 s      196,667     37,006    61,099    89,473
//   25 s      196,667     29,671    63,344    89,473
// Left at 0. (An earlier version divided a burst-container's airtime
// into a single frame's budget, capping framesPerBurst times too hard;
// that bit the fast tier it was meant to leave alone - coupled 196.7 ->
// 120.7 KB - while lifting the robust-tier seeds 12-37%. Tempting, and
// the wrong reason: it was a bug, not a bound.) SKW_DELIVER_TARGET_SEC
// re-enables it for measurement.
inline constexpr double kDeliverTargetSec = 0.0;

// Anti-starvation backstop: how many ARQ timeouts queued DATA may wait
// behind a busy channel before it airs regardless of the busy verdict.
// On a link where BOTH ends have traffic the channel is busy essentially
// all the time - each end hears the other - so this backstop, not the
// CCA gate, is what actually paces data. At 3x it delivered a trickle
// (~940 B per 60-100 s in the ST2 soaks, every scenario stalling).
// Tunable for measurement: SKW_DATA_STARVE_X.
inline double dataStarveTimeouts() {
  static const double v = [] {
    const char* e = std::getenv("SKW_DATA_STARVE_X");
    const double d = e ? std::atof(e) : 3.0;
    return d > 0.0 ? d : 3.0;
  }();
  return v;
}

}  // namespace skw
