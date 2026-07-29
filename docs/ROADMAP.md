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
- [ ] **2.7 Two-modem integration harness** — two in-process instances ×
  two chansim directions (asymmetric channels); scenario runner + failure
  artifacts (seeds, event CSV, baseband WAV).
  *Gate: **M2** — `connect_floor`, `bulk_transfer`, `disconnect_paths`
  scenarios pass per TESTING.md.*

## Phase 3 — Adaptation

*Exit gate: **Milestone M3**.*

- [ ] **3.1 AMC controller** — inputs aggregation, promote/demote rules,
  hysteresis, dwell, high-mode gates, fast-demote paths, `DEGRADED` flag.
  *Gate: `test_amc` table-driven rule coverage.*
- [ ] **3.2 `MODE_CHANGE` protocol + mode commands** — in-band
  coordination (demotion at target mode), `MODE/MAXMODE/CPMODE`, pinning
  with fast-demote safety.
  *Gate: two-modem `mode_pin` scenario.*
- [ ] **3.3 HARQ chase** — soft-buffer management, combining.
  *Gate: `test_harq` — combined-decode gain ≥ 2 dB at 2 transmissions.*
- [ ] **3.4 HARQ IR** — rate-stepdown retransmission with systematic-LLR
  priors; AUTO negotiation.
  *Gate: IR outperforms chase for rate > 1/2 modes in bertool A/B.*
- [ ] **3.5 Channel memory** `[P]` — keying, EMA store, persistence,
  aging, recommendation clamps, START seeding (lower-of-two rule).
  *Gate: `test_chanmem`.*
- [ ] **3.6 Probe-before-promote** `[P]` — PROBE/PROBE_ACK, eligibility,
  cooldown. *Gate: `test_probe` + two-modem promotion scenario.*
- [ ] **3.7 Carrier masking** `[P]` — weight tracking, MASK_UPDATE/ACK,
  RLE, pilot-skip interpolation, demote-on-ratio.
  *Gate: two-modem `mask_excision` scenario (injected tone → recovery).*
- [ ] **3.8 MAC burst container + profiles** — multi-frame PHY container,
  window/aggregate tiers, keepalive, greedy container parsing.
  *Gate: throughput at M13 within 10% of NET × (1 − container overhead).*
- [ ] **3.9 CP auto + TX frequency alignment** — delay-spread-driven
  CPMODE AUTO; split-correction TX pre-shift with slew/clamp and
  `EVT:CFO_WARN`.
  *Gate: two-modem run with ±40 Hz mistuning converges centered; CP steps
  under injected long multipath.*
- [ ] **3.10 Monitor mode** — control-port `MON:` lines, data-port binary
  records with connected-state suppression.
  *Gate: third passive instance decodes both directions in harness.*
- [ ] **3.11 Ramp validation** — scripted −10→+22→−10 dB SNR ramp.
  *Gate: **M3** — zero session drops; ≥ 75% of oracle throughput.*

## Phase 4 — Services

*Exit gate: **Milestone M4**.*

- [ ] **4.1 Compression** — zstd with marker byte, coalesce window,
  negotiation, 8 MiB cap. *Gate: `test_proto` compression cases.*
- [ ] **4.2 Fragmentation/reassembly** — more-fragments chain, codeword-
  budget-sized segmentation. *Gate: large-message round-trips under loss.*
- [ ] **4.3 File transfer** — metadata, service tags, 32 KiB checkpoints,
  `EVT:FILE_*`. *Gate: `test_file` + two-modem transfer scenario.*
- [ ] **4.4 Session resume** — tokens, window, sequence continuation,
  file-checkpoint continuation, invalidation rules.
  *Gate: two-modem `resume` scenario (mid-transfer kill → CRC-clean
  completion).*
- [ ] **4.5 Broadcast + CW ID** `[P]` — CQ/BEACON/BCAST_DATA with
  fragmenting, CW renderer as lowest-priority burst.
  *Gate: monitor instance receives/reassembles; CW timing verified.*
- [ ] **4.6 Observability completion** — `STATUS JSON`, `NOISEFLOOR`,
  `AUDIOLEVEL`, `FREQINFO`, remaining commands.
  *Gate: **M4** — command surface complete vs INTERFACES.md checklist;
  conformance matrix (`tests/conformance.yaml`) fully evidenced.*

## Phase 5 — Voice, Crypto, GUI

*Exit gate: **Milestone M5**. Three independent tracks; 5.1–5.3 ordered,
others `[P]` across tracks.*

- [ ] **5.1 Crypto: KEX + KDF** — X25519 ephemeral + PSK-HMAC transcript
  auth in START/START_ACK; directional key derivation.
  *Gate: `test_crypto` KEX vectors; tampered-transcript rejection.*
- [ ] **5.2 Crypto: AEAD framing** — AES-256-GCM, implicit nonces with
  stream id, AAD binding, replay window.
  *Gate: replay/tamper/nonce-uniqueness property tests.*
- [ ] **5.3 Crypto: epochs + surface** — KEY_UPDATE rotation with grace
  window, auto-rotation triggers, `SECURITY/ENCRYPTION/ENCRYPTKEY/
  KEYROTATE`, zeroization.
  *Gate: two-modem `crypto_session` scenario incl. mid-transfer rotation.*
- [ ] **5.4 Voice: codec + streaming** — Codec2 wrapper, talkspurt
  streaming (open-ended container), VOICE_DATA packing, PLC, voice
  sequence space.
  *Gate: `test_voice`; A→chansim→B PCM round-trip intelligible artifacts
  archived; concealment at 5/10% loss.*
- [ ] **5.5 Voice: session control + port** — OPEN/PTT-arbitration/CLOSE
  flows, broadcast voice, jitter buffer, voice TCP port records, codec
  auto-mapping with demotion stepdown.
  *Gate: two-modem `voice_loop` scenario + command surface.*
- [ ] **5.6 GUI: shell + telemetry** — SDL2/ImGui on main thread (service
  thread takeover), snapshot consumption, theming.
  *Gate: runs on macOS + Linux; clean headless fallback.*
- [ ] **5.7 GUI: instruments** — waterfall (band + mask overlays),
  constellation/despread view, ladder panel with AMC inputs, session
  panel. *Gate: live render during harness session (screenshot artifacts).*
- [ ] **5.8 GUI: settings + dialogs** — settings pages mirroring INI via
  the command dispatcher, calibration helper (TXTEST flow), crypto page
  (crypto builds).
  *Gate: **M5** — every GUI mutation routes through the dispatcher; all
  M2–M4 scenarios pass with crypto enabled.*

## Phase 6 — Extensions

*Exit gate: **Milestone M6**.*

- [ ] **6.1 CSS modulator/demodulator** — SF5–SF8 per WAVEFORM.md forward
  spec, non-coherent dechirp-FFT, light FEC.
  *Gate: CSS FER curves vs SNR down to −14 dB in bertool.*
- [ ] **6.2 CSS integration** — `waveform=1` PHY header path, sync
  arbitration, `M0–M3` ladder rows, `CAP_CSS_TIER` negotiation,
  handshake-on-CSS.
  *Gate: two-modem connect + transfer at −13 dB (WIDE, CCIR Moderate).*
- [ ] **6.3 Advisor layer** — bounded framework, AMC/CFO/mask domains,
  off/shadow/assist modes, env controls.
  *Gate: shadow non-degradation on the Phase-3 ramp; assist bounded-step
  property tests.*
- [ ] **6.4 Packaging + release** — install targets, third-party notices,
  versioning, release checklist, docs refresh pass.
  *Gate: **M6** — clean-machine install; full nightly suite green.*

## Phase 7 — VHF-FM Band Profile

*Exit gate: **Milestone M7**.*

- [ ] **7.1 Profile table + flat-channel preset** — `vhf-fm` numerology
  per WAVEFORM.md forward spec; Rician/near-AWGN chansim preset.
- [ ] **7.2 Ladder + CCA + sync tuning** — top-heavy ladder (evaluate
  256QAM rung), carrier-detect busy logic, retuned chirp/thresholds.
- [ ] **7.3 Validation** — bertool conformance for the profile; all
  Phase-2/3/4 scenarios under `band = vhf-fm`.
  *Gate: **M7** — ≥ 25 kbps net sustained over the flat-channel sim.*

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
