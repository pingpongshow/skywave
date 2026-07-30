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

- local SNR estimate (post-equalizer, EMA α = 0.25)
- freshest remote-reported SNR (from ACK/keepalive bodies)
- retry ratio (retransmitted / total, sliding 16-frame window)
- decode-failure ratio (header-ok-payload-fail / total)
- frequency-selectivity index (per-carrier SNR variance from the equalizer)
- pilot stability (variance of pilot phase innovations)
- dwell time since last mode change

Effective SNR for decisions: `min(local, remote)` when the remote report is
< 60 s old, else local with a −1.5 dB staleness penalty.

### Promotion / Demotion Rules

Per-family mode floors come from the WAVEFORM tables. AMC applies margins:

| Rule | Threshold |
|---|---|
| promote to mode `k` | effective SNR ≥ floor(k) + **2.5 dB** for ≥ 2 consecutive evaluations, retry ratio ≤ 0.15, dwell ≥ 6 s |
| demote from mode `k` | effective SNR < floor(k) + 0.5 dB, **or** retry ratio > 0.35, **or** 3 consecutive decode failures |
| fast demote | 2-step drop when retry ratio > 0.5 in the window, min dwell waived |
| promotion step | single step only (`k → k+1`); no promotion skipping |

Hysteresis: the 2.0 dB gap between promote margin (+2.5) and demote margin
(+0.5) prevents oscillation; dwell timers prevent thrash on flutter.

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

Store: 64 entries, LRU eviction, persisted to the config directory as a
small binary cache (survives restarts).

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
