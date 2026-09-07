# Skywave Link Adaptation

## Scope

The policy layer that chooses and maintains the runtime mode:

- AMC — adaptive mode controller (the ladder driver)
- channel memory (reconnect seeding)
- probe-before-promote (fragile-mode safeguard)
- carrier masking (per-carrier interference excision)
- demotion fast paths

Mode tables and floors are defined in [WAVEFORM.md](WAVEFORM.md).

## AMC Controller

### Inputs

Aggregated each received frame and each ACK:

- local SNR estimate (post-equalizer, EMA α = 0.25) — **calibrated to the
  3 kHz-noise convention of the floor tables** (`kOfdmSnrCalDb`, measured
  per family with `bertool --preset awgn`: the tracker's per-carrier
  figure sits +4 / +8 / +11 / +3.5 dB above it on WIDE / MID / NARROW /
  ULTRA — the bandwidth term plus a ~3 dB pilot-fit bias), and fed
  **only from frames that converged the tracker** (≥ 8 pilot-bearing
  symbols; a fast-tier ACK reports the noise seed, not a measurement,
  and feeding those swung the effective SNR 14 → 7 dB on a static channel)
- the handshake measurement seeds the EMA at connect; with no sample yet
  the controller makes no SNR-based decision (failure-driven demotions
  still apply)
- freshest remote-reported SNR (from ACK/keepalive bodies)
- retry ratio (retransmitted / total, sliding 16-**frame** window, cleared
  on every mode change). `Amc::windowSpanSec` reports how much wall time
  that window covers — it is frame-counted, so at robust-tier airtimes it
  can describe a stretch of channel minutes old. On a one-way transfer
  the sender's window is *empty* (`fill=0`): its frame evidence comes
  from DATA it receives, so it adapts on SNR alone, which is what the M3
  ramp measures. A received retransmission counts only when it is
  the **first copy to arrive**; a duplicate proves the original got
  through, so that retransmission was the MAC's doing (a frame stranded
  at a turn handover, the drain's fast-retransmit racing a late ACK),
  not the channel's. Counted as retries, those held the ratio at
  0.2–0.5 and pinned the ladder at M6 for 300 s at a measured 10–12 dB,
  above the 0.15 promotion gate.
- decode-failure ratio (header-ok-payload-fail / total)
- frequency-selectivity index (per-carrier SNR variance from the equalizer)
- pilot stability — the pilot-residual energy relative to channel power
  **in excess of its noise share**. The raw ratio is ~0.7 / SNR on pure
  AWGN (bertool `pilot` column: 2.95 at M4 −7.5 dB, 0.006 at M11 22 dB,
  zero Doppler), so uncompensated it made every robust-tier link below
  ~−3 dB read SEVERE and no static channel read STATIC below ~13 dB.
  The noise share is taken from the data-aided (post-decode EVM) noise
  figure, which does not contain channel motion; the tracker's own noise
  does, and subtracting it cancelled the excess (Moderate read 0 too).
  Frames without a decoded payload (control frames, failures) give no
  sample rather than a zero. After compensation: AWGN 0.001–0.007, CCIR
  Good 0.001–0.009, CCIR Moderate 0.02–0.06 at M6 and 0.008–0.014 at
  M10. The `M11+` gate's 0.25 bound (calibrated on an earlier tracker's
  raw ratio) is effectively inert against these levels; the penalty box
  is what keeps the fast tier off a Moderate channel.
- dwell time since last mode change

Effective SNR for decisions: `min(local, remote)` when the remote report is
< 60 s old, else local with a −1.5 dB staleness penalty.

### Promotion / Demotion Rules

Per-family mode floors come from the WAVEFORM tables. AMC applies margins:

| Rule | Threshold |
|---|---|
| promote to mode `k` | effective SNR ≥ floor(k) **− 1.5 dB** for ≥ 2 consecutive evaluations, retry ratio ≤ 0.15, dwell ≥ 6 s, not in the penalty box |
| demote from mode `k` | effective SNR < floor(k) − 3.5 dB, **or** retry ratio > 0.35, **or** 3 consecutive decode failures |
| fast demote | 2-step drop when retry ratio > 0.5 in the window, min dwell waived |
| promotion step | single step only (`k → k+1`); no promotion skipping |

Multi-rung promotion was built and measured (`AmcConfig::maxPromoteStep`,
bounded at 2, only into a rung clearing its floor by an extra 3 dB with
a clean retry window): M3 ramp 0.87 → 0.89 but the coupled churn
196,667 → 122,931 B. The ramp is a tracking metric on a monotone sweep,
where skipping a rung is free; on a fading channel the skipped rung is
the evidence. Left at 1. The underlying limit — one rung per
`MODE_CHANGE` round trip, so the climb rate is set by exchange latency
rather than by the channel — is unaddressed; the AMC trails a genie
oracle by one to three rungs the whole way up a ramp.

The margins are **negative on purpose**: the floors are one-shot 10–15%
FER points, and with chase/IR HARQ plus selective-repeat ARQ a rung is
throughput-optimal down to ~1.5 dB below its floor (a rung is worth ~1.5×
the one under it, so it pays until FER ≈ 33%). These are the values the
M3 ramp gate was tuned at (0.94 of genie on CCIR Good, 0 drops); before
the estimator was calibrated they read as +2.5 / +0.5 against a WIDE
estimate ~4 dB optimistic — i.e. they always were −1.5 / −3.5 in table
terms, and MID / NARROW (8 / 11 dB optimistic) were running far more
aggressively than designed. Hysteresis: the 2.0 dB gap between promote
and demote margins prevents oscillation; dwell timers prevent thrash on
flutter.

**Penalty box (failure memory).** A demotion driven by frame *failures*
(retry ratio, fail streak, fast demote) — not by SNR — remembers the
rung it left. Promotion back into that rung or above holds for 30 s,
doubling on every repeat at the same or a higher rung (60, 120, 240 s),
unless the effective SNR has risen ≥ 6 dB since the failure; the memory
clears once the link operates at or above the failed rung cleanly for a
full hold. (A 15 s first hold was shorter than one promote-fail cycle
at the fast tier, and a 3 dB override was unlocked by the ±3 dB
frame-to-frame swing of the estimate on CCIR Moderate — measured as
M9 ↔ M10 every ~40 s on a 23–26 dB Moderate channel, never learning.)

**Fresh trial per rung.** A mode change clears the 16-frame retry
window: it is evidence about the mode it was collected at. Carried
across, one bad window condemned every rung below it — measured as
M10 → M9 → … → M4 → CSS M1 in 170 s, one rung per 6 s dwell, on a 23 dB
Moderate channel with no frame completed between decisions. The SNR
floor rule and the 3-failure streak still demote immediately when the
channel really collapses. SNR alone is not evidence that a rung works: the fast
tier (M8+) fails 30–60% on CCIR Moderate at *any* SNR (its floors are
measured on Good, see the WAVEFORM fading annex), and an SNR-only ladder
re-tried it every promote-hold. SNR-explained (floor-break) demotions are
deliberately not remembered: the SNR coming back is exactly the evidence
that should let the ladder climb again.

### High-Mode Gates

Extra requirements stacked on top of SNR for fragile modes:

| Mode | Additional gates |
|---|---|
| `M11+` | retry ratio ≤ 0.10, pilot stability below threshold |
| `M12+` | + frequency-selectivity index below threshold (deep notches make 64QAM pointless even at high SNR) |
| `M13` | + probe success required ([below](#probe-before-promote)) |

### Manual Override

`MODE Mx` (control port) pins the mode: AMC stops promoting/demoting but the
fast-demote safety rule **still applies** (a pinned mode that cannot decode
is useless). `MODE AUTO` returns control to AMC. Pinning to `M0–M3` is
rejected in v1.

## Channel Memory

Avoids restarting every reconnect from a naive guess.

### Keying

- local callsign hash, remote callsign hash
- frequency bucket (from companion-app-reported dial frequency when
  available; bucket 1 kHz; "unknown" is its own bucket)
- family

Store: 64 entries, LRU eviction, persisted to `~/.skywave/chanmem.bin` as a
small binary cache (survives restarts).

Wiring: the worker owns the store and hands the engine a pointer only while
`modem.channel_memory` is on, so `CHANMEM OFF` disables both seeding **and**
recording. The recommendation is installed into the handshake policy just
before each handshake decision and clamps the quality-derived proposal on
*both* sides — the initiator's `START.initial_mode` and the responder's own
view — so the lower of memory and live measurement always wins. Observations
are folded in when a session ends, not continuously: mid-session samples
during a handshake or a collapse would bias the EMA toward transients.
Frequency buckets come from `FREQINFO <hz>` ([INTERFACES.md](INTERFACES.md));
with no dial report the store keys on callsigns and family alone.

### Stored State

Per entry, EMA-smoothed (α = 0.2): last stable mode, SNR, retry ratio,
decode-failure ratio, selectivity index, last carrier-mask, timestamp.

### Recommendation

On connect to a known key:

- start from stored stable mode
- −1 mode if stored retry > 0.18; −2 if > 0.30
- −1 mode per 5 min of age (half-life decay), capped at −3
- entries older than 30 min are ignored
- result clamps to `M4..config max`; used as the `START` initial-mode
  proposal input alongside the live handshake quality measurement — the
  **lower** of the two wins

## Probe-Before-Promote

Before committing promotion into a fragile mode, verify it live.

- applies to promotions into `M11+` (config-adjustable)
- eligibility: retry ratio ≤ 0.08 and stable pilots
- probe: 1 `PROBE` frame (2 for `M12+`) transmitted at the **candidate**
  mode carrying a known test payload; peer answers `PROBE_ACK` (at the
  current mode) with decode quality: clean / marginal / failed
- clean → promote; marginal or failed → stay, cooldown 10 observed frames
- probes count as traffic for keepalive purposes

## Carrier Masking

Per-carrier excision of persistent narrowband interference (the unified-PHY
successor to tone masking).

### Detection

The receiver maintains per-carrier quality weights from equalizer SNR and
LDPC soft-error concentration. Carriers persistently ≥ 12 dB worse than the
band median for ≥ 5 s become mask candidates.

### Exchange

- `MASK_UPDATE` body: `mask_id` (u8, cycles 0–15), flags (u8:
  `ACTIVATE_NOW`, `FULL_REPLACE`, `RLE`), carrier count (u16), bitmap
  (raw or RLE, LSB = lowest active carrier)
- receiver of a mask **transmits** with masked carriers zeroed (no data, no
  pilots relocated — pilots on masked carriers are skipped and the
  estimator interpolates)
- peer confirms with `MASK_ACK(mask_id)`; sender activates on ACK or on
  `ACTIVATE_NOW`
- retry: 2 s cadence, 2 attempts, then abandon this mask generation

### Interaction with the Ladder

- masked carriers reduce per-symbol capacity; the PHY reports effective
  data-carrier count and NET rates scale accordingly (STATUS reflects real
  throughput)
- if masked ratio > 0.30, AMC demotes one step to regain margin and clears
  the oldest mask generation
- spread modes (`M4–M7`) skip masking: spreading already averages over
  interference, and robust-tier airtime is too precious for mask exchange

## Demotion Fast Paths

Signals that bypass normal evaluation cadence:

- **ACK starvation**: 2 consecutive ACK timeouts → treat as fast-demote
  trigger (channel may have collapsed below current mode)
- **header-only reception**: robust header decodes but payload repeatedly
  fails → immediate one-step demote request via `MODE_CHANGE`
- **remote distress**: ACK rx-quality flags include a `DEGRADED` bit the
  peer sets when its decoder is near failure; receipt biases AMC one
  evaluation toward demotion

## Advisor Layer (Phase 6)

A bounded, deterministic, dependency-free predictive helper (successor to
the reference design's ML assist), implemented in `src/link/advisor.*` as
closed-form least-squares trends over short bounded histories:

- domains: AMC step bias (SNR slope ≥ ±1.5 dB/min, retry-vetoed, applied
  as a one-shot single-evaluation bias), CFO drift shaping (predicted
  oscillator drift counteracted ≤ 0.5 Hz per evaluation inside the global
  TX-shift clamp), mask threshold tuning (proposal churn hardens the
  below-median threshold, clamped ±1.5 dB)
- modes: `off` / `shadow` (log only) / `assist` (apply when the domain's
  confidence — trend fit quality × magnitude — clears its floor)
- constraints: single-step influence only, clamped to legal ranges, never
  overrides handshake mode selection, no effect on voice
- control: environment variables only (`SKW_ADVISOR=off|shadow|assist`,
  per-domain confidence floors); default `shadow` until validated in
  hardware, then `assist`

## Channel Character (floor-table selection)

The AMC operates against the measured **fading** floors by default. That
is the safe assumption and the wrong one at both extremes, both measured:

- a genuinely **static** path leaves most of its capacity unused (an AWGN
  ramp delivered 39% of an AWGN-floor oracle);
- a **CCIR-Poor-class** path cannot carry the upper ladder at all — WIDE
  never reaches 10% FER at *any* mode or SNR
  ([conformance/poor_findings.md](conformance/poor_findings.md)).

`link/chanclass.hpp` classifies the path from evidence the receiver
already produces — no extra measurement on air, no wire format, no
negotiation:

| Signal | Static | Fading (Good) | Moderate | Severe (Poor) |
|---|---:|---:|---:|---:|
| pilot excess (see Inputs) | 0.001–0.007 | 0.001–0.009 | ≥ 0.010 (0.008–0.014 at M10, 0.02–0.06 at M6) | ≥ 1.2 |
| delay spread | ~0 | ~0.5–1 ms | ~1 ms | ≥ 2 ms |
| SNR variance | low | moderate | moderate | high |
| frame error rate | ~0 | low | fast tier fails | ≥ 35% |

`Static` requires **all** gates; `Severe` requires **any two**;
`Moderate` is the pilot excess alone (its noise share is already
removed, so it is not an SNR proxy) once the EMA has been seeded from
decoded payload frames — checked before `Static`, because a Moderate
channel at high SNR looks quiet on every other signal. A verdict must
repeat (`confirmRuns`) before it replaces the last one, because
switching floor tables mid-session is disruptive and flapping is worse
than being wrong in one direction. `Unknown` keeps the fading floors.
(The pilot column was re-based on 2026-09-06: the older raw-ratio
figures — AWGN 0.003–0.025, Good 0.010–0.057, Moderate 0.38–0.55 — were
~0.7/SNR plus motion and belonged to an earlier tracker.)

Consumption: `Static` switches `Amc::operatingFloorDb()` to the AWGN
reference table (measured effect: static ramp 0.39 → 0.67 of its oracle);
`Severe` caps the ladder at `severeMaxMode` (M7, the robust tier);
`Moderate` can cap it at `moderateMaxMode` (M8 when enabled; **off by
default**, 13) — the fast tier's fading floors are measured on CCIR Good
(WAVEFORM annex) and it fails 25–75% on Moderate at any SNR (bertool),
while M8 still closes at 6%; measured with the cap on, the churn seed
spread was a wash and the coupled schedule lost 8.8 KB and its pass, so
the verdict is recorded (STATUS) but not acted on until a seed spread
shows a benefit. Both caps
act on eligibility only; a verdict that arrives while the ladder sits
above its cap leaves the failure rules to bring it down (a forced
one-rung-per-dwell walk-down was measured and withdrawn: under ISI the
Severe verdict marched M10 → M6 in 18 s and starved CPMODE AUTO of
decoded frames). The Moderate threshold is provisional: in the two-modem
churn the verdict was driven by single pilot spikes through the 0.15
EMA, and its cap did not change the soak outcomes.
