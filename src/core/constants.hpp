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
inline constexpr double kCfoCaptureConnHz   = 5.0;   // connected preambles
inline constexpr double kCfoCaptureCallHz   = 50.0;  // calling, stepped search
inline constexpr double kCfoSearchStepHz    = 12.5;

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
inline constexpr std::array<FloorTable, kNumFamilies> kFloors = {{
    // WIDE
    {{-13.0, -10.5, -8.0, -5.0,
      -8.5, -6.0, -3.5, -1.0, 2.0, 5.0, 8.0, 12.0, 16.0, 20.0, 99.0}},
    // MID
    {{-16.5, -14.0, -12.0, -9.0,
      -12.0, -9.5, -7.0, -4.5, -1.5, 1.5, 4.5, 8.5, 12.5, 16.5, 99.0}},
    // NARROW
    {{-19.5, -17.5, -15.0, -12.0,
      -15.0, -12.5, -10.0, -7.5, -3.5, -0.5, 1.5, 5.5, 9.5, 13.5, 99.0}},
    // ULTRA
    {{-12.5, -11.0, -8.0, -5.0,
      -8.0, -5.5, -3.0, -0.5, 2.5, 5.5, 8.5, 12.5, 16.5, 20.5, 99.0}},
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
// plus channel-quality gates. Measured 2026-07-29, 20-40 trials/point,
// bertool ladders archived in docs/conformance/.
inline constexpr std::array<FloorTable, kNumFamilies> kFadingFloors = {{
    // WIDE (M9 > M10 is real: r3/4 QPSK is more burst-fragile than
    // r1/2 16QAM under fading; M13 = kFadingNA like NARROW's. CSS
    // M2/M3 = kFadingNA: 1 ms delay spread is 2.4 of their chips and
    // the shallow spreading leaves a flat outage tail - ARQ territory)
    {{-3.0, 0.0, kFadingNA, kFadingNA,
      1.5, 1.5, 4.5, 11.0, 10.0, 15.0, 14.0, 16.0, 26.0, kFadingNA, kFadingNA}},
    // MID
    {{-10.0, -7.0, -5.0, -3.0,
      -4.0, -5.5, -3.0, -0.5, 8.5, 13.5, 14.5, 16.5, 24.5, 30.5, kFadingNA}},
    // NARROW (M13 = kFadingNA: 64QAM r5/6 over a flat-Rayleigh one-shot
    // frame has a near-flat outage tail - no 10% point exists at sane
    // SNR; AMC admits it only on near-static channels)
    {{-11.0, -9.0, -8.0, -7.0,
      -7.0, -4.5, -5.0, 0.5, 11.5, 12.5, 12.5, 19.5, 23.5, 99.0, kFadingNA}},
    // ULTRA - RE-MEASURED 2026-08-17 (bertool, 60 trials, M4-M7 on CCIR
    // Moderate and M8-M9 on Good, i.e. each mode's designated channel):
    //   M4 4.0 (unchanged)  M5 6.5 -> 4.5   M6 16.0 -> 10.0
    //   M7 12.5 (knee, no clean 10% point)  M8 8.5 -> 10.5
    //   M9 13.5 -> 15.5
    // The old M6/M7 pair ran 16.0/12.5 - DECREASING as the mode got
    // faster, which is physically backwards and was flagged provisional.
    // M8/M9 were optimistic by ~2 dB, so the AMC was promoting into
    // modes that do not hold up.
    //
    // The recorded cause of ULTRA's remaining ~3 dB deficit vs WIDE
    // ("outermost carriers ride the TX band-FIR transition, 40 Hz
    // margin") is REFUTED: sweeping that margin 40/120/200 Hz changes
    // the floors not at all (see ROADMAP). ~0.5 dB is simple noise
    // bandwidth (72 vs 64 carriers at equal total power); the rest is
    // still unexplained and pilot extrapolation is ruled out too -
    // pilots are evenly spaced across every family's full carrier set.
    {{-4.0, -2.0, kFadingNA, kFadingNA,
      4.0, 4.5, 10.0, 12.5, 10.5, 15.5, 14.5, 18.5, 24.5, 28.5, kFadingNA}},
}};
inline double fadingFloorDb(Family f, int mode);  // active profile

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
};
inline constexpr BandProfile kHfProfile = {
    "hf",     &kFamilies,    kHfCenterBin,    &kFloors,   &kFadingFloors,
    true,     true,          0.160,           CpMode::Medium,
    &kHfCpSamplesAudio,      3,               6,             8.0,  2};
inline constexpr BandProfile kVhfFmProfile = {
    "vhf-fm", &kVhfFamilies, 96 /* 3600 Hz */, &kVhfFloors, &kVhfFloors,
    false,    false,         0.040,           CpMode::Short,
    &kVhfCpSamplesAudio,     2,               4,             11.0, 1};

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
