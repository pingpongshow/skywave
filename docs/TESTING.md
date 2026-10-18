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

### 1. Unit (`tests/skytest.hpp`, per module)

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

The table above is the design-time list. `ctest` is the inventory of
what actually exists; run it rather than reading this list to find out
what is covered.

### 1b. Docs conformance (`docs_floors`)

`tests/check_docs_floors.py`, run by ctest, asserts that every OFDM
floor published in WAVEFORM.md equals `kFloors` in
`core/constants.hpp`. It exists because three NARROW rows had drifted
from the code without anything noticing: a stale number in a markdown
table looks exactly like a fresh one, and a floor entry is a
measurement, so a wrong one silently mis-aims the AMC. Exits 77 (ctest
SKIP) if it cannot parse either file, so a parser that needs updating
does not masquerade as a floor regression.

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
path** (preamble, sync, header copies, LDPC payload → `receiveFrame`);
it is the CSS conformance gate and defines the CSS floors (the old
payload-only `--css` sweep and its modem-layer API were removed
2026-10-14) — and `--css-hdr 1` sends a single header
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

**The two-modem world is deterministic** (since 2026-10-14): the same
seed gives byte-identical output whatever else the machine is doing, so
one run per configuration is an A/B. It was not before - the harness's
loopback TCP hand-offs landed in whichever simulated round the kernel
delivered them, and parallel load changed whole timelines. Harnesses
must send through `Node::sendData` (never a raw `::send` on `dataFd`).

**Event trace.** `SKW_SOAK_EVENTS=1` makes `soak_st2` print every
`EVT:`/`MON:` line each modem's control port emits, stamped with
simulated time; combined with `SKW_TX_DEBUG`/`SKW_FRAME_DEBUG` it shows
who keyed when and what each side decoded. **Single-seed soak results
are not evidence**: the floor scenario delivers in ~4 KB steps, so one
seed swings 0–25 KB on identical code. Compare totals over six seeds or
more. The world is chaotic down to single bits: changing one reserved
ACK flag bit (identical logic otherwise) moved a six-seed MID floor total
from 79.6 to 65.9 KB, because different bits on the air meet the same
noise differently. Effects under ~20% need well over six seeds; treat
smaller differences as noise.

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

#### ST1-VHF - FM radio emulator

The SSB chain cannot carry the VHF-FM profile (its 300-2700 Hz rig
passband removes most of the 300-6900 Hz signal). `sim/fmradio` models
the PC -> soundcard -> FM rig data port -> RF -> receiver -> data port
path at 48 kHz:

| Stage | Effects |
|---|---|
| TX rig | carrier keyed before the audio (PTT lead) and held after it, key-up loss, drive level, optional pre-emphasis, deviation limiter, data-port audio filter, frequency modulator |
| Path | flat Rayleigh/Rician flutter (sum of sinusoids), RF frequency offset, complex AWGN at a CNR referenced to the IF bandwidth |
| RX rig | IF channel filter, limiter-discriminator (the FM threshold and its clicks emerge from it), squelch closing on carrier drop, optional de-emphasis, data-port audio filter, soundcard clock |

Presets: `fmDataPort()` (25 kHz channel, 300-7000 Hz port - the profile's
assumption), `fmDataPortNarrow()` (12.5 kHz), `fmVoicePort()` (rig on its
voice/1200 path). `test_fmradio` pins the model to FM theory: clean path
transparent, output SNR = CNR - 11.9 dB for its test tone above
threshold, impulsive (clicks) below it.

`tools/fmsweep --preset NAME` sweeps every VHF mode's FER against CNR
(flat, lowedge, narrow, offset, hot, quiet, walk, mobile, uhf, voice);
results and findings in `docs/conformance/vhf_fm_2026-10-17/`.
`test_vhf` runs a full link through the radio (15 Hz flutter, 30 dB
CNR) and decodes frames at every audio sample phase.

#### Phase 8 measurement suites

`tools/gap_sweeps.sh BERTOOL OUTDIR [awgn ladder cfo drift]` runs the
GAP_CLOSURE_PLAN.md standard command lines in parallel (bertool
`--cfo`/`--drift` apply a static offset / linear drift in the channel);
`tools/gap_summary.py DIR [BASE_DIR]` prints the SNR at 10% FER per
ladder row and per-point FER, flagging AWGN points worse than a base run.
The v1.1.0 baseline is archived in `docs/conformance/gap_base/`.

#### ST2 - Process-level loopback (gate: Phase 4 exit)

The real `skywave` binary end to end - worker threads, telemetry,
command queue, TCP data/control ports - with an in-process loopback
audio driver in place of PortAudio, radio-chain emulator interposed.
Scripted via the control port (`CONNECT`, `STATUS JSON` scraping);
data through the TCP data port; EVT: notification correctness asserted.
Fault injection: audio dropouts, ring starvation, mid-frame TX abort,
simultaneous-TX collisions, client disconnect/reconnect on every port.

The world adds a -60 dBFS ambient floor at each receiver by default.
`World::ambientAmp` / `SKW_ST2_AMBIENT=0` gives EXACT digital silence
between bursts instead - what a virtual cable delivers when its writer is
idle or dead. The floor hid the ST3 kill/resume failure for two releases
(GAP_CLOSURE_PLAN.md R7): `st2_peer_death_digital_silence` runs with it
off. FakeClock also hides CPU cost, so worker-overload effects (RX ring
overrun) only show at ST3.

Cycle rate: `cycle_st2` replays the ST3 soak loop (512 B A->B, wait,
repeat) in simulated time - deterministic, so builds can be compared
without the ±10-20% run-to-run swing of real-device soaks.

VHF world: `SKW_ST2_BAND=vhf` runs both modems on the VHF-FM profile
through the FM radio emulator; `soak_st2`'s SNR axis becomes CNR (churn
45 -> 0 -> 45 dB) and its presets become flutter rates (static, 0.7, 15,
50 Hz). It is the only test of the VHF profile through the whole worker
(streaming detector, captures, CCA, TX level, timers): its first run
found that a vhf-fm worker kept HF's converters. `SKW_ST2_FM_BYPASS=1`
passes the audio untouched, to separate the modem from the radio model.

Voice: `voice_st2` runs live voice between the two full modems - CCIR
Good and Moderate, both directions, encrypted (crypto builds), VHF-FM
through the FM radio with flutter and at 700C, and broadcast voice on
static and fading channels - feeding speech in and playing it out in
real time, and scoring how much arrived, its correlation with a local
Codec2 round trip, and the latency to first audio. `st3_hardware` ends
with a voice phase through both modems' TCP voice ports over the real
audio devices. `SKYTEST_ONLY=<substring>` runs only the matching cases of
any skytest binary.

Frequency scenarios: `cfo_st2` runs one-way bulk while B's LO is offset
(`CFO0`), stepped (`STEP` at `STEP_AT`) or drifting (`DRIFT` Hz/s), and
fails on a drop or on fewer than `MIN_BYTES` delivered. Registered as
`cfo_st2_step40`, `cfo_st2_cfo45`, `cfo_st2_drift01`.

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
- `st3_killresume`: five SIGKILL/relaunch cycles, all must reconnect;
  logs are numbered per launch (`st3_<call>_NN.log` under
  `SKW_ST3_LOGDIR`) and the modem logs `rx overrun=N samples` and the
  receive-decode load (`rx decode N attempts, T s total, mean/max ms`)
  at shutdown - nonzero overrun means the worker fell behind real time
  and audio was lost; the decode total against wall time is the RX CPU
  budget a slower host must fit. The harness waits for each relaunched modem's ports rather
  than assuming a fixed start-up time.

#### ST4 - On-air readiness (gate: pre-release, attended)

- PTT timing scope check (lead/tail vs rig switching, no hot-key clicks)
- ALC/drive procedure per INTERFACES.md TX Drive Calibration
- attended two-station RF checklist: handshake at distance, mode ladder
  behavior, disconnect paths, monitor station copy
- one-station form (no second transmitter): bench and one-way skywave
  stages with `tools/onair/` (`onair_sounder` gen/play/score, `flexptt.py`),
  procedure and criteria in [ONAIR_TEST_PLAN.md](ONAIR_TEST_PLAN.md)

## Failure Artifacts

Every integration failure dumps: RNG seeds, chansim profile, per-frame event
log (CSV), and RX baseband capture (WAV) into `test-artifacts/` — enough to
replay the exact failure through `--replay`.

## Performance Budgets (asserted in CI on a reference runner)

- RX worker: < 25% of one core at `WIDE M13` sustained decode
- TX frame build: < 100 ms for the largest frame
- decode latency: < 1.5 symbol times from last sample to link delivery
- no allocations in the worker after startup (debug allocation hook)
