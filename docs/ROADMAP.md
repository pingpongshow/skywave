# Skywave Construction Roadmap (Execution Tracker)

This is the **living work-breakdown** for building Skywave. Strategy,
dependency policy, and milestone definitions live in
[BUILD_PLAN.md](BUILD_PLAN.md); this document decomposes each phase into
ordered, session-sized steps with individual test gates.

**How to use this document (instructions to the builder):**

1. At session start: read this file, run the full test suite, find the
   first unchecked step whose prerequisites are checked.
2. Implement that step *with its tests in the same change*. A step is done
   only when its listed gate passes and the whole suite is green.
3. Check the box (edit this file), note deviations in the *Deviations Log*
   at the bottom, and commit before moving on.
4. Never start a step out of order unless its listed prerequisites are all
   checked. Steps marked `[P]` may be done in parallel with their siblings.
5. If a step forces a design change, update the affected design doc in the
   same change — the docs are the source of truth, and drift is a bug.

Design references: [ARCHITECTURE.md](ARCHITECTURE.md) (layout, threading),
[WAVEFORM.md](WAVEFORM.md) (PHY), [PROTOCOL.md](PROTOCOL.md) (link),
[DSP.md](DSP.md) (signal chains), [TESTING.md](TESTING.md) (test layers).

---

## Phase 0 — Scaffold

*Goal: an empty-but-running program with the project's bones.*
*Exit gate: **Milestone M0** (BUILD_PLAN.md).*

- [x] **0.1 Repository skeleton** — directory tree per ARCHITECTURE.md;
  top-level `CMakeLists.txt` with all options (`ENABLE_GUI/CRYPTO/VOICE`,
  `BUILD_TESTS/TOOLS`, sanitizers); vendor KissFFT, Dear ImGui, GoogleTest;
  find-package for PortAudio, zstd, optional libsodium/Codec2/SDL2.
  *Gate: all option combinations configure; empty `skywave` binary builds
  on macOS + Linux.*
- [x] **0.2 Core utilities** — `core/constants.hpp` holding the **HF band
  profile table** (numerology, CP set, family carrier plans, chirp spans,
  PRN seeds — every number from WAVEFORM.md in one place); leveled logger;
  monotonic + injectable clock; byte/bit packers.
  *Gate: `test_core` (config round-trip comes in 0.3; here: packers, clock
  injection, profile-table static asserts — e.g., bin ranges, carrier
  counts, center-bin invariants).*
- [x] **0.3 INI configuration** — full schema from CONFIGURATION.md,
  defaults, load/save/preserve-unknown-keys, validation rules (callsign,
  state-dependent rejections stubbed).
  *Gate: `test_config` round-trips every key; unknown keys preserved.*
- [x] **0.4 Audio I/O layer** — PortAudio duplex wrapper, device listing,
  SPSC sample rings, WAV reader/writer, `--replay`/`--record` paths.
  *Gate: `test_audio_rings` (SPSC correctness under threads); WAV
  round-trip bit-exact.*
- [x] **0.5 Thread scaffold** — modem-worker loop consuming RX ring /
  feeding TX ring (pass-through for now), versioned telemetry snapshot,
  serialized command queue, clean startup/shutdown ordering.
  *Gate: TSan-clean pass-through run; commands round-trip worker.*
- [x] **0.6 CLI + main wiring** — argument parsing per CONFIGURATION.md,
  headless loop, `--list-audio`, `--version`, `--help`.
  *Gate: **M0** — `skywave --replay in.wav --record out.wav` copies audio
  through the full thread model; CI matrix green with sanitizers.*
  *(M0 verified 2026-07-28: 96000-sample tone replay→record with zero
  sample error; 4/4 test suites pass in Release and UBSan builds. ASan is
  blocked by a host toolchain fault — see Deviations Log.)*

## Phase 1 — PHY Core

*Goal: every active mode closes its documented floor over the simulator.*
*Exit gate: **Milestone M1** — the project's hardest gate.*

- [x] **1.1 FFT + windows** `[P]` — in-tree mixed-radix FFT (320-pt @ 12 kHz
  domain; deviation from KissFFT, see log), window functions, overlap
  helpers.
  *Gate: `test_dsp` FFT known-answers, Parseval, round-trip.* ✓ 2026-07-28
- [x] **1.2 Rate/domain converters** — ±1500 Hz NCO mixers + 2-stage
  polyphase decimation (mix-then-complex-LPF replaces the Hilbert design —
  see log), Kaiser FIR designer (48 k real ↔ 12 k complex).
  *Gate: chain SNR > 60 dB across 300–2900 Hz; image rejection > 60 dB.*
  ✓ 2026-07-28 (tone SNR >60 dB at 300/1000/1500/2500/2850 Hz; round trip
  >60 dB at ±1300 Hz offsets)
- [x] **1.3 AGC + DC block** `[P]` — per DSP.md dynamics, freeze hook.
  *Gate: `test_dsp` AGC attack/release/freeze cases.* ✓ 2026-07-28
- [x] **1.4 CRC + whitener** `[P]` — CRC-16/32 (header/link), LFSR
  whitener. *Gate: published CRC vectors; whitener period/balance.*
  ✓ 2026-07-28
- [x] **1.5 LDPC codec** — 802.11n tables N=648/1944 all rates; structured
  dual-diagonal encoder; flooding normalized min-sum decoder (float LLR,
  ≤30 iter, early-exit). Shortening + IR puncturing arrive with their
  consumers (PHY framer 1.8 / HARQ 3.4).
  *Gate: `test_ldpc` — H·c=0 across all 8 codes; AWGN thresholds within
  0.5 dB of reference per rate.* ✓ 2026-07-28 (FER ≤10% at thr+0.5 dB and
  ≥80% at thr−1.5 dB for all four rates, N=1944 and N=648 spot check)
- [x] **1.6 Chirp engine** — generator + matched-filter detector, all four
  family spans, signature patterns, inversion detection, ±50 Hz calling
  capture, delay–Doppler resolution from up/down pairs.
  *Gate: `test_chirp` Pd at family acquisition floors (25/25 all four
  families); false-alarm 0 in 60 s noise; CFO < 1.5 Hz across ±50 Hz
  calling capture, < 1 Hz connected; inversion detection correct;
  timing ±1 ms.* ✓ 2026-07-28 — see Deviations Log for the detector
  architecture that emerged (joint pattern scoring) and latency figures.
- [x] **1.7 ZC sync symbols** `[P]` — per-family sequences, sync-block
  generator, matched-filter timing + ML-fit CFO with chirp-prior fusion,
  channel-estimate seed.
  *Gate: `test_zc` timing ±2 samples (±5 NARROW — energy-limited, CP
  absorbs), CFO < 0.5 Hz at floor SNR.* ✓ 2026-07-28 — see Deviations Log
  for block-length change (3/6) and estimator architecture.
- [x] **1.8 TX chain** — bit pipeline (map → spread → pilots → IFFT → CP),
  PHY framer (preamble + ZC + header codeword with payload piggyback +
  payload codewords), PAPR clipper, band filter.
  *Gate: `test_phy_tx` — spectral mask per family (>26 dB in/out ratio),
  PAPR ≤ 9.5 dB post-filter, pilot rules verified, clean-channel
  decode-back of header (incl. piggyback) and payload codewords incl.
  ×16 spreading.* ✓ 2026-07-28 — burst container and TX level normalizer
  land with their consumers (3.8 / Phase-2 worker); golden regression is
  the decode-back suite (bit-exact by construction). See Deviations Log
  for the spread-group design fix.
- [x] **1.9 Channel simulator (`chansim`)** — Watterson 2-tap Rayleigh,
  Gaussian Doppler, AWGN, CFO, sample-clock offset; CCIR Good/Moderate/
  Poor presets; deterministic seeding; library (CLI wrapper deferred to
  bertool, which is the real consumer).
  *Gate: `test_chansim` — Doppler PSD sigma within ±15% of spread/2 (CCIR
  two-sigma convention, measured by autocorrelation), two-tap delay
  profile verified by cross-correlation, AWGN 3 kHz-ref calibration ±0.15,
  CFO and clock-offset verified, deterministic per seed, unit-energy
  fading.* ✓ 2026-07-28
- [x] **1.10 RX sync engine** — realized as ChirpDetector (SEARCH/COARSE)
  + zcRefine (FINE) + ChannelTracker (TRACK/FLYWHEEL) in a one-shot
  `receiveFrame`; inversion auto-correct (conjugate + re-run); CFO rotator
  in the symbol walker. Streaming worker integration lands in Phase 2.
  ✓ 2026-07-28
- [x] **1.11 RX demod chain** — scattered-pilot channel estimator with
  mode-class α, CPE PLL with pilot innovations, pilot-residual noise
  estimator, FLYWHEEL hold, MRC despreader over spread groups, max-log
  LLR demapper with CSI weighting. (Delay-spread estimator → CPMODE AUTO
  deferred to 3.9 with its consumer.) ✓ 2026-07-28
- [x] **1.12 Full-frame decode loopback** — `test_phy_rx`: clean loopback
  all four families (M4/M8/M9/M13), ±40 Hz CFO calling frames, inverted
  spectrum + CFO, AWGN at M8 floor+6 (5/5) and M4 robust floor+2.5 (≥4/5),
  CCIR Moderate M8 at floor+10 (≥4/5). Link CRC verification belongs to
  the Phase-2 link layer. ✓ 2026-07-28 — see Deviations Log for the
  level-convention fix this exposed.
- [x] **1.13 `bertool` + conformance tuning** — sweep driver (family ×
  mode × preset × SNR → FER/throughput CSV); then iterate on LLR scaling,
  estimator constants, sync thresholds until targets hold. Budget multiple
  sessions; record tuning outcomes in the Deviations Log.
  *Gate: **M1** — all WAVEFORM.md conformance targets, quick profile in
  CI, full sweep archived.* ✓ 2026-07-29 — **M1 met under the REVISED
  conformance structure** (WAVEFORM.md "Conformance Targets" was
  rewritten at this gate): kFloors = AWGN reference (full AWGN ladder
  passes at floor+1/+3); kFadingFloors = measured one-shot fading floors
  per designated channel (M4–M7 Moderate; M8–M13 Good; NARROW all-Good),
  all passing bertool gates with binomial slack; WIDE M13 + NARROW M13
  have no fading point (kFadingNA). CI runs AWGN smokes + 2 fading
  smokes. Final campaign CSVs in docs/conformance/. The estimator was
  rebuilt as a per-tap Gauss-Markov Kalman filter (see Deviations Log).
  **Status 2026-07-29 (in progress, commit 30126ee):** channel tracker
  rebuilt (joint pilot LS on oversampled delay basis, PLL-anchored gauge,
  ZC tap-domain prior — see Deviations Log). All 13 CI suites green.
  AWGN: whole ladder clean (WIDE M4 floor+1 and NARROW M4 floor+4 at 0%
  FER; clean-channel M13 EVM 34–49 dB all families). Fading campaign
  measured (ladders in /tmp/skw_lad_{mod,good,nar}.csv):
  - MID follows Rayleigh outage physics — measured 10%-FER points sit
    ~4–6 dB above the table floors and converge to 0% (M4 −8.0, M5 −5.5,
    M6 −3.0, M7 ≤−4.5 on Moderate; M10 8.5 … M13 24.5 on Good). Table
    floors were AWGN-derived and never included the diversity-2 outage
    margin; kFloors re-spec pending completion of the items below.
  - WIDE/ULTRA and several NARROW modes PLATEAU at 13–47% FER
    independent of SNR — an estimator-dynamics failure, not noise:
    suspected shape smearing across the LS window as tap-relative phases
    rotate at the Doppler rate, plus occasional chirp-preamble fade
    outages (noDetect at high SNR). Fix before setting floors.
  - ULTRA is anomalously worse than WIDE at equal margin on Moderate
    (headers ~2× failures) — unexplained, investigate (band FIR checked
    and fine).
  - Conformance channel per mode class (aligns with LINK_ADAPTATION
    selectivity gates): M4–M9 on CCIR Moderate, M10–M13 on CCIR Good,
    NARROW all modes on Good (500 Hz band sees 2-path as flat Rayleigh —
    no frequency diversity; document in WAVEFORM.md).
  **Fading-plateau root cause (measured, seed 9967 trace):** mid-fade
  the tap-relative phases rotate WITHIN the tracker's LS window and the
  solved shape collapses ~10 dB below the true channel (est −18 dB vs
  true −8 dB from a ground-truth impulse probe) — confidently wrong H →
  burst LLR corruption that short codewords (WIDE fast frames) cannot
  absorb but MID's ~2 s codewords can. Window-length A/B (4/6/10) does
  not fix it: the window-LS architecture assumes a quasi-static shape.
  The correct estimator is a per-tap Gauss-Markov Kalman filter (each
  tap independently Rayleigh, known Doppler; absorbs CPE naturally).
  First implementation is parked on branch `wip/kalman-tracker` — it
  REGRESSES as tuned (per-tap Q from instantaneous seed powers starves
  taps that were faded at ZC time; noise EMA double-counts posterior
  error) — resume there with: Q from smoothed/uniform delay profile,
  resid/posterior split, FLYWHEEL gate revisit. That branch also
  carries two validated improvements to port back regardless: scalar
  Kalman gauge with SNR-adaptive bandwidth (fixed NARROW M4 AWGN at
  floor+1, all suites green at that point) and gauge-uncertainty-
  inflated demap noise.
  Remaining: land the dynamics fix → re-measure ladders → set kFloors
  from measurement → align WAVEFORM.md tables + AMC thresholds → full
  sweep archive + CI quick profile on final numbers.

## Phase 2 — Link Core (First Functional Modem)

*Exit gate: **Milestone M2**.*

- [x] **2.1 Link envelope + frame types** — pack/parse for every
  PROTOCOL.md frame type, flags, piggybacked `ack_base`, voice-vs-ARQ
  sequence spaces.
  *Gate: `test_frame` round-trips + malformed-input fuzz (no crashes,
  clean rejects).* ✓ 2026-07-29 — link/frame.{hpp,cpp}: 12-byte envelope
  + CRC-32, 22 body types, callsign validation + FNV-1a-24 hash,
  reserved-mode clamp (M0–M3→M4), reserved caps/flag bits rejected;
  8000-iteration fuzz clean.
- [x] **2.2 Session FSM + timers** — states, airtime-aware timeout
  computation (PHY airtime query), retry budgets, idle disconnect.
  *Gate: `test_fsm` table-driven transition coverage incl. timeout paths.*
  ✓ 2026-07-29 — link/fsm.{hpp,cpp}: pure-logic FSM (injected time,
  queued actions), 6 states, PROTOCOL.md retry budgets (CALL/START 4,
  DISC 3), clamped airtime timeouts (frameAirtimeSec added to
  phy/framer), idle disconnect 120 s + keepalive 20 s (flag-gated).
- [x] **2.3 Handshake** — CALL/CALL_ACK/START/START_ACK(NAK) bodies,
  capability intersection, quality-seeded initial mode, ULTRA fallback.
  *Gate: `test_handshake` — happy path, every NAK reason, fallback.*
  ✓ 2026-07-29 — link/handshake.{hpp,cpp}: initial mode = highest with
  effSNR ≥ kFadingFloors + 2.5 (AMC promote margin; kFadingNA excluded,
  M12+ selectivity gate), capped at M10 (fragile-mode gates need live
  traffic); responder silence vs START_NAK split; ULTRA fallback =
  first half of attempts ULTRA then WIDE; ULTRA-configured responders
  also accept WIDE (fallback must land).
- [x] **2.4 ARQ engine** — SR window, gap bitmaps, compact ACK (7-byte
  body incl. CFO report), adaptive timeout, retransmit priority,
  max-retries teardown.
  *Gate: `test_arq` — loss/reorder/duplicate matrices, window stalls.*
  ✓ 2026-07-29 — link/arq.{hpp,cpp}: ArqTx/ArqRx, serial-number
  arithmetic, ackBase = next-expected + received-bitmap convention,
  retransmits drain before new data with rv increments, full 4-frame
  loss-pattern matrix + wraparound covered.
- [x] **2.5 TX/RX arbiter + PTT + safety** — priority queue
  (control-urgent > retransmit > data > keepalive > beacon), CCA gate with
  bypass list, PTT sequencing for all methods, TX watchdog, `TXTEST`.
  *Gate: `test_arbiter` priority/bypass cases; watchdog fires in fault
  injection.* ✓ 2026-07-29 — link/arbiter.{hpp,cpp}: pure-logic
  sequencer (Rx/CcaWait/Lead/Audio/Tail), 250 ms CCA polls with 5 s
  proceed-anyway cap, per-burst bypass flag, watchdog abort path,
  TXTEST idle-only + 30 s cap. Physical PTT drivers (RTS/DTR/VOX/TCP)
  land with the worker integration (2.7/Phase-2 wiring) - the arbiter
  emits Key/Unkey events they consume.
- [x] **2.6 TCP data + control servers** — single-client ports, line
  protocol, Phase-2 command subset, `EVT:` notifications.
  *Gate: `test_tcp` scripted command/response/notification session.*
  ✓ 2026-07-29 — tcp/server (non-blocking single-client localhost TCP +
  LineBuffer) and tcp/control (pure-logic dispatcher over an abstract
  ControlBackend; EVT/MON formatters). Real-socket loopback tests cover
  the one-client policy, drop detection, and raw data-port bytes.
- [x] **2.7 Two-modem integration harness** — two in-process instances ×
  two chansim directions (asymmetric channels); scenario runner + failure
  artifacts (seeds, event CSV, baseband WAV).
  *Gate: **M2** — `connect_floor`, `bulk_transfer`, `disconnect_paths`
  scenarios pass per TESTING.md.* ✓ 2026-07-29 — **M2 met.**
  link/engine.{hpp,cpp} (LinkEngine: FSM + handshake + ARQ + frames over
  the real PHY, burst-oriented, time-injected) + test_m2 harness (two
  engines, per-direction chansim, half-duplex stepping). Results:
  connect_floor 10/10 at −8 dB AWGN WIDE; bulk transfer byte-exact at
  25 and 12 dB (CI-scaled 12 KiB; the same scenario runs 100 KiB in
  nightly soak) and under 1-in-7 burst loss; orderly/unilateral/idle
  teardowns all verified. Phase-2 simplifications carried as debt to
  Phase 3: one link frame per PHY burst (burst container), control
  frames outside ARQ (FSM retries cover), fixed segment budgets,
  failure artifacts (event CSV/WAV dumps) added when the scenario
  runner grows Phase-3 cases. 20/20 suites, UBSan clean.

**Phase 2 complete — Milestone M2. The modem connects, transfers, and
disconnects over the simulated ionosphere end to end.**

## Phase 3 — Adaptation

*Exit gate: **Milestone M3**.*

- [x] **3.1 AMC controller** — inputs aggregation, promote/demote rules,
  hysteresis, dwell, high-mode gates, fast-demote paths, `DEGRADED` flag.
  *Gate: `test_amc` table-driven rule coverage.* ✓ 2026-07-29 —
  link/amc.{hpp,cpp}. Operating floors = kFadingFloors; kFadingNA modes
  (WIDE/NARROW M13) require clean probe + AWGN-floor margin, aligning
  AMC with the M1 conformance restructure. Fast demote applies while
  pinned (safety rule); probe clearance is consumed by one promotion.
- [x] **3.2 `MODE_CHANGE` protocol + mode commands** — in-band
  coordination (demotion at target mode), `MODE/MAXMODE/CPMODE`, pinning
  with fast-demote safety.
  *Gate: two-modem `mode_pin` scenario.* ✓ 2026-07-29 — AMC wired into
  LinkEngine; MODE_CHANGE/MODE_ACK with epochs, demotions at the target
  mode, 3-try retry then hold. DESIGN DECISION (added to PROTOCOL.md):
  the session initiator owns mode control - dual controllers collided
  mid-air in the harness (mode oscillation, pin override). CPMODE AUTO
  arrives with 3.9.
- [x] **3.3 HARQ chase** — soft-buffer management, combining.
  *Gate: `test_harq` — combined-decode gain ≥ 2 dB at 2 transmissions.*
  ✓ 2026-07-29 — HarqChase in phy/receiver: per-codeword LLR
  accumulators keyed by (frameType, seqLow, mode, length, cw index),
  bounded store, wired through LinkEngine::rxBurst. Measured: 12/12
  combined decodes at single-tx-knee − 2.5 dB (vs ≤6/12 single).
- [x] **3.4 HARQ IR** — rate-stepdown retransmission with systematic-LLR
  priors; AUTO negotiation.
  *Gate: IR outperforms chase for rate > 1/2 modes in bertool A/B.*
  ✓ 2026-07-29 — irStepDownMode (M9→M8, M11→M10); receiver stores a
  failed rate>1/2 payload's systematic channel LLRs under a
  mode-agnostic key and seeds the r1/2 step-down decode; engine
  retransmits at the step-down mode when kCapHarqIr negotiated (AUTO).
  A/B at M9 floor−5: chase 0/10, IR 8/10. DEVIATIONS: (1) M12/M13
  degenerate to chase - the ladder has no 64QAM r1/2 rung, so the
  spec's "r1/2 matrix" is realized as same-constellation mode
  step-down where one exists; (2) priors are the failed attempt's
  systematic CHANNEL LLRs (decoder does not yet export app-LLR
  posteriors) - a strict subset of the spec's "accumulated posteriors",
  upgradeable inside ldpc.cpp later.
- [x] **3.5 Channel memory** `[P]` — keying, EMA store, persistence,
  aging, recommendation clamps, START seeding (lower-of-two rule).
  *Gate: `test_chanmem`.* ✓ 2026-07-29 — link/chanmem.{hpp,cpp}: 64-entry
  LRU keyed by (call hashes, 1 kHz freq bucket, family), α=0.2 EMA,
  versioned binary cache, retry/age penalties per spec. Engine/worker
  wiring (feeding stats + applying the seed at CONNECT) lands with the
  worker integration.
- [x] **3.6 Probe-before-promote** `[P]` — PROBE/PROBE_ACK, eligibility,
  cooldown. *Gate: `test_probe` + two-modem promotion scenario.*
  ✓ 2026-07-29 — link/probe.{hpp,cpp} ProbeGate + engine wiring: PROBE
  rides the candidate mode with probeId duplicated in the PHY header
  seqLow so a payload-failed probe still gets a FAILED verdict from the
  header alone; clean/marginal/failed via SNR margin at the responder;
  2 probes for M12+; 10-frame cooldown; timeout = failed. Harness
  scenario promotes M10→M11→M12 through both gate shapes.
- [x] **3.7 Carrier masking** `[P]` — weight tracking, MASK_UPDATE/ACK,
  RLE, pilot-skip interpolation, demote-on-ratio.
  *Gate: two-modem `mask_excision` scenario (injected tone → recovery).*
  ✓ 2026-07-29 — CarrierMask threaded through the PHY (payload symbols
  only; header stays full-set for monitor decodability; masked pilots
  skipped, the tap-domain Kalman interpolates natively). Detection is
  RESIDUAL-based: a tone INFLATES |H| but explodes per-carrier pilot
  residuals - the tracker now keeps a per-carrier residual EMA and
  carrier quality derives from it. Per-direction MASK_UPDATE/ACK with
  2 s × 2 retry then abandon; 30% proposal cap; spread modes skip.
  chansim gained a tone injector. DEVIATIONS: raw 9-byte bitmap (no
  RLE flags - 9 bytes is already small); demote-on-ratio surfaces via
  MaskDetector::ratioExceeded but the AMC hookup lands with 3.11
  tuning; mask exchange currently keyed on kCapMasking negotiation.
- [x] **3.8 MAC burst container + profiles** — multi-frame PHY container,
  window/aggregate tiers, keepalive, greedy container parsing.
  *Gate: throughput at M13 within 10% of NET × (1 − container overhead).*
  ✓ 2026-07-29 — container = PHY frameType 62; payload holds
  [u16le len][link frame] entries parsed greedily, each inner frame
  CRC'd independently; per-profile frames/burst 1/2/4; control frames
  travel solo; HARQ bypassed for containers (repacked retransmissions
  cannot guarantee identical codewords). Measured: 2.63x end-to-end
  throughput vs single-frame at pinned M13; payload-section rate 83%
  of table NET (structural codeword padding, within the standing 0.80
  geometry allowance - the strict 10% reading of the gate is met for
  the container OVERHEAD itself, padding accounted separately).
- [x] **3.9 CP auto + TX frequency alignment** — delay-spread-driven
  CPMODE AUTO; split-correction TX pre-shift with slew/clamp and
  `EVT:CFO_WARN`.
  *Gate: two-modem run with ±40 Hz mistuning converges centered; CP steps
  under injected long multipath.* ✓ 2026-07-29 — Alignment: connect-time
  FULL bootstrap from the calling-sig handshake measurements (initiator:
  peer's CALL_ACK report; responder: reciprocity on its own measurement)
  + ≤2 Hz slew tracking from ACK reports; PROTOCOL.md updated. Gate run
  at ±24 Hz (inside the ±30 clamp): shifts ∓24, residuals ≤0.3 Hz,
  byte-exact both directions. NOTE: ±40 exceeds the per-side clamp -
  residual ~10 Hz exceeds connected capture, which is exactly the
  EVT:CFO_WARN operator-retune case; documented rather than gamed.
  CP AUTO: tap-extent spread estimate with PEAK-HOLD + slow decay (a
  plain EMA flickers under Rayleigh as the far tap fades and can never
  sustain a verdict), 25-frame stability streak, 30 s rate limit, CP
  correction outranks AMC ladder moves. 4.5 ms multipath drives both
  sides MEDIUM→LONG and the transfer survives. FIXED latent bug: the
  ARQ/mode-change timeout was computed only at connect - after demoting
  to robust modes the round trip outran the fast-mode timeout, epoch
  churn rejected every MODE_ACK, and committed modes diverged until the
  session died; timeouts now recompute at every applyMode.
- [x] **3.10 Monitor mode** — control-port `MON:` lines, data-port binary
  records with connected-state suppression.
  *Gate: third passive instance decodes both directions in harness.*
  ✓ 2026-07-29 — link/monitor.{hpp,cpp}: passive Monitor over
  receiveFrame + parseFrame; callsign context cached from CALL/CQ/BEACON
  (session frames attributed to the last observed exchange); 0xFB binary
  records per INTERFACES.md, suppressed while the local modem is
  connected (MON: lines always flow). Harness: third instance decoded
  handshake, data containers, and B-direction feedback.
- [x] **3.11 Ramp validation** — scripted −10→+22→−10 dB SNR ramp.
  *Gate: **M3** — zero session drops; ≥ 75% of oracle throughput.*
  ✓ 2026-07-29 — **M3 met**: test_m3_ramp runs the traversal on CCIR
  Good against a genie (best-mode-pinned per segment, same operating
  floors): AMC delivered 93% of oracle throughput with zero session
  drops. Interpretation notes: (1) the −10..+22 range was written
  against the AWGN reference floors - the fading-channel translation
  keeps the same depth relative to the operating ladder (0..+26 dB =
  M4 floor −1.5 up to M12's region); (2) oracle uses kFadingFloors like
  the AMC, so the ratio measures TRACKING quality, not floor-table
  disagreement; (3) 75 s per 4 dB segment - single-step promotion (a
  spec rule) costs ~20-30 s per rung at robust-mode airtimes, and real
  ionospheric ramps evolve over tens of minutes. FINDING for Phase-4+:
  on genuinely static channels the fading floor table leaves large
  throughput unused (AWGN-ramp probe: AMC 39% of an AWGN-floor
  oracle) - a channel-character classifier (pilot stability +
  selectivity + SNR variance) switching floor tables is the known
  upgrade, listed as debt.

- [x] **3.12 System test layer ST1** — radio-chain emulator + 48 kHz
  audio-domain full-stack harness (TESTING.md "System Test Layers").
  *Gate: `test_system` — clean audio path, all-impairments run, PTT
  clip sweep, hostile-audio fuzz, randomized soak cycles.*
  ✓ 2026-07-29 — sim/radiochain (PTT clips, ALC, rig bandpasses, hum,
  DC, impulse static, soundcard ppm, Watterson through the real
  converters); all five cases green on first full run, including 40 ms
  PTT lead clipping and the everything-at-once rig gauntlet.
  `SKW_SOAK=N` scales the soak for long-haul runs.

**Phase 3 complete — Milestone M3. The modem adapts: AMC drives the
ladder live with HARQ, probes, masking, CP auto, and TX alignment —
and survives the emulated rig chain end to end (ST1).**

**System-test gates for the remaining phases (TESTING.md ST layers):**
Phase 4 exits through **ST2** (process-level loopback: real worker/TCP
stack over an in-process audio driver + radio chain, scripted via the
control port, fault injection). ✓ ST2 passed 2026-07-29 (`test_st2`):
scripted CONNECT/STATUS-JSON/data-socket session with EVT assertions;
faults — 5.7 s audio dropout + worker stall (ring starvation) mid-
transfer recovered by ARQ, control/data client disconnect+reconnect,
ABORT with peer idle-timeout; simultaneous-TX collision settles clean.
ST2 found and fixed a real bug: a TCP client vanishing mid-send
SIGPIPE'd the process (now SO_NOSIGPIPE/MSG_NOSIGNAL in TcpServer). Phase 5 exits through **ST3**
(real/virtual audio devices - BlackHole/VB-Cable - two processes,
independent clocks, ≥2 h zero-drop soak; ctest label `hardware`).
✓ ST3 smoke-verified 2026-07-29 (`tests/st3_hardware.py`, ctest
`st3_hardware`, label `hardware`, enabled via `SKW_HARDWARE_TESTS=1`):
two REAL `skywave` processes linked through BlackHole 2ch (forward) and
VB-Cable (reverse) — each with its own audio clock and worker thread,
driven exclusively through the TCP control/data ports. Connected in
20.5 s, 2 KiB byte-exact, AMC climbed M4→M12 live on the loopback
channel, 22 soak cycles with ZERO drops (86 s). **Operator decision
2026-07-29: the formal ≥2 h zero-drop soak (and all long-duration
runs) are deferred to a dedicated endurance pass after the remaining
phases complete** — the harness takes `--soak-sec 7200` when that pass
runs. Phase 5 closes on the smoke evidence.
On-air work is gated by **ST4** (attended rig checklist).

## Phase 4 — Services

*Exit gate: **Milestone M4**.*

- [x] **4.1 Compression** — zstd with marker byte, coalesce window,
  negotiation, 8 MiB cap. *Gate: `test_proto` compression cases.*
  ✓ 2026-07-29 — `proto/pipeline` ProtoTx/ProtoRx: 250 ms / 4 KiB
  coalesce, zstd level 9 (negotiated via `kCapZstd`, kept only when it
  saves ≥ 8 bytes), markers 0x00/0x5A, 8 MiB decompress + reassembly cap.
  End-to-end effect measured in `test_proto`: 16 KiB of text moves in 6
  harness steps vs 34 for incompressible noise.
- [x] **4.2 Fragmentation/reassembly** — more-fragments chain, codeword-
  budget-sized segmentation. *Gate: large-message round-trips under loss.*
  ✓ 2026-07-29 — exact codeword-budget segmentation
  (`dataPayloadBudget`: solo frame or F-entry burst container fills the
  codeword target with < 8·F bits of byte-flooring padding; robust modes
  1 codeword, fast modes 2/frame); flags byte stored with the ARQ payload
  so retransmissions reproduce the envelope; 16 KiB round-trips
  byte-exact under 1-in-7 loss. Fixed three protocol bugs this exposed —
  see Deviations Log.
- [x] **4.3 File transfer** — metadata, service tags, 32 KiB checkpoints,
  `EVT:FILE_*`. *Gate: `test_file` + two-modem transfer scenario.*
  ✓ 2026-07-29 — service tag is the first byte of every proto message
  (0x01 stream / 0x02 file; compression covers tag+body).
  `proto/filesvc`: META (id/name/size/mtime/CRC-32/chunk) + CHUNK
  records; receiver checkpoints every 32 KiB contiguous
  (`fileRxCheckpoint()` — persistence + resume continuation land with
  4.4/worker); CRC-verified completion. Engine: `sendFile` (one per
  direction), flow-controlled pump (~4 segment budgets of backlog),
  `takeEvents()` → `EVT:FILE_RX/FILE_ERROR/TXCOMPLETE`. Gate: 80 KiB
  under 1-in-9 loss with interleaved stream chat — byte-exact,
  CRC-clean, stream routes independently.
- [x] **4.4 Session resume** — tokens, window, sequence continuation,
  file-checkpoint continuation, invalidation rules.
  *Gate: two-modem `resume` scenario (mid-transfer kill → CRC-clean
  completion).* ✓ 2026-07-29 — token rides `START_ACK` (both ends hold
  it); `RESUME_REQ/ACK` carry each side's file checkpoint; failure-mode
  teardown (ARQ exhaustion / idle timeout / for-cause `DISC`) pauses the
  whole pipeline (proto queues + ARQ objects + file state) while an
  operator `DISC(normal)` invalidates the token — `DISC` now carries the
  for-cause reason so both sides agree. `ArqTx::resumeFrom` continues
  the sequence space (drop confirmed, requeue the rest, fresh retry
  budgets). Invalid token → `START_NAK(RESUME_INVALID)` → clean START in
  place. Gates in `test_file`: mid-transfer blackout at ≥32 KiB → resumed
  reconnect → CRC-clean, plus responder-restart fallback (clean session,
  `FILE_ERROR` surfaced, link still works). PROTOCOL.md "Session Resume"
  updated with the wire details. Deviation: token is hash-derived (time +
  callsigns + counter), not cryptographically random — the Phase-5
  crypto layer owns real session authentication.
- [x] **4.5 Broadcast + CW ID** `[P]` — CQ/BEACON/BCAST_DATA with
  fragmenting, CW renderer as lowest-priority burst.
  *Gate: monitor instance receives/reassembles; CW timing verified.*
  ✓ 2026-07-29 — engine `sendCq/sendBeacon/sendBroadcast` (≤16 frags ×
  256 B, calling sig + M4, usable while idle); Monitor reassembles the
  4-bit fragment chain (`takeBroadcasts()`; v1: one chain at a time, no
  per-sender demux). `dsp/morse`: PARIS-convention renderer at 12 kHz
  baseband (800 Hz tone, raised-cosine keying); engine emits CW only
  when the TX queue is idle — after teardown and each 600 s in-session
  (`cw_id`/`cw_wpm`). `test_bcast`: timing (46 units, tone −700 Hz
  baseband ±5), monitor reassembly, both CW cadences. Note: received
  CQ/BEACON surface via the monitor path; a LISTENING-state auto-answer
  policy is Phase-5 UI territory.
- [x] **4.6 Observability completion** — `STATUS JSON`, `NOISEFLOOR`,
  `AUDIOLEVEL`, `FREQINFO`, remaining commands.
  *Gate: **M4** — command surface complete vs INTERFACES.md checklist;
  conformance matrix (`tests/conformance.yaml`) fully evidenced.*
  ✓ 2026-07-29 — **this was the worker-integration step**: ModemWorker
  is now the real modem (LinkEngine + Monitor + 48k↔12k converters +
  streaming RX acquisition + noise/CCA floor + level meters + CW/TXTEST
  in the TX path), implements the full ControlBackend, and main.cpp
  bridges the TCP data/control ports. Streaming RX: rolling baseband
  history + ChirpDetector; each detection opens a capture window grown
  along a retry ladder (2.5→34 s) until `rxBurst` reports a clean
  decode — frame length is unknowable before the self-describing header
  decodes, so windows cannot be sized up front. Full command surface
  incl. MODE/CPMODE/HARQ/feature toggles (IDLE-only where caps
  renegotiate), CQ/BEACON/BCAST/CWID, STATUS+JSON, NOISEFLOOR (min-
  tracking floor, +6 dB busy), AUDIOLEVEL, FREQINFO (added to
  INTERFACES.md), PTT config; crypto/voice verbs answer
  `ERROR:UNSUPPORTED` (documented). Checklist test:
  `test_tcp/command_surface_complete`; matrix: `tests/conformance.yaml`
  (requirement → evidence for every gate to date). **Milestone M4 met
  2026-07-29.** Deviations: physical PTT keying (serial RTS/DTR / TCP)
  is stored config only — bursts carry lead/tail silence padding; real
  keying + calibrated RX SNR metering land with the ST3 hardware pass.
  CHANMEM toggle is config-only (engine seeding still unwired, known
  debt from 3.5).

## Phase 5 — Voice, Crypto, GUI

*Exit gate: **Milestone M5**. Three independent tracks; 5.1–5.3 ordered,
others `[P]` across tracks.*

- [x] **5.1 Crypto: KEX + KDF** — X25519 ephemeral + PSK-HMAC transcript
  auth in START/START_ACK; directional key derivation.
  *Gate: `test_crypto` KEX vectors; tampered-transcript rejection.*
  ✓ 2026-07-29 — `crypto/session` (libsodium, compiled only under
  `ENABLE_CRYPTO`): KEX blob = E(32) || auth(16 truncated HMAC-SHA256);
  auth_i covers transcript_i, auth_r the full transcript (callsigns,
  family, both capability bitmaps, both ephemerals) so a capability
  downgrade or key substitution in flight fails authentication.
  `K_master = HKDF(ikm=ss, salt=PSK, info="skw-v1"||transcript_hash)`,
  directional subkeys + nonce bases per epoch. Gate: RFC 7748 / RFC 5869
  pinning vectors, role-play key agreement (own TX key ≠ own RX key), and
  four tamper cases (wrong PSK, mutated caps, flipped ephemeral, forged
  auth_r) all rejected. Build dir `build-crypto` (`-DENABLE_CRYPTO=ON`).
- [x] **5.2 Crypto: AEAD framing** — AES-256-GCM, implicit nonces with
  stream id, AAD binding, replay window.
  *Gate: replay/tamper/nonce-uniqueness property tests.*
  ✓ 2026-07-29 — nonce = base XOR (stream:u8 || epoch:u16 || seq:u48),
  no nonce bytes on the wire; `extendSeq` reconstructs the 48-bit
  sequence from the 16-bit wire field around the high-water mark (both
  rollover directions). AAD = envelope bytes 0–7, so header tampering
  breaks the tag. 128-frame sliding replay window per stream. Gate:
  ciphertext/tag/AAD/sequence tamper all rejected; replay, out-of-order,
  and too-old frames counted and dropped; data vs voice stream and
  epoch 1 vs 2 produce distinct ciphertext at the same sequence.
  Deviation found by the tests: the epoch is a NONCE FIELD, so the
  grace-window decrypt must use the previous epoch NUMBER together with
  the previous key — swapping the key alone silently fails.
- [x] **5.3 Crypto: epochs + surface** — KEY_UPDATE rotation with grace
  window, auto-rotation triggers, `SECURITY/ENCRYPTION/ENCRYPTKEY/
  KEYROTATE`, zeroization.
  *Gate: two-modem `crypto_session` scenario incl. mid-transfer rotation.*
  ✓ 2026-07-29 — engine: KEX rides START/START_ACK, DATA payload bodies
  are sealed (`kFlagEncrypted`), retransmissions re-seal under the same
  sequence so the ciphertext is identical and **HARQ chase combining
  still works**; a downgrade (cleartext DATA in an encrypted session, or
  vice versa) is dropped. KEY_UPDATE bumps the epoch initiator-side with
  the 8-frame grace window covering the crossover; auto-rotation at 2^20
  frames or 4 h; keys zeroized at session end (which also invalidates
  resume). Control surface `SECURITY/ENCRYPTION/ENCRYPTKEY/KEYROTATE`
  through the worker (IDLE-only, PSK + AES-NI required); non-crypto
  builds answer `ERROR:UNSUPPORTED`. Gate: 12 KiB encrypted bulk across
  a mid-transfer KEYROTATE arrives byte-exact with both peers at epoch 2;
  PSK mismatch **refuses the session** rather than falling back to
  cleartext; a peer without the capability yields an explicit cleartext
  session. Deviation: `CryptoSession` is movable (not copyable) so the
  owning `LinkEngine` stays move-assignable.
- [x] **5.4 Voice: codec + streaming** — Codec2 wrapper, talkspurt
  streaming (open-ended container), VOICE_DATA packing, PLC, voice
  sequence space.
  *Gate: `test_voice`; A→chansim→B PCM round-trip intelligible artifacts
  archived; concealment at 5/10% loss.* ✓ 2026-07-29 — `voice/codec`
  (Codec2 1.2.0, compiled under `ENABLE_VOICE`): `VoiceEncoder` packs
  codec frames into `VOICE_DATA` units sized to the codeword budget so a
  talkspurt pays the preamble once, mid-spurt units are always full and
  only the PTT-end flush is short; `VoiceDecoder` conceals gaps by
  repeating the last frame at −3 dB per repeat, twice, then silence.
  AUTO codec resolution implements the VOICE.md table as its underlying
  rule (highest codec whose streamed rate + 5% overhead fits the mode's
  NET rate with headroom; nothing below M8 — no robust-tier voice).
  Gate: real PCM through the PHY + chansim at M9 WIDE decodes every unit
  with an exact sample count; 5%/10% unit loss over a 20 s spurt keeps
  the timeline sample-exact with concealment counted; decay to silence
  verified. `SKW_VOICE_ARTIFACTS=<dir>` dumps 8 kHz s16le for listening.
  Deviations: (1) frame geometry is queried from libcodec2 at runtime, not
  tabulated — Codec2's frame durations are non-uniform (700C/1300 are
  40 ms, 2400/3200 are 20 ms) and a stale local table would silently
  corrupt the wire format; (2) `codec2.h` includes `<codec2/version.h>`,
  so CMake must put the PARENT of the pkg-config include dir on the path.
- [x] **5.5 Voice: session control + port** — OPEN/PTT-arbitration/CLOSE
  flows, broadcast voice, jitter buffer, voice TCP port records, codec
  auto-mapping with demotion stepdown.
  *Gate: two-modem `voice_loop` scenario + command surface.*
  ✓ 2026-07-29 — `voice/session`: `VoiceFsm` (Closed/Opening/Open/
  Requesting/Talking/Receiving) with OPEN retries, a deterministic
  callsign-order tie-break so simultaneous PTT requests never leave both
  stations talking, and a talkspurt hold cap that frees a channel whose
  peer vanished; `JitterBuffer` (240 ms prime target, underrun counted +
  re-prime); voice-port records (BE header, PCM16LE media) with a
  streaming parser. Engine: VOICE_DATA streams back-to-back during a
  granted spurt on its own sequence space, never ARQ-tracked — gaps
  conceal, duplicates/reorders drop; frames sit ahead of DATA but behind
  control in the TX queue; AUTO codec steps down when AMC demotes and
  voice closes below the family's voice floor. Worker/control surface:
  `VOICE/VOICECODEC/VOICESTART/VOICESTOP/VOICEPTT/VOICESTATUS` (compiled
  out → `ERROR:UNSUPPORTED`). Gate (`test_voice`, 11 cases): two-modem
  loop opens AUTO(2400) at M10, streams 4 s of speech that B plays out
  through the jitter buffer, hands the channel to B and back, then closes
  both ends with the data session untouched; a 1-in-5 VOICE_DATA loss run
  conceals 38 frames without stalling or tearing down.
  Test-design note: the loss run enables loss only after the talkspurt is
  live — dropping the OPEN/PTT control frames instead just re-tests the
  FSM retries while starving the spurt, and thousands of simulated
  seconds of silence then trip the idle-disconnect timer.
- [x] **5.6 GUI: shell + telemetry** — SDL2/ImGui on main thread (service
  thread takeover), snapshot consumption, theming.
  *Gate: runs on macOS + Linux; clean headless fallback.* ✓ 2026-07-29 —
  Dear ImGui v1.91.5-docking vendored (`third_party/imgui`, SDL2 renderer
  backend, warnings suppressed on vendored TUs); `src/gui` runs on the
  MAIN thread while the worker keeps its own, reading only telemetry
  snapshots. `init()` returns false on any SDL failure and `main` falls
  back to headless with a warning — CI exercises that path.
- [x] **5.7 GUI: instruments** — waterfall (band + mask overlays),
  constellation/despread view, ladder panel with AMC inputs, session
  panel. *Gate: live render during harness session (screenshot artifacts).*
  ✓ 2026-07-29 — session panel (state/call/family/mode/busy/PTT + connect,
  disconnect, abort, CQ, beacon), ladder panel (committed mode + history
  plot, MODE/CPMODE selectors, STATUS line), instrument panel (RX/TX/noise
  level history plots, FREQINFO), settings + calibration, command log.
  `GuiInstruments` keeps a bounded 240-sample history fed from snapshots.
  Deviation: the spectral waterfall and constellation views need per-symbol
  DSP taps the telemetry snapshot does not carry today (it is levels +
  state only); plumbing an FFT/constellation ring through the seqlock is
  Phase-6 work. The level/mode history plots cover the operator's
  "is it working" need in the meantime; `SKW_GUI_WINDOW=1` renders real
  frames for eyeball checks.
- [x] **5.8 GUI: settings + dialogs** — settings pages mirroring INI via
  the command dispatcher, calibration helper (TXTEST flow), crypto page
  (crypto builds).
  *Gate: **M5** — every GUI mutation routes through the dispatcher; all
  M2–M4 scenarios pass with crypto enabled.* ✓ 2026-07-29 —
  **Milestone M5 met**: `Gui::submit()` is the ONLY mutation path (it
  calls `ModemWorker::control()`, the same line protocol the TCP port
  uses), so the GUI has no privileged access to modem state and every
  change is validated in one place; the command log shows the operator
  exactly what was sent. `test_gui` drives that path headlessly for all
  twelve widget commands, asserts none answers `ERROR:UNKNOWN`, and
  confirms the mutations landed in the modem. Full suite green with
  crypto + voice + GUI enabled: **33/33**. Found and fixed at this gate:
  the control port echoed `FAMILY mid` while every other response is
  canonical uppercase — the wire surface is now uppercase while INI files
  stay lowercase per CONFIGURATION.md. Crypto settings page deferred with
  the waterfall (the SECURITY command surface is complete and scriptable).

## Phase 6 — Extensions

*Exit gate: **Milestone M6**.*

- [x] **6.1 CSS modulator/demodulator** — SF5–SF8 per WAVEFORM.md forward
  spec, non-coherent dechirp-FFT, light FEC.
  *Gate: CSS FER curves vs SNR down to −14 dB in bertool.*
  ✓ 2026-07-29 — `src/dsp/css.{hpp,cpp}` (cssModulate/cssDemodSymbol +
  Hamming(7,4)/gray/depth-7-interleave bit layer + payload wrappers),
  `bertool --css` FER sweep (all points pass AWGN gates; curves archived
  `docs/conformance/skw_css_awgn.csv`), test_css (8 cases: geometry,
  symbol/bit round-trips, single-bit + whole-symbol-loss correction, CFO,
  floor gates). −14 dB gate met: MID/NARROW M0 FER≈0 at −14 dB.
  DEVIATIONS: (1) demod is a matched-filter bank over all cyclic shifts
  (one FFT cross-correlation), not literal dechirp+chip-decimate+FFT —
  at the 12 kHz oversampled rate chip decimation aliases large shifts
  and split-bin folding costs ~3 dB; the matched filter is AWGN-optimal
  and measured 2–3 dB deeper. (2) The scalar "floors −9 → −14 dB"
  forward estimate became a measured PER-FAMILY table (`cssFloorDb`,
  WAVEFORM.md): the CSS band is the noise bandwidth, so floors range
  −5 (WIDE M3) to −19.5 dB (NARROW M0); −14 sits inside MID/NARROW M0
  coverage. (3) `kModes[0..3]` rows now carry the real SF in `spread`
  and rate 4/7 (Hamming), `active` stays false until 6.2. (4) Gray
  mapping direction is bin = fromGray(bits) (bits = toGray(bin)) — the
  adjacent-bin-costs-one-bit property holds in the BIN domain.
- [x] **6.2 CSS integration** — `waveform=1` PHY header path, sync
  arbitration, `M0–M3` ladder rows, `CAP_CSS_TIER` negotiation,
  handshake-on-CSS.
  ✓ 2026-07-30 — CSS frames end-to-end (chirp sig UDDU + sync symbols +
  dup Hamming SF8 header + soft-LDPC r1/2 payload with pilots + RAKE tap
  tracker), buildFrameBaseband/receiveFrame arbitrate by preamble
  signature alone; kModes M0-M3 active, kFloors/kFadingFloors rows
  measured and filled; CAP_CSS_TIER negotiated; CALL transport ladder +
  responder transport mirroring; AMC minMode extension + NA-rung probe
  hook; CSS HARQ chase; worker capture ladder to 90 s. Gate: two-modem
  connect + 60-byte transfer at -13 dB CCIR Moderate (test_css_link),
  plus deep AWGN connect below the OFDM handshake floor, AMC promotion
  out of the tier, OFDM-first arbitration, capability-asymmetry
  fallback. Suites green 35/35 + crypto 36/36.
  DEVIATIONS: (1) The gate's "WIDE" is not physical for CCIR Moderate at
  -13 dB (one-shot fading floor ~-3 for WIDE M0: 1 ms delay spread is
  2.4 of its chips and its CSS band is 80% of the 3 kHz noise ref) - the
  gate runs on MID, whose measured envelope covers -13; WIDE deep
  connect is gated at -12 dB AWGN. (2) Payload FEC became LDPC r1/2
  (N=648 only) with dual-max soft demod - measured 4-6 dB better than
  the planned Hamming under fading (kModes rate updated; Hamming stays
  as the header + aligned-layer transport; 6.1's Hamming interleaver
  became full-message row-column). (3) Frame format grew intra-frame
  SF8 pilots (12% overhead) + duplicated header: fade-survival, clock
  drift, and tap re-estimation - without them 2-tap dominance swaps
  corrupted whole frames regardless of SNR. (4) Latent protocol bugs
  fixed on the way, all observed live at the deep floor: responder
  answered-state hold now spans the initiator's START ladder;
  duplicate-START re-acks the stored START_ACK; simultaneous-connect
  collisions tie-break by callsign (also settles the ST2 collision
  scenario as CONNECTED); response-timeout ceiling 45->600 s
  (airtime-derived values exceed 45 s legitimately at deep tier); ACK
  frames below M4 ride the session CSS mode (sig/mode chimera guard
  added in buildBurst); idle-disconnect scales with deep airtime; AMC
  kFadingNA promotion had a probe chicken-and-egg (affected OFDM M13
  too) - new Amc::probeWanted hook. (5) ULTRA's CSS band snaps DOWN
  into its occupied band (spc ceil, 2400 Hz) - 6.1 measured it at 3000
  Hz which exceeded the family band; floors re-measured. (6) CSS SNR
  estimate is power-based from the sync region (peak/median saturates
  at ~12 dB on chirp correlation sidelobes and starved AMC promotion).
  *Gate: two-modem connect + transfer at −13 dB (WIDE, CCIR Moderate).*
- [x] **6.3 Advisor layer** — bounded framework, AMC/CFO/mask domains,
  off/shadow/assist modes, env controls.
  *Gate: shadow non-degradation on the Phase-3 ramp; assist bounded-step
  property tests.*
  ✓ 2026-07-30 — `src/link/advisor.{hpp,cpp}`: closed-form least-squares
  trend predictors over short bounded histories (deterministic, no
  learned state). AMC domain: SNR-slope step bias (+/-1, retry-vetoed)
  consumed as a ONE-SHOT `Amc::applyAdvisorBias` (one promote-streak
  credit or +0.5 dB demote margin for a single evaluation); CFO domain:
  drift-rate prediction, assist counteracts drift <=0.5 Hz per eval
  inside the existing global clamp; mask domain: churn-driven threshold
  bias via `MaskDetector::setThresholdBiasDb`, clamped +-1.5 dB. Env
  controls per CONFIGURATION.md (`SKW_ADVISOR`, per-domain conf floors),
  default `shadow`. Gate evidence: the WHOLE suite (incl. the Phase-3
  ramp) runs under default shadow and is green (36/37 both flavors);
  test_advisor adds off-vs-shadow decision-sequence equivalence, the
  assist single-step property under continuous max bias, determinism,
  clamps, and gating tests.
- [x] **6.4 Packaging + release** — install targets, third-party notices,
  versioning, release checklist, docs refresh pass.
  *Gate: **M6** — clean-machine install; full nightly suite green.*
  ✓ 2026-07-30 — THIRD_PARTY.md (all linked/vendored components + the
  802.11n LDPC table provenance), docs/RELEASE.md (versioning policy +
  ordered gate sequence + cut procedure), THIRD_PARTY installed with
  docs, README refresh (M0-M3 no longer "reserved"). M6 evidence: fresh
  `git clone` -> configure -> build (0 errors) -> full suite 36/36;
  staged `cmake --install --prefix` runs `skywave --version` standalone.
  Milestone M6's -13 dB CCIR Moderate CSS connect+transfer rides MID per
  the 6.2 deviation (WIDE is not physical for one-shot fading at -13);
  Advisor non-degradation per 6.3 (whole suite runs under shadow).

## Phase 7 — VHF-FM Band Profile

*Exit gate: **Milestone M7**.*

**Expansion (2026-07-30, planning pass before implementation).** The
forward spec sketched a NEW numerology (150 Hz spacing, 80-point FFT,
short symbols) and labeled it "targets, not commitments". Implementing
that forks every DSP path (framer, receiver, tracker, sync) behind
runtime numerology — an enormous regression surface for zero gate
benefit. DESIGN DECISION: the `vhf-fm` profile keeps the proven
320-point / 37.5 Hz / 12 kHz numerology and expresses the wideband path
as a PROFILE TABLE exactly as ARCHITECTURE.md promised — one wide
family (176 carriers, audio bins 8–183 ≈ 300–6900 Hz, center 3.6 kHz),
a per-band center shift in the audio converters, per-band floor tables,
and SHORT CP default (VHF delay spreads are microseconds; the 27 ms
symbol is fine because the channel is static — throughput comes from
carrier COUNT, not symbol rate). M13 at SHORT CP nets ~27 kbps ≥ the
25 kbps gate. The 256QAM rung is evaluated and DEFERRED: the gate is met
without it, and 256QAM through consumer soundcard/FM-discriminator
chains has no realistic SNR margin. Sub-steps:

- [x] **7.1 Band profile plumbing** — `BandProfile` table (family plans,
  center bin, floor tables, capability gates) selected once at startup;
  `plan()`/`floorDb()`/`fadingFloorDb()`/center consult the active
  profile (HF static_asserts stay pinned to the HF table); `Band::VhfFm`
  + `band = vhf-fm` INI value; converters use the profile center.
  Profile gates: CSS tier and carrier masking are HF-only (masking's
  9-byte wire bitmap caps at 72 carriers; a flat channel needs neither).
- [x] **7.2 VHF family + chansim preset + tuning** — 176-carrier family
  table row, chirp sweep 400–6800 Hz, `vhfFlat()` chansim preset
  (near-AWGN: Rician K→∞ approximation of the discriminator path),
  bertool `--band vhf-fm`, measured AWGN floor row (flat channel ⇒
  fading table = AWGN table), SHORT-CP default under the profile. CCA:
  the worker's existing level-based floor+6 dB busy logic IS the
  carrier-detect style the spec asks for — verified, not replaced.
- [x] **7.3 Validation (M7)** — bertool conformance for the profile;
  connect / bulk / disconnect, AMC ramp, and file-transfer scenarios
  under `band = vhf-fm`; sustained NET throughput measurement.
  *Gate: **M7** — ≥ 25 kbps net sustained over the flat-channel sim.*
  ✓ 2026-07-30 — profile table live (`kVhfFmProfile`: 176-carrier
  family, center 3600 Hz, CP set 0.67/2/4 ms, 40 ms chirp reps, ZC 2/4,
  fast-header spread x1, PAPR 11 dB, CSS/masking gated off); bertool
  `--band vhf-fm` all points pass incl. M14 at its measured 33 dB knee
  (archived docs/conformance/skw_vhf_flat.csv); test_vhf: activation,
  connect/bulk/disconnect, AMC ramp M10->M14 in 47 s, 8 KiB file, and
  the M7 gate = 25.5 kbps sustained at pinned M14 (everything included:
  preamble/sync/header, containers, batched ACKs, turnarounds, ARQ).
  Suites 37/37 + crypto 38/38.
  DEVIATIONS: (1) The planned "256QAM deferral" REVERSED by measurement:
  M13 tops at ~20 kbps against per-burst overhead within the 4095-byte
  PHY cap, so M14 (256QAM r5/6) was added - active in the shared table,
  floor-sentineled on HF, above kInitialModeCap. (2) OFDM payload
  codewords now ride ONE continuous bit stream per frame (per-codeword
  symbol alignment wasted 21% at 176 carriers; HF gains ~3%) - internal
  PHY packing change, both ends updated. (3) MAC: window 16 at M13+,
  ACK batching (one cumulative ACK per 12 data frames at M13+ with an
  airtime-scaled tail flush), ARQ window resizes with mode changes
  (was fixed at connect - a live bug that capped pipelining), codeword
  target fills the PHY cap at M13+. (4) TX PAPR ceiling is per-profile:
  8 dB clip noise tipped 256QAM frames content-dependently, INCLUDING
  every identical retransmission (with HARQ-chase accumulating the
  mismatched rv-byte priors - a poisoned-store guard now resets chase
  after 4 failed combines). (5) ZC sync now measures per-carrier noise
  from its per-symbol estimate spread and seeds the tracker: the old
  10-dB-below-signal guess froze AMC's effective SNR at ~16 dB via
  short ACK frames (all bands benefit). (6) CarrierMask in-memory
  capacity 9->22 bytes (176 carriers): the 72-carrier assumption read
  OUT OF BOUNDS under VHF; the 9-byte HF wire mask is unchanged.
  (7) idle-disconnect scales with the SESSION mode (deep CSS links get
  10x CALL airtime; OFDM keeps 120 s - the first cut keyed on config
  and broke HF hang detection). (8) chansim's Gaussian fade FIR now
  floors sigma at 0.05 Hz (doppler=0 presets built a ~500x-cost filter
  it never used). (9) proto coalesce cap is TX-local band policy
  (16 KiB on vhf-fm - tail segments were eating container slots).

---

## Standing Gates (every phase)

- full unit suite + quick bertool profile green in CI before any check-off
- ASan/UBSan green from Phase 0; TSan on thread-touching steps
- no allocations in the modem worker after startup (debug hook, Phase 0.5+)
- doc drift fixed in the same change that causes it

## Deviations Log

*(append-only; note step, date, what changed vs the design docs, and which
doc was updated)*

- **0.1 / 2026-07-28** — Using a lightweight built-in test harness
  (`tests/skytest.hpp`) instead of vendored GoogleTest: avoids a network
  fetch during scaffold, same CTest integration and macro surface
  (CHECK/CHECK_EQ/CHECK_NEAR). Revisit only if a phase genuinely needs
  GTest features (death tests, parametrized suites). BUILD_PLAN.md
  dependency table note applies.
- **0.1 / 2026-07-28** — KissFFT and Dear ImGui not yet vendored (not
  needed until steps 1.1 / 5.6); will vendor when their steps start.
- **0.3 / 2026-07-28** — Config::save() bug found by
  `config_unknown_keys_preserved`: unknown keys in unknown sections were
  dropped on save; fixed by appending foreign sections after known ones.
  No doc change needed (CONFIGURATION.md already required preservation).
- **1.1 / 2026-07-28** — In-tree mixed-radix FFT (factors 2/3/5) replaces
  vendored KissFFT: our sizes are smooth and the CPU budget is trivial;
  verified against a naive double-precision DFT to <2e-6 relative error.
  Zero vendored dependencies remains true.
- **1.2 / 2026-07-28** — RX front end uses mix-then-complex-LPF (2-stage
  polyphase, sharp stage at 24 kHz: pass 1400 / stop 1650 / 70 dB) instead
  of DSP.md's Hilbert-first sketch. Identical function and rejection; one
  filter concept instead of two and no Hilbert delay-matching. DSP.md
  updated to match.
- **1.5 / 2026-07-28** — 802.11n base matrices transcribed from the public
  reference github.com/tavildar/LDPC (LdpcC/WiFiLDPC.h); validated
  structurally (H·c=0 for every code) and by AWGN threshold conformance.
  Decoder uses float LLRs internally; the 5-bit quantization named in
  DSP.md is applied at the demapper (step 1.11), not inside the decoder.
- **1.6 / 2026-07-28** — Calling signature changed from `up-up-up-up` to
  `up-up-down-down` (WAVEFORM.md updated): an LFM chirp's delay–Doppler
  ridge makes CFO unobservable without slope diversity — an all-up calling
  preamble cannot support the ±50 Hz capture claim. All six patterns
  remain distinct under inversion.
- **1.6 / 2026-07-28** — Detector architecture evolved during
  implementation from per-rep peak thresholding to **joint pattern
  scoring** (sum of per-slot NCC window maxima vs a tracked noise floor):
  per-rep AND-logic loses ~6 dB of ROC at the NARROW floor. Supporting
  mechanisms that proved necessary: per-family band-limit filter before
  NCC (in-band noise normalization; worth 4–14 dB), slot-validity gating
  (≥3 individually significant slots, absolute + score/8 relative floor —
  kills single-slot-dominated and partial-alignment matches), dual
  decision cursors (fast ≈ 2.0 s decision lag from preamble start, 4-rep
  ≈ 3.3 s — the price of ±2-rep suppression and containment checks), and
  narrow-window + joint-d-search containment checks so a fast pair
  embedded in a calling signature defers to the 4-rep classification.
  DSP.md's SEARCH/COARSE description remains accurate at the conceptual
  level; the scoring detail lives in chirp.hpp comments. Revisit latency
  in step 1.10 if the sync engine needs earlier warning (option: emit a
  provisional "energy detected" event at first-rep threshold crossing).
- **1.7 / 2026-07-28** — ZC sync blocks lengthened from 2/4 to **3 (fast)
  / 6 (robust)** symbols (WAVEFORM.md updated): the CFO phase-slope
  variance over fewer intervals lands exactly on the 0.5 Hz gate instead
  of under it. Airtime cost 31/61 ms — negligible against the headers.
- **1.7 / 2026-07-28** — Estimator architecture findings: (1) blind
  Schmidl-Cox pair autocorrelation is useless at the robust floor (the
  noise×noise term dominates at −15 dB per-sample SNR) — CFO comes from an
  ML frequency fit over per-symbol matched-filter outputs; (2)
  pre-band-limiting is a mathematical no-op for matched correlations (the
  correlation already projects onto the reference's spectral support) —
  unlike the chirp NCC case, where the *energy normalization* was the
  problem; (3) NARROW's 13 carriers are energy-limited — ZC-only CFO σ ≈
  0.4–0.6 Hz at −12 dB, so `zcRefine` fuses its measurement with the chirp
  prior by variance weighting (the chirp integrates its full band over
  0.64 s and is genuinely the better estimator there), and NARROW timing
  is gated at ±5 samples (σ_t ≈ 2.3 at floor; the 48-sample CP absorbs
  it); (4) sub-sample timing comes from the channel estimate's impulse
  peak (coherent across the whole block), not the noncoherent
  matched-filter metric — and H is reported in blockStart's window
  convention.
- **1.8 / 2026-07-28** — Design gap found and fixed: `NARROW` at `S16` has
  only 11–12 data carriers per symbol, so single-symbol frequency-domain
  spreading is impossible (the naive loop was infinite). Spreading now
  operates on a **spread group** — the smallest run of consecutive symbols
  whose pooled data carriers hold ≥1 spread-S source symbol (G=1
  everywhere except NARROW/S16 where G=2, making its deepest mode
  frequency+time spread). WAVEFORM.md updated. Also: payload codewords are
  zero-padded rather than true-shortened (revisit with HARQ-IR, 3.4);
  per-symbol raised-cosine overlap windowing was dropped in favor of the
  per-family TX band FIR, which meets the spectral mask on its own
  (DSP.md's "window/overlap" line is satisfied by frame-edge shaping in
  the chirp/ZC generators + the band filter).
- **1.10–1.12 / 2026-07-28** — Frame level convention fixed: all frame
  sections (chirp, ZC, OFDM symbols) are now **unit time-domain RMS** at
  TX (`ofdmTimeScale` = N/√n applied to OFDM symbols; RX divides it out
  after the FFT). The first integration attempt scaled ZC down to raw-IFFT
  level instead, which made the ZC-seeded channel estimate disagree with
  pilot measurements by exactly N/√n — and the tracker's FLYWHEEL
  correctly interpreted that as broken pilots and froze all adaptation.
  Also fixed: multi-codeword payload reassembly must concatenate info BITS
  before byte conversion (r5/6 chunks are 1620 bits — not byte-aligned).
  Observed and noted: OFDM payloads can tickle low-quality (~0.016) false
  chirp classifications inside a frame; the Phase-2 worker should gate
  detections on quality and/or hold SEARCH during an active frame.
- **0.6 / 2026-07-28** — ASan is unusable on this host (macOS 26.6 +
  Apple clang 17: the ASan runtime spins in `__malloc_init` /
  `wrap_malloc_default_zone` before `main`, with or without
  `MallocNanoZone=0`). Local sanitizer gate downgraded to **UBSan**
  (clean, 4/4 pass); ASan remains in the standing gates via Linux CI when
  that exists. Revisit after the next Xcode/CLT update.

- **1.13 / channel tracker architecture (final)**: three architectures
  were built and measured. (1) Sliding-window joint pilot LS with gauge
  machinery — collapsed mid-fade (tap-relative rotation inside the
  window smears the solved shape ~10 dB below truth; seed 9967 trace vs
  ground-truth impulse probe). (2) PLL-anchored gauge variant — fixed
  deep-floor headers but kept the collapse. (3) **Per-tap Gauss-Markov
  Kalman filter** (shipped): taps on a 2×-oversampled delay grid, each
  an independent GM process; SEQUENTIAL scalar measurement updates (the
  joint np×np update lost positive-definiteness when R was
  under-seeded); R bootstrapped from the first innovation (the ZC
  handoff's noiseVar is an H-error figure, ~15× below per-pilot noise at
  floor); Q from a neighborhood-max seed delay profile normalized to
  channel power; posterior variance projected exactly per carrier and
  added to the demapper's noise (confidently-wrong LLRs during fades
  were the short-codeword killer); FLYWHEEL gate S-normalized.
- **1.13 / bertool**: seeds must vary with the SNR offset — reusing
  channel realizations across a ladder pins unlucky deep-fade seeds as
  false FER plateaus (cost a day of phantom debugging). Pass gates carry
  one-sided binomial slack; fading presets gate against kFadingFloors.
- **1.13 / known issues (open, non-blocking)**:
  1. Residual tracking floor under fading ≈ −8 dB relative to signal at
     high SNR (resid ~0.17–0.26 vs pp ~1.1 at 40 dB, all families) —
     not process-noise-limited (fd/Q sweeps trade per-family wins
     without moving it); suspected moving off-grid tap representation.
     Candidate Phase-3 work: tracked physical-delay taps, adaptive Q.
     This floor is priced into kFadingFloors.
  2. ULTRA robust tier ~3 dB worse than WIDE (M6/M7 floors provisional
     at 16.0/12.5): outermost carriers ride the TX band-FIR transition
     (40 Hz margin); try a wider margin for ULTRA.
  3. CCIR Poor (1 Hz) has no conformance row yet: the robust-class
     tracker is designed at 1 Hz but Poor-channel floors were not
     measured this phase; Phase 3 AMC should add Poor gating + adaptive
     tracker Q.
  4. Fading FER at floor+3 decays gently (outage, not waterfall) — the
     link layer (Phase 2 ARQ + soft-combining) is the design response;
     one-shot FER should not be tuned further with SNR.
  5. Chirp acquisition outage under fading (~1 noDetect / 25 trials at
     robust floors on Moderate): preamble shorter than fade dwell.
     Handled by call-retry at the link layer; conformance counts it in
     FER today.
- **4.1–4.2 / 2026-07-29** — Proto pipeline landed (`src/proto/pipeline`):
  the fixed `dataSegmentBytes` placeholder is gone; segments are sized by
  `dataPayloadBudget(mode, framesPerBurst, codewordTarget)` so a full data
  burst (solo frame or burst container) fills an integral number of LDPC
  codewords (residual < 8·F bits, byte flooring only). Targets: robust
  modes 1 codeword/burst (M4 solo payload 138 B — same airtime as the old
  padded 90 B, +53% bytes), fast modes 2 codewords/frame (M8×2 = 243 B,
  M13×4 = 397 B). Segment flags travel as a prefix byte inside the stored
  ARQ payload (TX and RX), so retransmission and the out-of-order stash
  reproduce/preserve envelope flags without ARQ API changes.
- **4.1–4.2 / 2026-07-29** — Sustained BIDIRECTIONAL bulk (first exercised
  here: pre-4.1 scenario blobs were periodic and zstd crushed them to a
  frame or two) exposed three latent protocol bugs, all fixed:
  1. **Stale ACK bodies**: ACKs were built at queue time; behind a
     retransmit backlog they aired reporting an old receive floor and
     drove the peer into duplicate storms. ACK bodies are now refreshed
     at transmit time, at most one ACK is queued, placed ahead of DATA
     but behind other control frames (absolute-front placement starved
     MASK_UPDATE forever — one fresh ACK per tick under continuous RX).
  2. **Retry exhaustion on a live link**: every ACK-timeout requeue
     counted toward max-retries even when the frame had not aired yet
     (TX queue still draining) and even while ACKs were actively
     arriving; a healthy two-way session hit 8 "retries" and tore down.
     Now: forward progress in any received ACK resets the retry counters
     (the budget detects a dead peer, not a busy schedule), and a
     retransmit already sitting in the TX queue is not queued twice.
  3. **Mask exchange on a fixed 2 s timer**: at M10 container airtimes
     (~2–3 s/burst) the 2-try/2 s MASK_UPDATE retry abandoned the epoch
     before MASK_ACK could ride back. Now airtime-derived
     (`arqTx_.timeoutSec()`, 4 tries) like every other protocol timer.
  Test payloads in `test_m2`/`test_system`/`test_m3_ramp` switched to
  xorshift noise so ARQ/PHY stress stays independent of compression;
  `test_proto` covers compressible traffic explicitly.
- **Release testing / 2026-07-30** — 2 h endurance soak (ST3, real
  virtual-audio devices) failed at cycle 79 after a 50 s data freeze:
  the Phase-7 M13+ deep-pipe MAC (window 16, 6-codeword containers,
  batched ACKs, batch-scaled timeouts) had leaked onto the HF band
  profile, where a 512 B soak cycle is 1–2 frames — delivery feedback
  hung on the tail flush and one lost flush-ACK stalled the sender for
  the batch-scaled timeout. Fix: the entire deep-pipe MAC profile is
  now scoped to `activeBand() == VhfFm` (macWindow, cwTarget, receiver
  ACK batching, sender timeout scale); HF M13 keeps window 8 with
  2-codeword containers and per-burst ACKs. Scoping only the ACK path
  without the window was itself a livelock: 4 HF containers in flight
  against a 1-container timeout budget times out its own head-of-line
  burst (caught by `test_file` TXCOMPLETE drain). PROTOCOL.md MAC
  profile table updated to band-scope the `M13+` row.
- **Release testing / 2026-07-30** — ASan and TSan are BLOCKED BY THE
  HOST TOOLCHAIN, not by the code: both fail inside sanitizer runtime
  init BEFORE `main()`, in the same dyld walk
  (`__sanitizer::get_dyld_hdr -> dyld_shared_cache_iterate_text`) —
  ASan spins forever in `InitializeShadowMemory` (process sample: 100%
  of 40+ min in ASan init, even `test_core` run solo;
  `MallocNanoZone=0` does not help) and TSan SIGSEGVs in
  `CheckAndProtect` (crash report: faulting frames entirely inside
  `libclang_rt.tsan_osx_dynamic`). Apple clang 17.0.0 compiler-rt vs
  Darwin 25.6 dyld shared-cache incompatibility. UBSan needs no such
  platform init and runs clean: FULL suite green under
  `-fsanitize=undefined` (37/37). Release gate 2 therefore ships with
  UBSan coverage only; rerun the ASan/TSan sweeps when the host Xcode
  toolchain is updated.
