# Skywave Build Plan

The phased construction strategy. Each phase ends with a verifiable
milestone; no phase starts until the previous phase's acceptance tests pass
in CI ([TESTING.md](TESTING.md)).

This document defines *phases, milestones, and policy*. The step-by-step
execution checklist — session-sized tasks in dependency order, with
per-step test gates and progress tracking — is [ROADMAP.md](ROADMAP.md);
builders work from there and treat this document as the milestone
authority.

## Toolchain and Dependencies

| Component | Requirement | Notes |
|---|---|---|
| C++ | C++20 (Clang 14+, GCC 11+, Apple Clang 14+) | no exceptions across the audio callback; RTTI allowed elsewhere |
| CMake | 3.20+ | single top-level project |
| PortAudio | required | audio I/O |
| FFT | in-tree | mixed-radix (factors 2/3/5), 320-point at the 12 kHz domain. KissFFT was planned; the in-tree version replaced it (ROADMAP deviation 1.1). |
| zstd | required | compression |
| libsodium | optional (`ENABLE_CRYPTO`) | X25519/HKDF/AES-GCM |
| Codec2 | optional (`ENABLE_VOICE`, default ON) | voice |
| SDL2 + Dear ImGui | optional (`ENABLE_GUI`, default ON) | ImGui vendored |
| Test harness | in-tree, tests only | `tests/skytest.hpp`. GoogleTest was planned and never vendored (ROADMAP deviation 0.1). |

### Build Options

| Option | Default |
|---|---|
| `ENABLE_GUI` | `ON` (auto-off if SDL2 missing, with warning) |
| `ENABLE_CRYPTO` | `OFF` (hard error if libsodium missing when ON) |
| `ENABLE_VOICE` | `ON` (auto-off if Codec2 missing, with warning) |
| `BUILD_TESTS` | `ON` |
| `BUILD_TOOLS` | `ON` |
| `ENABLE_ASAN` / `ENABLE_UBSAN` / `ENABLE_TSAN` | `OFF` |

Output binary: `build/skywave`. Tools: `build/chansim`, `build/bertool`,
`build/siggen`.

## Phase 0 — Scaffold

**Goal:** empty-but-running program with the project's bones.

- repo layout per [ARCHITECTURE.md](ARCHITECTURE.md)
- CMake with all options; Dear ImGui is the only vendored tree (FFT and
  test harness are in-tree - see the dependency table)
- `core/`: INI config (full schema from [CONFIGURATION.md](CONFIGURATION.md)),
  leveled logging, monotonic clock abstraction
- `io/audio`: PortAudio duplex stream, device listing, WAV read/write,
  `--replay`/`--record` paths, SPSC sample rings
- headless main loop; `--list-audio`, `--version`, `--help` work
- CI: build matrix (macOS/Linux × options), unit-test runner

**Milestone M0:** `skywave --replay in.wav --record out.wav` copies audio
through the full thread model; config round-trips INI; all scaffold unit
tests pass.

## Phase 1 — PHY Core

**Goal:** modulate and demodulate every active mode over a simulated channel.

Order of work:

1. `dsp/`: FFT wrapper, Hilbert/mixer/resampler chain (48k real ↔ 12k
   complex), windows, AGC, chirp generator + matched-filter detector, ZC
   sequences, PRN tables (single constants header)
2. `fec/`: LDPC N=648/1944 encode + min-sum decode (802.11n tables), CRC-16/32,
   whitener; bit-exact known-answer tests
3. `phy/` TX: mapper, spreader, pilot inserter, IFFT/CP/window, frame builder
   (preamble + ZC + header + payload); PAPR clipper; band filter
4. `tools/chansim`: Watterson model — 2 independent Rayleigh taps with
   Gaussian Doppler spectrum, configurable delay/Doppler spread, AWGN, CFO,
   sample-clock offset; CCIR Good (0.5 ms / 0.1 Hz), Moderate (1 ms /
   0.5 Hz), Poor (2 ms / 1 Hz) presets
5. `phy/` RX: sync engine (SEARCH→TRACK, including sideband-inversion
   detection/correction), channel estimator (+ delay-spread estimate),
   MMSE equalizer, despreader, soft demapper, header decoder, payload
   decoder
6. `tools/bertool`: in-process TX → chansim → RX sweep driver producing
   FER/SNR curves per mode

**Milestone M1:** every `ULTRA`/`WIDE`/`MID`/`NARROW` × `M4–M13` mode meets its
[WAVEFORM.md](WAVEFORM.md) conformance targets in bertool (CCIR Moderate,
≤10% FER at floor). This is the project's hardest gate — budget accordingly.

## Phase 2 — Link Core (First Functional Modem)

**Goal:** two Skywave instances connect and move bytes reliably.

- link envelope framing, frame types, CRC integration
- session FSM: `IDLE/LISTENING/CALLING/CONNECTING/CONNECTED/DISCONNECTING`,
  handshake (`CALL/CALL_ACK/START/START_ACK`), timers/retries per
  [PROTOCOL.md](PROTOCOL.md)
- selective-repeat ARQ with compact ACK, adaptive ACK timeout, piggyback
- fixed-mode operation (initial mode from handshake quality; no AMC yet)
- TX/RX arbiter with priority queue, PTT sequencing (all methods), CCA
  gate, TX watchdog, `TXTEST` calibration signal
- TCP data + control servers with the Phase-2 command subset
  (`MYCALL/CONNECT/DISCONNECT/LISTEN/STATE/STATUS/BUSY/BUFFER/PTT*/TXTEST`)
- two-modem integration harness: two in-process instances wired through
  chansim ([TESTING.md](TESTING.md))

**Milestone M2:** two instances complete handshake at −8 dB (WIDE, CCIR
Moderate) and transfer a 100 KiB payload error-free at +10 dB and at 0 dB;
data survives 20% injected frame loss via ARQ.

## Phase 3 — Adaptation

**Goal:** the modem rides real channel dynamics.

- AMC controller with full rule set ([LINK_ADAPTATION.md](LINK_ADAPTATION.md)),
  `MODE_CHANGE` protocol, `MODE/MAXMODE/CPMODE` commands, delay-spread-driven
  `CPMODE AUTO`
- HARQ chase, then IR (rate-stepdown with LLR priors), `HARQ` command
- channel memory (persisted), probe-before-promote, carrier masking
- MAC burst profiles with the PHY burst container, keepalive, demotion
  fast paths
- TX frequency alignment (CFO in ACK feedback, slewed TX pre-shift)
- monitor mode (control + data port surfaces)

**Milestone M3:** scripted chansim SNR ramp (−10 → +22 → −10 dB over 10 min)
shows monotone-reasonable mode tracking with zero session drops; throughput
within 25% of the oracle (best-fixed-mode-per-SNR) integral; mask exchange
demonstrably excises an injected carrier tone.

## Phase 4 — Services

**Goal:** the application-facing feature set.

- zstd compression with negotiation and coalesce window
- fragmentation/reassembly; file transfer with checkpoints and notifications
- session resume (token, window, file continuation)
- beacons, CQ, `BCAST_DATA`, CW ID renderer
- `STATUS JSON`, remaining observability commands, `FREQINFO`

**Milestone M4:** file transfer of 1 MiB across a session that is forcibly
dropped and resumed completes with matching CRC; beacon/CQ/monitor
interoperate between two instances; compression shows measurable gain on
text payloads.

## Phase 5 — Voice, Crypto, GUI

- crypto subsystem per [SECURITY.md](SECURITY.md) (flag-gated), `SECURITY`
  surfaces, epoch rotation, replay tests
- Codec2 voice per [VOICE.md](VOICE.md): voice port, talkspurt control,
  jitter buffer, codec auto-mapping, broadcast voice
- GUI per [INTERFACES.md](INTERFACES.md): waterfall, constellation, ladder
  panel, session panel, settings; telemetry snapshot plumbing

**Milestone M5:** encrypted session passes all Phase-2/3 integration tests
with crypto enabled; voice round-trip through two instances + chansim at
`WIDE M9` is intelligible (informal) with concealment under 5% loss; GUI
renders live waterfall/constellation during a connected session.

## Phase 6 — Extensions

- CSS deep tier: `M0–M3` modulator/demodulator, sync arbitration,
  `CAP_CSS_TIER` negotiation, handshake-on-CSS, ladder integration
  (forward spec in [WAVEFORM.md](WAVEFORM.md))
- Advisor layer (shadow → assist) per [LINK_ADAPTATION.md](LINK_ADAPTATION.md)
- companion-app integration polish, packaging, THIRD_PARTY notices,
  release engineering

**Milestone M6:** connect and transfer at −13 dB (WIDE, CCIR Moderate) via
the CSS tier; Advisor shadow logs show non-degradation across the Phase-3
ramp scenario.

## Phase 7 — VHF-FM Band Profile

- second band-profile table per the forward spec in
  [WAVEFORM.md](WAVEFORM.md): ~150 Hz spacing, sub-ms CP, ~6.75 kHz plan,
  top-heavy ladder (possible 256QAM rung), carrier-detect CCA, retuned sync
- flat-channel simulator preset (Rician/near-AWGN) in chansim
- everything above the PHY is reused unchanged

**Milestone M7:** two instances over the flat-channel simulator sustain
≥ 25 kbps net; all Phase-2/3/4 integration scenarios pass under the
`vhf-fm` profile.

*(Prerequisite discipline from Phase 0: all `HF` numerology lives in one
profile table so this phase adds a table, not a refactor.)*

## Standing Engineering Rules

- every numeric constant in these docs lives in **one** header
  (`src/core/constants.hpp`) or the mode tables — no magic numbers in logic
- the modem worker never allocates after startup; verify with an allocation
  hook in debug builds
- all on-air formats round-trip through pure functions testable without
  audio or sockets
- new features land with their conformance test in the same change
- sanitizer CI (ASan/UBSan) stays green from Phase 0 onward
