# Skywave Architecture

## Overview

Skywave is a half-duplex HF audio modem built around a shared `48 kHz`
real-passband runtime and a **single unified spread-OFDM waveform engine**.
Every mode on the ladder — from the ~−8.5 dB robust tier to the ~9 kbps top
tier — is the same OFDM engine with different spreading, constellation, and
code-rate parameters. There is exactly one modulator, one demodulator, one
synchronizer, one equalizer, and one framing path.

Four bandwidth families share one control model and differ only in carrier
plans and rate tables:

| Family | Occupied BW | Carriers | Center | Typical use |
|---|---:|---:|---:|---|
| `ULTRA` | ~2.7 kHz | 72 | ~1500 Hz | opt-in; radios with ~100–2900 Hz passband (modern DSP rigs, SDRs) |
| `WIDE` | ~2.4 kHz | 64 | 1500 Hz | **default** wideband; every stock SSB passband |
| `MID` | ~1.0 kHz | 27 | 1500 Hz | shared/crowded band segments |
| `NARROW` | ~0.5 kHz | 13 | 1500 Hz | CW-filter operation |

`ULTRA` must be explicitly enabled on both stations; `WIDE` is and remains
the default wideband mode.

All families use the same session model:

- connection handshake: chirp preamble + the family's most robust active mode
- connected modes `M4–M13` on spread-OFDM (see [WAVEFORM.md](WAVEFORM.md))
- ladder slots `M0–M3` reserved for a future non-coherent CSS deep tier
- in-band mode control and compact feedback after connect

## Layered Model

```text
+--------------------------------------------------------------+
| Applications (terminal, BBS, files, voice app, companion)    |
+-----------------+----------------+---------------------------+
| TCP data 9100   | TCP ctrl 9101  | TCP voice 9102            |
+-----------------+----------------+---------------------------+
| Services: segmentation, zstd compression, file transfer,     |
|           session resume, crypto (optional), Codec2 voice    |
+--------------------------------------------------------------+
| Link: session FSM, handshake, selective-repeat ARQ + HARQ,   |
|       adaptive mode control (AMC), channel memory, probing,  |
|       carrier masking, MAC burst policy                      |
+--------------------------------------------------------------+
| PHY: frame builder/parser, spread-OFDM modem, sync (chirp +  |
|      ZC), pilots/equalizer, LDPC + interleaving, CRC         |
+--------------------------------------------------------------+
| I/O: PortAudio duplex stream, TX shaping/levels, PTT keying, |
|      WAV record/replay, CW ID                                |
+--------------------------------------------------------------+
```

Strict layering rule: **services never touch samples; PHY never touches
sessions.** The link layer is the only component that talks to both.

## Session Lifecycle

The link-layer finite state machine:

```text
IDLE -> LISTENING
IDLE -> CALLING -> CONNECTING -> CONNECTED -> DISCONNECTING -> IDLE
LISTENING -> CONNECTING -> CONNECTED
```

1. Initiator sends `CALL` (chirp + robust-mode frame) carrying identity and
   capability summary.
2. Responder measures received quality, replies `CALL_ACK` with its own
   measurement.
3. Initiator sends `START` committing family, capabilities, and proposed
   initial mode (derived from the exchanged quality reports).
4. Responder answers `START_ACK` (or `START_NAK`) with the final initial mode.
5. Both sides enter `CONNECTED`; the AMC controller owns the mode from there.

The handshake rides the family's most robust active mode (`M4` in v1), so
connection establishment works wherever data can flow — by design there is no
"can connect but cannot pass traffic" regime.

## Major Subsystems

Repository layout (as built; the scaffold plan's standalone `chansim` and
`siggen` tools were not built as such - the channel simulators are
`src/sim/` libraries that the tests and tools link):

```text
skywave/
  CMakeLists.txt
  README.md      build, run, test
  THIRD_PARTY.md linked and vendored components, licenses
  src/
    core/        config (INI load/save/merge), logging, clocks, constants
                 and band profiles, byte packing
    dsp/         fft, fir, up/down converters (12 kHz <-> 48 kHz), agc,
                 windows, chirp gen/detect, zc sequences, css, morse
    phy/         framer (preamble+sync+header+payload), receiver,
                 equalizer/channel tracker, mapper, carrier layout
    fec/         ldpc (802.11n tables), crc, interleaver, whitener
    link/        engine, session fsm, handshake, arq, amc, channel
                 classifier, channel memory, probe, carrier mask, monitor,
                 advisor
    proto/       payload pipeline (segmentation, zstd), file service
    crypto/      session keys and AEAD framing             [ENABLE_CRYPTO]
    voice/       codec2 wrapper, voice session              [ENABLE_VOICE]
    io/          audio (PortAudio), ptt, sample rings, wav
    gui/         SDL2 + Dear ImGui application              [ENABLE_GUI]
    tcp/         data/control/voice servers, control parser
    sim/         chansim (Watterson HF), radiochain (SSB rig chain),
                 fmradio (FM rig and RF path) - test/tool support
    worker.*     the modem process: audio <-> engine, TX/RX scheduling
    main.cpp     CLI, process wiring, headless event loop
  tools/
    bertool      FER sweeps over chansim (conformance)
    fmsweep      VHF FER vs CNR through fmradio
    fuzztool     receiver fuzzing
    onair/       on-air test tooling (sounder, PTT, receiver captures)
    *.sh, *.py   sweep campaigns and summaries
  tests/         unit, loopback, link, conformance, ST2 (two modem
                 processes in one world) and ST3 (real audio devices)
  docs/          this documentation set
```

### Subsystem Responsibilities

| Subsystem | Responsibility | Key design refs |
|---|---|---|
| `core` | INI config, leveled logging, monotonic clocks | [CONFIGURATION.md](CONFIGURATION.md) |
| `dsp` | numerics only; no protocol knowledge | [DSP.md](DSP.md) |
| `phy` | symbols <-> samples; one frame in, one frame out | [WAVEFORM.md](WAVEFORM.md) |
| `fec` | LDPC N=1944/648, CRC, whitening, HARQ soft combining | [WAVEFORM.md](WAVEFORM.md) |
| `link` | sessions, ARQ, adaptation; the only PHY client | [PROTOCOL.md](PROTOCOL.md), [LINK_ADAPTATION.md](LINK_ADAPTATION.md) |
| `proto` | payload pipeline above the link | [PROTOCOL.md](PROTOCOL.md) |
| `crypto` | session keys, AEAD framing, epochs | [SECURITY.md](SECURITY.md) |
| `voice` | Codec2 encode/decode, talkspurt control | [VOICE.md](VOICE.md) |
| `io` | real-time audio, PTT, TX levels, CW ID | [DSP.md](DSP.md), [INTERFACES.md](INTERFACES.md) |
| `tcp` | the three localhost ports and command surface | [INTERFACES.md](INTERFACES.md) |
| `sim` | channel and radio emulators for tests and tools (never linked into the modem's signal path) | [TESTING.md](TESTING.md) |
| `gui` | visualization and settings; consumes the same telemetry the control port exposes | [INTERFACES.md](INTERFACES.md) |

## Execution Model

Four threads, lock-free where it matters:

| Thread | Role | Constraints |
|---|---|---|
| Audio callback (PortAudio) | move samples between device and SPSC ring buffers | hard real-time; no allocation, no locks, no logging |
| Modem worker | all PHY + link processing, pulled from the RX ring, pushed to the TX ring | soft real-time; owns all modem state |
| Main thread | headless: startup/shutdown, TCP polling, PTT service, command dispatch, poll loop. GUI mode: the SDL2/ImGui event + render loop **must run here** (macOS requires SDL event handling on the main thread) | GUI reads only the telemetry snapshot |
| Service thread (GUI mode only) | takes over TCP polling, PTT service, and command dispatch when the main thread is occupied by the GUI loop | — |

Data crossing rules:

- audio <-> worker: single-producer/single-consumer sample ring buffers
- worker -> main/GUI: versioned telemetry snapshot (double-buffered POD struct)
- main -> worker: serialized command queue; the worker applies commands at
  frame boundaries

This keeps the entire modem core single-threaded and deterministically
testable: the test harness drives the worker directly with sample buffers and
never needs an audio device.

## Half-Duplex TX/RX Arbitration

The worker owns a TX/RX arbiter:

- RX is the default state; the demodulator's chirp detector runs continuously
- TX requests (link frames, keepalive, voice, CW ID) queue with priorities:
  `control-urgent > retransmit > data > keepalive > beacon`
- before ordinary TX, the clear-channel gate is consulted
  (see [DSP.md](DSP.md) CCA); urgent control and connected recovery bypass it
- PTT sequencing wraps every burst: key → lead delay → audio → tail delay →
  unkey (see [INTERFACES.md](INTERFACES.md))

## Design Principles

1. **One waveform engine.** Any feature that would require a second
   demodulator path must justify itself against the reserved-CSS-tier plan.
   The engine is parameterized by a **band profile** table (`HF` is the
   only v1 profile; a VHF-FM profile is forward-specified in
   [WAVEFORM.md](WAVEFORM.md)) — numerology must never leak out of the
   profile table into logic.
2. **One control surface.** There is no legacy mode numbering; `MODE M0–M13`
   is the only ladder surface in config, TCP, and GUI.
3. **Waveform-agnostic framing.** The PHY header carries an explicit waveform
   ID so the reserved CSS tier can slot in later without a wire-format change.
4. **Deterministic core.** The modem worker is a pure function of
   (samples, commands, clock); all randomness is seeded and injectable.
5. **Simulator-first validation.** Every mode must close its documented SNR
   floor in the Watterson simulator before hardware testing
   (see [TESTING.md](TESTING.md)).
