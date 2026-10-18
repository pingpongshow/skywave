# Skywave

Skywave is a half-duplex, connection-oriented software modem for amateur
radio data and voice. It runs on an ordinary computer with a soundcard
interface to a transceiver: HF SSB rigs (300-3000 Hz audio) and, with the
VHF-FM profile, FM rigs with a 9600-capable data port. Applications talk
to it over three localhost TCP ports.

**Status:** v1.2.7 (2026-10-18). Not publicly released: every result below
comes from simulation (Watterson HF channels, an FM radio emulator, and a
two-modem world running the real modem process), plus real-audio-device
tests. On-air confirmation is pending. There is no LICENSE yet (see
[Known limitations](#known-limitations)).

## What it does

- **One spread-OFDM waveform** for the whole adaptive mode ladder
  (`M4-M13`; `M14` 256QAM on VHF), with LDPC FEC, frequency-domain
  spreading for the robust modes, and chirp acquisition with a ZC
  fine-sync block.
- **CSS deep tier** (`M0-M3`) for links below the OFDM floor: connects and
  moves data at -13 dB SNR on CCIR Moderate.
- **Bandwidth families** (HF, centred on 1500 Hz):

  | Family | Occupied | Peak net rate | Notes |
  |---|---|---|---|
  | `WIDE` | 2.4 kHz | 9.1 kbps | default |
  | `MID` | 1 kHz | 3.8 kbps | |
  | `NARROW` | 500 Hz | 1.9 kbps | |
  | `ULTRA` | 2.7 kHz | 10.3 kbps | opt-in, for ~100-2900 Hz rig passbands |
  | `VHF` | 6.6 kHz | 25.5 kbps measured | `band = vhf-fm`, FM data port, centred on 3600 Hz |

- **Link layer:** CALL/START handshake on the most robust mode,
  selective-repeat ARQ with HARQ (chase and incremental redundancy),
  scheduled turn-taking with a poll bit, adaptive mode control with
  channel memory and probe-before-promote, carrier masking around
  interference, session resume, keepalives.
- **Services:** zstd compression, file transfer, beacons/CQ, CW ID,
  passive monitor of other stations' traffic.
- **Voice:** Codec2, mode-mapped to the link, PCM over the voice port.
- **Encryption (optional build):** X25519 key exchange, AES-256-GCM,
  epochs and key rotation.
- **GUI** (SDL2 + Dear ImGui): waterfall, constellation, mode ladder,
  settings. Headless operation is the default.

Radio control (CAT, frequency) belongs to a companion app; Skywave keys
PTT itself: VOX, serial RTS or DTR, or a TCP `KEY`/`UNKEY` bridge to the
app that owns the rig.
**Skywave Link** is that companion (chat, mail, files, BBS, APRS, voice,
QSY); it lives in its own repository, `skywave-link`.

## Build

Requirements: CMake 3.20+, a C++20 compiler, pkg-config, and

| Library | Needed for |
|---|---|
| PortAudio | audio I/O (required) |
| zstd | compression (required) |
| Codec2 | voice (`ENABLE_VOICE`, on by default; skipped if absent) |
| SDL2 + OpenGL | GUI (`ENABLE_GUI`, on by default; skipped if absent) |
| libsodium | encryption (`ENABLE_CRYPTO`, off by default) |

On macOS: `brew install cmake pkg-config portaudio zstd codec2 sdl2 libsodium`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

```bash
cmake -S . -B build-crypto -DENABLE_CRYPTO=ON
```

Codec2 is LGPL-2.1: the default build links it dynamically. For a static
or single-file distribution, configure `-DENABLE_VOICE=OFF` or take on the
LGPL obligations ([THIRD_PARTY.md](THIRD_PARTY.md)).

## Run

```bash
./build/skywave --list-audio
./build/skywave --callsign N0CALL --audio-in "USB Audio CODEC" --audio-out "USB Audio CODEC" --config ~/.skywave/skywave.ini
./build/skywave --gui --callsign N0CALL
```

| Port | Default | Carries |
|---|---|---|
| data | 9100 | the session's byte stream (one client) |
| control | 9101 | line commands (`CONNECT`, `MODE`, `STATUS`, ...) and `EVT:` notifications |
| voice | 9102 | framed PCM16 voice (voice builds only) |

`--config` names an INI file that is loaded at start and saved on exit
(atomically, mode 0600, merged with any edits made to the file while the
modem ran). The full CLI, INI schema and environment knobs are in
[CONFIGURATION.md](docs/CONFIGURATION.md); the ports and command set in
[INTERFACES.md](docs/INTERFACES.md); drive-level calibration
(`TXTEST`, `AUDIOLEVEL`) in INTERFACES.md "TX Drive Calibration".

For VHF-FM, set `band = vhf-fm` in the INI, use the rig's 9600 data port,
and set its level so bursts peak at the rig's maximum deviation
([WAVEFORM.md](docs/WAVEFORM.md) "VHF-FM Band Profile").

## Test

```bash
cd build && ctest -j8
```

The suite (48 tests) covers unit, PHY loopback, two-engine link,
conformance, docs-consistency, two-modem process-level (ST2) and real
audio-device tests (ST3: two duplex loopback devices, enabled with
`-DSKW_HARDWARE_TESTS=ON`; skipped otherwise).
Sanitizer builds: `-DENABLE_ASAN=ON`, `-DENABLE_UBSAN=ON`,
`-DENABLE_TSAN=ON`.

Long-running and measurement tools (not run by ctest):

| Tool | Purpose |
|---|---|
| `bertool` | FER sweeps per family and mode over the Watterson channel simulator |
| `fmsweep` | VHF FER vs carrier-to-noise ratio through the FM radio emulator |
| `soak_st2` | endurance scenarios (churn, floor, blackout) on two full modems; `SKW_ST2_BAND=vhf` for VHF through the FM radio |
| `fuzztool` | receiver fuzzing on corrupted captures |
| `onair_sounder` | on-air one-transmitter test tooling ([ONAIR_TEST_PLAN.md](docs/ONAIR_TEST_PLAN.md)) |

The test strategy and layers are in [TESTING.md](docs/TESTING.md);
measured results are under [docs/conformance/](docs/conformance/).

## Documentation

[docs/README.md](docs/README.md) indexes the full set: architecture,
waveform, protocol, link adaptation, DSP, interfaces, security, voice,
configuration, testing, release procedure, the execution roadmap, and the
per-release notes in [docs/releases/](docs/releases/).

## Known limitations

- **Simulation only so far.** On-air tests (HF and FM) are pending.
- **No LICENSE file.** Terms of use and redistribution are undefined,
  which blocks any public release. This is a deliberate hold.
- **VHF `M14` needs a data port flat below 300 Hz.** On a port that rolls
  off at 300 Hz the modem tops out at `M13`.
- **Voice latency is seconds on HF** (4-7 s to first audio): every voice
  unit is its own frame with its own preamble. NARROW has no real-time
  voice, MID only from M12 ([VOICE.md](docs/VOICE.md)).
- **NARROW on a Moderate floor at 6 dB** connects on 4 of 6 seeds: its
  robust frames are ~20 s long.
- Both ends of a link must run the same release: there is no backward
  compatibility before the first public release.
