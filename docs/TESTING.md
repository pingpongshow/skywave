# Skywave Test Strategy

## Principles

- **Simulator-first.** Every claim in [WAVEFORM.md](WAVEFORM.md) is proven
  against the Watterson channel simulator before any hardware test.
- **No-audio testability.** The modem worker is driven directly with sample
  buffers; sockets and PortAudio are never required by any test below the
  system level.
- **Determinism.** Fixed PRN seeds and an injectable clock make every
  failure reproducible from its seed.

## Test Layers

### 1. Unit (GoogleTest, per module)

| Suite | Covers |
|---|---|
| `test_dsp` | FFT known answers, resampler flatness, Hilbert chain SNR, AGC dynamics, window properties |
| `test_chirp` | detection probability vs SNR, false-alarm rate in noise, timing/CFO accuracy |
| `test_zc` | ZC autocorrelation properties, Schmidl–Cox metric accuracy |
| `test_ldpc` | bit-exact encode vectors, decode thresholds per rate, shortening/puncturing |
| `test_crc_whiten` | CRC vectors, whitener periodicity |
| `test_spread` | MRC combining gain vs S, copy-phase derotation |
| `test_frame` | link envelope pack/parse round-trips, malformed-input fuzzing |
| `test_arq` | window management, gap bitmaps, retransmit ordering, timeout adaptation |
| `test_amc` | promotion/demotion rules table-driven, hysteresis, gates, fast paths |
| `test_chanmem` | keying, EMA, aging, recommendation clamps |
| `test_proto` | segmentation/reassembly, compression markers, file checkpoints, resume tokens |
| `test_crypto` | KEX vectors, AEAD round-trip, implicit nonce sequence, replay window, epoch rotation |
| `test_voice` | codec framing, PLC behavior, mode-mapping table |

### 2. PHY Conformance (bertool)

For each family × mode × CCIR preset, bertool sweeps SNR and asserts the
[WAVEFORM.md](WAVEFORM.md) conformance targets:

- ≤10% FER at documented floor (CCIR Moderate)
- ≤1% FER at floor + 3 dB
- NET throughput within 5% of table at floor + 10 dB

Also: header decode floors, sync acquisition probability curves, CFO capture
range (±5 / ±20 Hz), sample-clock offset tolerance (±50 ppm), long-CP
performance on CCIR Poor. Quick profile (`--quick`: Moderate only, 3 SNR
points) gates CI; full sweeps run nightly.

Frame-structure trade-offs are measured here too, not guessed:
`--css-frames` runs the CSS tier (`M0–M3`) through the **full PHY frame
path** (preamble, sync, header copies, LDPC payload → `receiveFrame`) —
unlike `--css`, which measures payload-only FER through the modem-layer
API and cannot see header loss — and `--css-hdr 1` sends a single header
copy (the `css_hdr1` layout). `--payload N` overrides the frame payload
length for any mode. Recorded verdicts (WAVEFORM.md "Header copies —
measured", ROADMAP 2026-09-05): a single CSS header copy saves ~11% of
airtime and costs 20–30 percentage points of FER at the fading floors
(kept at two); doubling OFDM frame length at `M8/M9` on Moderate raises
FER from 29–50% to 54–58% (frame-length adaptation upward not adopted).

Every sweep row also prints three receiver-side figures next to the set
point: `est`, the mean tracker SNR over frames whose header decoded;
`evm`, the mean **data-aided** SNR (decoded bits re-encoded and
re-mapped, noise from the error vector of each equalized symbol) over
frames whose payload decoded; and `pilot`, the mean pilot-stability
excess. On `--preset awgn` `est` is the **SNR calibration** check (the
AMC steers on it against floor tables in the 3 kHz set-point
convention): after `kOfdmSnrCalDb` it reads within ~1 dB of the set
point at the floor region on every HF family, and `evm` within ~1 dB
everywhere. On the fading presets the two diverge, and that divergence
is the estimator's fading bias: on CCIR Moderate the tracker under-reads
by 5–10 dB at the fast tier (channel-motion innovation counted as noise)
while `evm` under-reads by 3–5 (WIDE M10 at 24 dB: est 15.8, evm 19.5).
The tracker figure remains what the AMC steers on; the penalty box, not
the estimate, keeps the fast tier off Moderate. `pilot` is what
calibrated the noise-share coefficient and the classifier's thresholds
(LINK_ADAPTATION.md).

Three further columns carry the **frame-size economics**: `air` (frame
airtime), `ovh` (share of airtime that is not payload symbols) and
`good`, the expected goodput of that frame shape under ARQ —
`payload × (1−FER) / air`, since a failed frame costs its whole airtime
and is sent again. Frame length pays exactly while `good` rises. This is
what sized the robust-tier codeword target (`--payload 140/280/560/1120`
at `--modes 5,6,7`: goodput monotonic, 1.6–2.2×, overhead 79% → 30% at
M7); `SKW_ROBUST_AIR_CAP` is the matching A/B knob in the soak, because
the frame is also the unit of the AMC's 16-frame evidence window.

**The soak is a robustness test, not a throughput benchmark.** Its feed
offers more only while `sent < delivered + SKW_SOAK_WINDOW` (4 KiB by
default) and proto delivery is head-of-line, so delivered bytes are
capped at window / delivery-gap however fast the link is. Measured, that
window is shut 98–100% of a churn run with the modem's TX buffer
repeatedly empty (`starvedA`, `txbufA` in the status line). Any lever
that shortens the delivery gap therefore scores as "throughput" even if
it costs the link airtime — several did, in the rounds before this was
found. Opening the window is not a fix by itself: at 64 KiB the link is
loaded but goodput *falls* 3.3× and the AMC settles two to three rungs
lower. A turn carries ~89 B/s at M9 and at M11 alike. Use
`SKW_SOAK_WINDOW` deliberately, and prefer bertool or the one-way gate
for anything claiming a rate.

**The two-modem world's SNR axis.** `soak_st2` prints `estA`/`estB`,
each modem's calibrated measured SNR, next to the schedule's set point.
That field found the radio chain referencing its noise to unit power
while the worker's audio arrived at its own level times drive, ALC and
converter gains: the static preset delivered **8–10 dB less** than its
label (set 26 read 18.5, set 17 read 8–9). The chain now references the
SNR to the signal power actually arriving at the channel (samples above
−30 dB of the burst peak), and the static preset reads within ~3 dB of
its label. Every soak number recorded before 2026-09-06 (late) sits on
the mislabeled axis.

Diagnostic A/B knobs kept from the same round: `SKW_CHANCLASS_DEBUG`
(per-frame classifier input — pilot excess, EVM, spread, verdict, by
mode), `SKW_DRAIN_CAP_MULT` (turn-taking drain cap in handoff intervals,
default 2; 3 halved the coupled churn), `SKW_DUP_RETRY` (count
duplicate arrivals as retries, the pre-2026-09-06 rule; pinned seed 11
at M6). `SKW_TURN_DEBUG` also prints `TURNSTAT` at every handover — turn
duration, DATA frames, bytes and bursts sent while ISS, mode, srtt — the
per-turn utilisation that sized the minimum hold and the HF window
(`SKW_TURN_HOLD_RTTS`, `SKW_TURN_HOLD_MIN_SEC`, `SKW_HF_WINDOW`,
`SKW_ROBUST_FPB` are the A/B knobs). The churn soak is deterministic but
chaotic: compare cells of a 2×2 across the seed spread and the coupled
schedule, never one run.

### 3. Two-Modem Integration

Two in-process modem instances cross-wired through two independent chansim
instances (asymmetric channels supported):

| Scenario | Asserts |
|---|---|
| `connect_floor` | handshake success ≥ 90% at −8 dB WIDE / family floors |
| `bulk_transfer` | 100 KiB error-free at multiple SNRs; ARQ under injected loss |
| `snr_ramp` | AMC tracking quality vs oracle throughput (Phase-3 milestone) |
| `mode_pin` | pinned-mode behavior + fast-demote safety |
| `mask_excision` | injected tone → mask exchange → FER recovery |
| `resume` | mid-transfer kill → reconnect → file completes, CRC matches |
| `crypto_session` | all of the above with encryption on; replay injection dropped |
| `voice_loop` | PCM in A → RF → PCM out B; loss concealment at 5/10% |
| `disconnect_paths` | orderly, unilateral, and timeout teardowns |

### 4. Protocol Conformance Matrix

A generated table (like the reference design's) mapping every
[PROTOCOL.md](PROTOCOL.md) requirement to its covering test; CI fails if a
requirement row has no passing evidence. Lives at `tests/conformance.yaml`
with a checker script.

### 5. System Test Layers (ST1-ST4)

Exhaustive full-stack validation under rig-realistic conditions, attached
as END-OF-PHASE gates so bugs surface before any on-air test. Each layer
adds realism the previous one cannot model.

#### ST1 - Audio-domain harness + radio-chain emulator (gate: with M3) ✓

The full DSP stack exercised at the REAL 48 kHz audio domain:
`engine TX (12 kHz) -> Upconverter -> radio chain -> Downconverter ->
engine RX`. The radio-chain emulator (`sim/radiochain`) models the whole
PC -> soundcard -> SSB rig -> ionosphere -> rig -> soundcard path:

| Stage | Effects |
|---|---|
| TX rig | PTT lead/tail switching clips, drive-level error, ALC soft compression, TX audio bandpass |
| Path | Watterson fading / AWGN / carrier-point CFO (chansim through the real converters) |
| RX rig | RX bandpass, mains hum + harmonic, DC offset, impulse static crashes, soundcard clock ppm |

`test_system` cases: clean-path integration; all-impairments-at-once at
CCIR Good 18 dB; PTT lead-clip tolerance sweep (10/25/40 ms); hostile-
audio fuzz (tones / noise / AM / hum - no crashes, no fabricated
sessions or data); connect-transfer-disconnect soak cycles with
randomized impairments and per-cycle throughput reporting. CI runs the
short soak; `SKW_SOAK=N` runs the long-haul stress (run under
sanitizers for leak/corruption checks).

#### ST2 - Process-level loopback (gate: Phase 4 exit)

The real `skywave` binary end to end - worker threads, telemetry,
command queue, TCP data/control ports - with an in-process loopback
audio driver in place of PortAudio, radio-chain emulator interposed.
Scripted via the control port (`CONNECT`, `STATUS JSON` scraping);
data through the TCP data port; EVT: notification correctness asserted.
Fault injection: audio dropouts, ring starvation, mid-frame TX abort,
simultaneous-TX collisions, client disconnect/reconnect on every port.

#### ST3 - Real/virtual audio devices (gate: Phase 5 exit)

Two `skywave` processes over real audio hardware: BlackHole pair on
macOS, VB-Cable on Windows, or a physical cable between two interfaces.
This is the only layer with genuinely INDEPENDENT device clocks (real
drift, real buffer timing, real underruns). Requirements:
- scripted two-process connect / bulk / disconnect matrix over the
  device pair (ctest label `hardware`, skipped when devices absent)
- >= 2 h soak: zero session drops on clean audio, no leaks (leaks/ASan),
  ring overflow/underflow counters zero, throughput drift < 10%
- level calibration path exercised (TXTEST + AUDIOLEVEL against the
  device loop)

#### ST4 - On-air readiness (gate: pre-release, attended)

- PTT timing scope check (lead/tail vs rig switching, no hot-key clicks)
- ALC/drive procedure per INTERFACES.md TX Drive Calibration
- attended two-station RF checklist: handshake at distance, mode ladder
  behavior, disconnect paths, monitor station copy

## Failure Artifacts

Every integration failure dumps: RNG seeds, chansim profile, per-frame event
log (CSV), and RX baseband capture (WAV) into `test-artifacts/` — enough to
replay the exact failure through `--replay`.

## Performance Budgets (asserted in CI on a reference runner)

- RX worker: < 25% of one core at `WIDE M13` sustained decode
- TX frame build: < 100 ms for the largest frame
- decode latency: < 1.5 symbol times from last sample to link delivery
- no allocations in the worker after startup (debug allocation hook)
