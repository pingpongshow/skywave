# Gap Closure Plan — fading, frequency tolerance, kill/resume (2026-10)

This plan covers the three gaps that stand between Skywave and an on-air comparison with VARA HF:

1. **Fading robustness.** WIDE never reaches 10% FER on CCIR Poor, and the fast tier has error floors on Moderate.
2. **Frequency tolerance once connected.** A 5 Hz retune, or 0.02 Hz/s of drift, kills a session.
3. **Kill/resume.** The `st3_killresume` gate is intermittent.

Every item below comes from a measured root cause, not a hypothesis. The investigation was done on copies of the tree. Prototypes, CSVs and logs are archived in [investigations/2026-10-gap/](investigations/2026-10-gap/). The execution checklist is **Phase 8** in [ROADMAP.md](ROADMAP.md#phase-8--gap-closure-fading-cfo-killresume). This document is the reference behind those checkboxes.

## Status (2026-10-11)

8.0-8.7 landed; per-step results, gate numbers and deviations are in the
ROADMAP Phase 8 entries, final sweeps in `conformance/gap_final/`
(compared against `gap_base/`). 8.8 (CSS drift) was re-planned after two
measured designs failed on fading - see its ROADMAP entry. 8.9
(re-measure floors, recalibrate) and 8.10 (release gates) remain.

## Rules for this phase

- **One root cause per step.** Measure before and after on the same command lines (given per step). A step is done only when its gate passes and `ctest -LE hardware` is green.
- **Receiver-only unless stated.** No step changes `kWireVersion` (1) or `kProtocolVersion`. Patched and unpatched peers must interoperate, and each step's gate includes a check that old TX + new RX decodes.
- **Each new behaviour gets an env knob to disable it** (`SKW_*=0`, project convention) for A/B and rollback. The default is ON once the gate passes.
- **If a measurement contradicts a prediction here, stop.** Record it in the ROADMAP Deviations Log and re-plan that step. Do not stack further changes on an unexplained result.
- **Floor tables are re-measured once, at the end (8.9), not per step.** Intermediate steps are judged on bertool FER at fixed SNRs.

## Root causes (evidence summary)

### R1 — Chirp CFO ambiguity: wrong branch (fast frames, and robust frames under multipath)
- **Mechanism, `src/dsp/chirp.cpp:467-501`.** Fine CFO comes from same-slope inter-rep phase, which is ambiguous modulo `1/(gap·T)`. It is unwrapped against a coarse estimate taken from the up/down peak-time difference.
  - **Fast signature (U-U).** There are no down reps, so the coarse estimate is 0. Real capture is therefore **±3.125 Hz**, not the documented ±5. Measured: M8/M11 frames go 0% → 100% FER between 3.0 and 3.5 Hz on every family (`cfo/static_awgn6.csv`).
  - **Robust signature (U-D-U-D) under multipath.** When the two paths swap dominance between up and down reps, the 2 ms delay converts to a coarse-CFO error. On WIDE's 15 kHz/s sweep that error is ~15 Hz, against ~6 Hz on MID and ~3 Hz on NARROW. Fine CFO then lands on a wrong branch, 4.9–8.9 Hz off.
    - This is **the entire WIDE/Poor header-failure outage**. A genie-CSI receiver still fails 40–47% because the CFO is wrong, not the channel estimate.
    - It refutes the "channel decorrelates within the frame" explanation in `conformance/poor_findings.md`.
- **The ZC stage cannot repair either case.** The ML fit is limited to ±3 Hz (`src/dsp/zc.cpp:157`), and the chirp prior is trusted with σ = 0.7 Hz (`src/phy/receiver.cpp:626`).
- The fast-signature start estimate also has a delay-Doppler timing bias of `c·T/B·fs` samples: 0.8/Hz WIDE, 1.9 MID, 4.3 NARROW.
- `kCfoCaptureConnHz/CallHz/SearchStepHz` (`constants.hpp:102-104`) are **dead constants**. The "peak-shift disambiguation" in WAVEFORM.md:297 does not exist.

### R2 — No in-frame frequency tracking
- `SymbolWalker` derotates each frame by one constant (`receiver.cpp:78`). `ChannelTracker` has no frequency state; `phase_` is telemetry only.
- The "2nd-order PLL" in DSP.md:78 and `equalizer.hpp:18` is not implemented.
- Measured: NARROW dies at 0.1 Hz/s; WIDE M8 fails at 1 Hz/s. The failure point is ~1–1.5 Hz of accumulated in-frame drift.

### R3 — Session TX alignment can't follow real offsets
`onCfoReport` (`src/link/engine.cpp:1724`):
- **Slew is too slow.** ≤2 Hz per report means a 40 Hz step takes ~180 s.
- **The clamp is too tight.** It holds at ±30 Hz, so a 45 Hz offset leaves 15 Hz of residual and the link dies.
- **One-way flows are never corrected.** Reports ride only on ACK/CALL_ACK, so the data receiver's TX is never corrected in one-way bulk.
- **The warning can never fire.** `EVT:CFO_WARN` fires at ≥40 Hz (`worker.cpp:940`), but the shift is clamped at 30.
- **The docs are wrong.** "Split correction" (PROTOCOL.md:463) is really a loop gain of 0.5.

ST2 end to end (`cfo_st2`, 20 dB AWGN):
- A step of 5, 10, 20 or 40 Hz gives 0 bytes afterwards, then a drop.
- Drift of 0.02, 0.05 or 0.1 Hz/s kills the link at 163, 72 or 39 s.
- The link never recovers, because demotion needs feedback that cannot decode.

### R4 — FFT window anchor and tap grid miss a weak early path
- `zc.cpp:290-304` anchors on the earliest tap that reaches ≥½ peak power, with a 2-sample backoff.
- The tracker delay grid starts only 5 samples before the window (`equalizer.cpp:21`, `(t-2)*spacing`).
- An early path that is weak at ZC time (~⅓ of frames) later sits off-grid at −12 samples (Moderate) or −24 (Poor). This produces the **SNR-independent fast-tier floors**: Moderate WIDE M8 15%, M11 44%.
- Static two-path 1 ms, fast tracker: estimation error goes from **+7.7 dB to −3.6 dB** relative to noise with backoff 16 and grid k0 = 5.

### R5 — Kalman Q scaled to channel power at ZC time
- `equalizer.cpp:99` seeds the process noise from the ZC-time channel power. A frame that starts in a fade gets Q 10–20 dB too small and cannot follow the recovery.
- Incremental gain from normalising Q: Moderate M8 @25 dB 12% → 4%; M11 @32 dB 20% → 9%.

### R6 — Causal-only tracking
- The whole frame is buffered, but the tracker runs forward only.
- A fixed-interval RTS smoother gives: Moderate M8 4% → 1%; Poor M8 23% → 11%; channel-estimate error −11 → −15 dB on robust Poor.
- Cost: +5–40 ms CPU per frame.

Not losses, measured and ruled out:
- Demapper LLR/noise weighting (~0 dB).
- The Watterson simulator (`src/sim/chansim.cpp:60-80` is correct: Gaussian Doppler, 2σ spread, independent taps).

### R7 — Kill/resume: the chirp detector floor collapses in exact digital silence, and audio is dropped
- **The detector floor collapses.** `ChirpDetector::evaluateGridPoint` (`chirp.cpp:197-207`) feeds NCC = 0 from zero-energy windows into `emaNoiseNcc_`. The floor decays geometrically (4.9e-82 after 60 s). All thresholds are multiples of it, so after ~30 s of silence a **phantom detection fires every ~0.65 s**.
  - Virtual cables produce exact silence whenever the peer is dead.
  - A real radio never does, but a muted, squelched or VOX'd input can.
- **Phantoms flood the capture ladder.**
  - Phantoms climb the full 90 s capture ladder at 0.2–2.8 s CPU per attempt.
  - The cap of 8 evicts real frames after ~6 s.
  - The worker falls behind real time (RTF 1.02).
- **The RX ring overruns silently.** The ring is 1 s (`main.cpp:145`), and `SampleRing::write` drops the overflow without counting it. Baseline run r3 lost **82–195 s of audio**. A handshake frame lands in a dropped stretch, and which one depends on timing, hence intermittent.
- **The "decode-latency race on START" hypothesis is refuted.** In r3, B decoded three RESUME_REQs and answered each, and A never heard a single NAK.
- **Why ST2 never reproduced it:** ST2 adds a −60 dBFS ambient floor, and its FakeClock hides CPU cost.

## Execution order and gates (Phase 8)

The order follows risk and dependency. 8.1 is independent and already proven. 8.2 fixes both the CFO cliff and the Poor outage. 8.3–8.5 stack on 8.2's correct CFO. 8.6 is measured on top of the tracker changes because it interacts with them.

Standard command lines (the `bertool` built from the step's tree; `--cfo/--drift` are added in 8.0):
```
AWGN_GATE:  for F in wide mid narrow: bertool --preset awgn --families F --offsets 0,1,3 --trials 100
MOD_FAST:   bertool --preset moderate --families wide,mid,narrow --modes 4,6,8,11 --offsets 6,12,20 --trials 200
POOR:       bertool --preset poor --families wide,mid,narrow --modes 4,6,8 --offsets 0,6,12,20 --trials 200
CFO_SWEEP:  bertool --preset awgn --offsets 6 --modes 4,7,8,11 --families wide,mid,narrow --trials 50 --cfo C   (C ∈ 0,±3.5,±10,±20,±30,±50)
DRIFT:      bertool --preset awgn|moderate --offsets 6 --modes 4,7,8,11 --trials 50 --drift D (D ∈ 0.1,0.5,1,2)
```

### 8.0 Instruments and baseline (no behaviour change)
- **Add instruments.**
  - `ChansimParams::cfoDriftHzPerSec`.
  - bertool `--cfo`, `--drift`, and a CFO-error column.
  - `tests/cfo_st2.cpp` ST2 scenarios: CFO step, initial offset, drift.
  - ST2 `World::ambientAmp` (0 = exact digital silence).
- **Run every standard command line on the unmodified receiver.** Archive as `docs/conformance/gap_base_*.csv`. These are the "before" numbers for every later gate.
- **Gate:** suites green. Baseline reproduces the investigation figures within ±5 pp: fast cliff at 3.5 Hz; WIDE Poor M4 hdrFail ≥ 35%.

### 8.1 Kill/resume (R7)
Prototype: `investigations/2026-10-gap/killresume/fix.patch`.
1. **Detector floor (`chirp.cpp` `evaluateGridPoint`).** Update `emaNoiseNcc_` only when `denom > 1e-12`, and clamp it at ≥ 0.5/N. White-noise NCC ≈ 1/N, so the clamp engages only on exact silence.
2. **Never drop audio (`worker.cpp`).** Split `pumpRxLocked` into ingest (drain the ring, up to 512 chunks per pass) then decode (budget 2). Under overload, shed the oldest captures, never live audio.
3. **Margin and visibility.**
   - RX ring `kFsAudio*8`.
   - `SampleRing` dropped-sample counter.
   - Rate-limited `WARN rx audio overrun`, plus the overrun total on shutdown.
   - Telemetry `rxCapturesOpened/Dropped`.
4. **Harness.** `st3_hardware.launch` writes a numbered log per launch, so relaunches no longer erase evidence.

**Tests.** `test_chirp::chirp_no_phantoms_in_digital_silence` (120 s zeros + burst + 40 s zeros + 1 preamble → exactly 1 detection) and `test_st2::st2_peer_death_digital_silence` (ambient 0: survivor opens ≤4 captures, evicts 0).

**Gate:**
- New tests fail on baseline and pass after the fix.
- `ctest -LE hardware` is green.
- `-DSKW_HARDWARE_TESTS=ON`, `ctest -L hardware`: `st3_killresume` passes **2 consecutive runs (10/10 reconnects)**, with no `rx audio overrun` and `rx overrun=0`. `st3_hardware` still passes.

### 8.2 Unified CFO ambiguity resolution (R1)
In `receiveFrame`, for **every** signature, enumerate the ambiguity branches `c_k = cfoFine + k/(gap·T)` over ±55 Hz (the detector's ridge).
- Correct each branch's start for delay-Doppler bias: the full `c·T/B·fs` for slope-less fast patterns, 0 for balanced U-D patterns.
- Score each branch with a local `zcRefine` (radius 8, fit ±1 Hz, new `fitPower` output).
- Run the full `zcRefine` at the winner.
- Widen the ZC fit to cover half a branch spacing, and drop the chirp prior when the residual exceeds `3·sqrt(σprior² + σmeas²)`.

Prototypes: `cfo/prototype.diff` (`SKW_CFO_HYP`, fast only) and `fading/experiment.patch` (`SKW_ZC_RANGE/GATE`, robust). The unified version replaces both.

**Cost:** +0.7–3 ms per detected frame.

**Tests.**
- `test_phy_rx`: M8 and M11 on every family at ±3.5, ±20 and ±45 Hz.
- A two-path 2 ms static channel with the early path dominant on up reps and the late path on down reps (WIDE robust header must decode).

**Gate:**
- CFO_SWEEP: every mode ≤ 5% FER at every C (baseline: fast 100% at |C| ≥ 3.5).
- POOR: WIDE M4 hdrFail ≤ 2% at ≥ 6 dB, and WIDE M4 ≤ 10% FER at ≤ 0 dB (baseline: 38% floor).
- AWGN_GATE: no point worse than baseline by > 4 pp.
- `cfo_st2` STEP=40, CFO0=45: zero drops, ≥ 90% of baseline bytes.

### 8.3 Window backoff and tap grid (R4)
- **Window backoff (`zc.cpp:304`).** Make it profile-dependent: `min(16, ⅔·shortCP)` samples, so HF gets 16 and VHF stays at its current 2 (its Short CP is 8 samples).
- **Tap grid (`equalizer.cpp:21,28`).** Start it k0 = 5 taps earlier on HF.
- **Update tests** `test_zc.cpp:85,109,126` (timing assertions shift by the backoff).
- **Gate:**
  - MOD_FAST: WIDE M8 ≤ 15% @ 22 dB (baseline 15–25%).
  - Fast-tracker estimation error on a static two-path 1 ms channel < 0 dB relative to noise (new `test_phy_rx` case).
  - `test_vhf` passes.
  - AWGN_GATE no regression.

### 8.4 Q normalised to frame-mean channel power (R5)
`receiver.cpp:657` passes the frame-mean in-band power minus the out-of-band noise; `equalizer.cpp:99` uses it in place of the ZC-time power.
- **Gate:** MOD_FAST WIDE M8 ≤ 5% @ 25 dB and M11 ≤ 10% @ 32 dB. AWGN_GATE no regression.

### 8.5 RTS smoother (R6)
Store the forward-filter history in `ChannelTracker`, smooth with RTS, and run it as a `SymbolWalker` pre-pass over the header and payload walks.
- **Gate:**
  - MOD_FAST WIDE M8 ≤ 10% at ≤ 11.5 dB and ≤ 3% at 25 dB; M11 ≤ 10% at ≤ 23 dB.
  - POOR WIDE M6 ≤ 10% at ≤ 1 dB.
  - Worker CPU per frame ≤ 2× baseline (bertool timing column).
  - AWGN_GATE no regression.

### 8.6 In-frame FLL (R2)
- **What:** add a phase-continuous residual-frequency state in `SymbolWalker`, fed from `Σ h·conj(h_prev)`.
- **Gain:** 0.2 for the fast class, ~0.07 for robust (spread) modes. Gain 0.2 hurt robust modes under fading in the prototype: NARROW M4 7% → 16%.
- **Interaction:** with the RTS smoother (8.5) active, the FLL derotation must be applied before the forward pass, so the smoother sees the same derotated symbols. Measure the stack, not FLL alone.
- **Gate:**
  - DRIFT on AWGN at 1 Hz/s: all families M4–M11 ≤ 5%. At 2 Hz/s: WIDE/MID ≤ 10%.
  - DRIFT on Moderate at 0.5 Hz/s: ≤ the 0-drift FER + 5 pp.
  - MOD_FAST and POOR at 0 drift: no point worse than the 8.5 result by > 2σ (binomial, 200 trials).
  - `cfo_st2` DRIFT=0.1: full throughput.

### 8.7 TX alignment hygiene (R3)
- **Slew and clamp.** Use `max(2 Hz, |reported|/2)` slew and a ±50 Hz clamp (`engine.cpp:1724-1728`). Make the advisor nudge and bootstrap clamp consistent.
- **Warning.** `EVT:CFO_WARN` fires on |measured offset| > 40 Hz, not on the clamped shift.
- **Docs.** Fix the PROTOCOL.md "split" text: it is a 0.5 loop gain, and both ends converge.
- **One-way flows.** The data receiver applies reciprocity from its own measured CFO of the peer's frames, with the same slew and clamp. No wire change. A DATA-borne report would need a `kWireVersion` bump, so it is deferred.
- **Gate:** `cfo_st2` STEP=40 re-aligns within 30 s, and the CONNECT-at-45 Hz residual is < 2 Hz. ST2 suites green.

### 8.8 CSS AFC
- **What:** decision-directed tracking of the dechirped peak's fractional bin across symbols (`src/dsp/css.cpp`, `cssReceiveFrame`, `receiver.cpp:163+`).
- **Baseline:** NARROW M3 fails at 0.05 Hz/s, M0 at 0.2 Hz/s.
- **Gate:** `bertool --css` NARROW and WIDE M0–M3 at 0.1 Hz/s drift ≤ the floor+3 spec. Static ±45 Hz unchanged.

### 8.9 Re-measure and re-calibrate
- **Floor tables.** Re-measure `kFloors` (AWGN) and `kFadingFloors` (Good, Moderate), and **add Poor rows** (`constants.hpp:218-256`). Use 200 trials per point; `docs_floors` keeps the docs in sync.
- **Downstream calibration.** Recalibrate the `chanclass` pilot-residual thresholds, `kOfdmSnrCalDb`, and `severeMaxMode` (currently 7, set because WIDE Poor was unusable).
- **Doc corrections.**
  - Correct `conformance/poor_findings.md`, `WAVEFORM.md:292-298` (CFO capture), and DSP.md:64-78 (PLL/CFO).
  - Remove the dead `kCfoCapture*` constants and update `test_css.cpp:161-167`.
- **Gate:** all six conformance sweeps "all points pass", the `test_m3_ramp` genie oracle passes, and `soak_st2` churn is not worse than v1.1.0.

### 8.10 Release gates (RELEASE.md 1–7) and v1.2.0 notes
Run the full sequence:
- Both flavors.
- ASan, UBSan and TSan (`test_st2`, `test_tcp`).
- Conformance.
- `st3_hardware` plus `st3_killresume`, ×2.
- The 2 h soak.
- Install and docs.

The release notes state the wire compatibility: no `kWireVersion`/`kProtocolVersion` change.

## Deferred (recorded, not in this phase)

- **Kill/resume follow-ups:**
  - Duplicate decodes from overlapping captures (retire sibling captures once one decodes).
  - A per-attempt decode deadline on long ladder rungs.
- **A periodic robust-signature probe** carrying a CFO report when nothing has decoded for N s with traffic queued (re-acquisition belt and braces).
- **A DATA-borne CFO report** (needs a `kWireVersion` bump).
- **TX-side fading gains:**
  - Frame-wide interleaving.
  - N=1944 for fast-tier codewords: predicted ≤1–2 dB on Moderate, needs a TX change.
- **A multipath-robust coarse chirp CFO**, i.e. a better estimator rather than ambiguity search. Not needed if 8.2's branch search holds at the CSS tier. 8.8 checks this.
- **On-air (ST4) testing.** It comes after this phase. Real radios are the remaining unknown: rig AGC, ALC/PAPR, T/R switching, QRM.
