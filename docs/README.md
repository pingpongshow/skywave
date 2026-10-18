# Skywave — Design Documentation

Skywave is a half-duplex, connection-oriented HF (and VHF-FM) data/voice
modem implemented entirely in software against a soundcard audio
interface. This documentation set is the **design source of truth**: it
was written up front, the modem was built against it in phases (all of
them are done; see [ROADMAP.md](ROADMAP.md)), and it is kept in step with
the code. The repository's [README](../README.md) covers building,
running and testing; this file indexes the design.

Skywave is an original design that keeps the proven operational theme of
an earlier reference design (adaptive mode ladder, chirp acquisition,
weak-signal handshake, ARQ sessions, three-port TCP interface) while
committing to a simpler, unified physical layer and a modernized stack.

Current release: **v1.2.6** ([releases/v1.2.6.md](releases/v1.2.6.md)).

## Design Decisions Record

These decisions were made deliberately and are binding unless revisited:

| Decision | Choice |
|---|---|
| Language / build | C++20, CMake, minimal dependencies |
| PHY architecture | **Unified spread-OFDM** — one waveform engine for the whole ladder |
| Weak-signal floor | ~−8.5 dB SNR (3 kHz ref) on the OFDM ladder. The CSS deep tier at `M0–M3` was planned as future work and **shipped in v1.0.0**: it connects and moves data at −13 dB (MID, CCIR Moderate). |
| Handshake robustness | Matches the data floor (handshake rides the most robust data mode) |
| Bandwidth families | Three standard: `WIDE` 2.4 kHz (default), `MID` 1 kHz, `NARROW` 500 Hz — plus opt-in `ULTRA` 2.7 kHz for radios with ~100–2900 Hz passbands; all centered near **1500 Hz** |
| VHF-FM | A second band profile (`band = vhf-fm`): one 176-carrier 6.6 kHz family centred on 3600 Hz for an FM rig's 9600 data port, ladder to `M14` (256QAM), 25.5 kbps measured |
| Voice | Codec2 vocoder, OFDM-mode-mapped, PCM over local TCP voice port |
| Crypto | Optional, build-flag gated (`ENABLE_CRYPTO`); X25519 + AES-256-GCM with implicit nonces |
| UI | Built-in lightweight GUI (SDL2 + Dear ImGui) plus headless mode; radio control delegated to a companion app |
| Delivery | Full-parity design documented up front, **built in phases** — data core first (Phases 0-8 complete) |

## Document Index

| Document | Contents |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | System overview, subsystems, threading model, repository layout |
| [WAVEFORM.md](WAVEFORM.md) | OFDM numerology, bandwidth families, mode ladder, sync, spreading, FEC |
| [PROTOCOL.md](PROTOCOL.md) | Session model, frame formats, handshake, ARQ/HARQ, resume, beacons |
| [LINK_ADAPTATION.md](LINK_ADAPTATION.md) | Adaptive mode control (AMC), channel memory, probing, carrier masking |
| [DSP.md](DSP.md) | RX/TX signal chains, sync algorithms, equalization, AGC, interference tools, CCA |
| [INTERFACES.md](INTERFACES.md) | TCP data/control/voice ports, command set, notifications, PTT, companion split |
| [SECURITY.md](SECURITY.md) | Key exchange, frame protection, epochs, rotation, legal posture |
| [VOICE.md](VOICE.md) | Codec2 integration, voice session control, mode mapping |
| [CONFIGURATION.md](CONFIGURATION.md) | CLI, INI schema, GUI settings, runtime precedence |
| [BUILD_PLAN.md](BUILD_PLAN.md) | Dependencies, build options, **phased construction plan with milestones** |
| [ROADMAP.md](ROADMAP.md) | **Execution tracker**: ordered session-sized build steps with per-step test gates and checkboxes |
| [GAP_CLOSURE_PLAN.md](GAP_CLOSURE_PLAN.md) | Phase 8 reference: measured root causes and gates for fading, CFO tolerance, kill/resume |
| [ONAIR_TEST_PLAN.md](ONAIR_TEST_PLAN.md) | ST4 with one transmitter: bench and one-way skywave stages, `tools/onair/` tooling, what each run feeds |
| [TESTING.md](TESTING.md) | Test strategy: unit, loopback, channel-simulator, two-modem, conformance |
| [RELEASE.md](RELEASE.md) | Release gate sequence, how to cut a release, and the open blockers |
| [../THIRD_PARTY.md](../THIRD_PARTY.md) | Every linked or vendored component, with licenses (repository root) |
| [releases/](releases/) | Per-release notes and recorded gate results |
| [conformance/](conformance/) | Measured sweep and soak results (CSVs and findings), e.g. `vhf_fm_2026-10-17/` |
| [investigations/](investigations/) | Write-ups of specific measured problems |

## What Carried Over From the Reference Design, and What Changed

| Reference-design feature | Skywave disposition |
|---|---|
| Two waveform engines (CSS + OFDM) | **Evolved** — one spread-OFDM engine carries `M4–M14`, and the CSS deep tier shipped at `M0–M3` (v1.0.0), gated by `CAP_CSS_TIER` |
| Chirp acquisition preamble | **Retained** — chirp remains the detection/coarse-sync front end; the preamble signature additionally selects the header class |
| Dual ladder families at 1675 Hz center | **Evolved** — three standard families (`WIDE`/`MID`/`NARROW`) plus opt-in `ULTRA` 2.7 kHz, at the industry-standard 1500 Hz center |
| `M0–M13` runtime mode ladder + DRC | **Retained in shape** — 14-slot HF ladder (plus `M14` on VHF-FM), SNR-driven adaptive mode control with hysteresis; new mode contents |
| Legacy `LEVEL 1..12` dual control surface | **Dropped** — Skywave has exactly one mode surface (`MODE M0–M14`); no legacy aliases |
| Selective-repeat ARQ + HARQ (chase/IR) | **Retained** — compact base+bitmap feedback, chase and IR combining |
| Channel memory, probe-before-promote, tone masking, aggregated MAC | **Retained** — same concepts, renamed carrier masking, cleaner thresholds |
| zlib compression | **Replaced** with zstd (better ratio, negotiated capability) |
| X25519 + AES-256-GCM crypto, epochs, rotation | **Retained**, with implicit-nonce framing to cut per-frame overhead 28 → 16 bytes |
| Custom voice codec | **Replaced** with Codec2 (open source, proven, buildable) |
| Session resume, file transfer, beacons/CQ, CW ID | **Retained** |
| Three localhost TCP ports (data/control/voice) | **Retained** — new defaults 9100/9101/9102, line control protocol, JSON telemetry added |
| PTT in modem, radio control in companion app | **Retained** — same split, same TCP `KEY`/`UNKEY` bridge |
| ML assist (bounded nudges) | **Retained as "Advisor"** — bounded, deterministic, env-controlled, late phase |
| SDL2 + Nuklear GUI | **Replaced** with SDL2 + Dear ImGui |

## Build Phases at a Glance

Details and acceptance criteria live in [BUILD_PLAN.md](BUILD_PLAN.md).

- **Phase 0 — Scaffold**: repo, CMake, audio I/O, config, logging, test harness
- **Phase 1 — PHY core**: OFDM mod/demod, sync, LDPC, spreading; loopback over simulated HF channel
- **Phase 2 — Link core**: framing, session FSM, handshake, ARQ, TCP data/control, PTT → *first functional modem*
- **Phase 3 — Adaptation**: AMC controller, channel memory, probing, carrier masking, HARQ-IR
- **Phase 4 — Services**: compression, file transfer, resume, beacons/CQ, CW ID
- **Phase 5 — Voice, crypto, GUI**: Codec2 voice, gated encryption, ImGui interface
- **Phase 6 — Extensions**: CSS deep tier (`M0–M3`), Advisor layer, companion-app integration polish
- **Phase 7 — VHF-FM band profile**: wideband FM-rig data-port operation (6.6 kHz, 25.5 kbps measured) as a second numerology table on the same engine
- **Phase 8 — Gap closure**: fading, CFO tolerance, kill/resume ([GAP_CLOSURE_PLAN.md](GAP_CLOSURE_PLAN.md))

All phases are complete. Releases since then are fix releases; what each
changed and how it was verified is in [releases/](releases/) and the
dated sections of [ROADMAP.md](ROADMAP.md).

## Deliberately Deferred (Documented Non-Goals for v1)

Named here so their absence reads as a decision, not an oversight.
(The VHF-FM band profile was on this list; it SHIPPED in Phase 7 —
`band = vhf-fm`. Plain FM *voice-channel* operation already worked with
the HF profile.)

- **Mid-session bandwidth-family renegotiation** — family is fixed per session; changing requires reconnect (resume makes this cheap). Built and then removed 2026-08-17 as unnecessary: the CCIR Poor result argues for choosing the right family up front, not switching mid-session
- **True wideband SDR/IQ operation** (≥ 25 kHz channels, non-soundcard I/O) — out of scope for this project; a future sibling front-end reusing the link/proto layers as libraries
- **ALE-style multi-channel scanning** — belongs in the companion app driving `FREQINFO`/`LISTEN`
- **zstd dictionary training** for ham-traffic priors — plain zstd first; dictionaries are a compatible later capability bit
- **Multi-party / group sessions and group keying** — strictly point-to-point plus broadcast in v1

## Conventions Used Throughout

- SNR figures are referenced to a **3 kHz noise bandwidth** unless stated otherwise.
- `NET` rates are post-FEC payload bit rates; `GROSS` rates are uncoded waveform rates.
- All multi-byte wire fields are **little-endian** unless a format says otherwise.
- Mode identifiers `M0–M14` are ladder positions; `M0–M3` are the CSS deep tier (Phase 6, HF only), used only when both peers negotiate `CAP_CSS_TIER`; `M14` (256QAM) exists only on the VHF-FM profile.
- VHF-FM figures through the FM radio emulator are quoted as **CNR** in the receiver's IF bandwidth, not audio SNR.
- Rates and floors are quoted for the default `MEDIUM` cyclic prefix.
