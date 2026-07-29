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

### 5. System / Hardware Harness (post-Phase-2, manual + scripted)

- loopback via virtual audio devices (BlackHole / VB-Cable): full process,
  real sockets, real PortAudio
- dual-machine or dual-process real-audio runs driving `STATUS JSON`
  scraping for long-duration soak (≥ 2 h, zero session drops on clean audio)
- real-rig checklist: PTT timing scope check, ALC/drive level procedure,
  on-air two-station test script

## Failure Artifacts

Every integration failure dumps: RNG seeds, chansim profile, per-frame event
log (CSV), and RX baseband capture (WAV) into `test-artifacts/` — enough to
replay the exact failure through `--replay`.

## Performance Budgets (asserted in CI on a reference runner)

- RX worker: < 25% of one core at `WIDE M13` sustained decode
- TX frame build: < 100 ms for the largest frame
- decode latency: < 1.5 symbol times from last sample to link delivery
- no allocations in the worker after startup (debug allocation hook)
