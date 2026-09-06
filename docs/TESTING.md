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

Every sweep row also prints `est`, the receiver's mean estimated SNR over
the frames whose header decoded, next to the set point. On `--preset
awgn` this is the **SNR calibration** check (the AMC steers on this
number against floor tables in the 3 kHz set-point convention): after
`kOfdmSnrCalDb` it reads within ~1 dB of the set point at the floor
region on every HF family. On the fading presets it also shows the
estimator's fading bias — CCIR Moderate under-reads by 4–6 dB at M8
(channel-motion innovation counted as noise), which is left in place
because it keeps the fast tier out of a channel its floors were never
measured on.

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
