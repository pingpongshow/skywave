# On-Air Test Plan (ST4)

First RF tests of Skywave, using the hardware on hand: a **FlexRadio**
(the only HF transmitter), a **Web-888** SDR, and public Web-888/KiwiSDR
receivers. There is no second station, so this plan tests the
**PHY one-way**. Two-way link-layer testing on air (ARQ, AMC in a live
session, turn-taking) waits for a second station.

A Pluto-class SDR is no use as a transmitter here. It tunes 70 MHz and up
(325 MHz stock), puts out milliwatts with unfiltered harmonics, and
nothing else on hand receives above 54 MHz.

Tooling lives in [`tools/onair/`](../tools/onair/README.md), outside the
modem:

| Tool | Role |
|---|---|
| `onair_sounder gen` | builds a sounding WAV and manifest: real PHY frames at each mode, interleaved round-robin and grouped into keyed segments, with CW ID |
| `onair_sounder play` | plays it to the radio's TX audio device and keys PTT per segment |
| `onair_sounder score` | decodes a received recording against the manifest. Reports FER per mode; SNR, EVM, CFO, delay spread and pilot stability per frame; CFO drift; and the clock offset |
| `flexptt.py` | TCP PTT server. Speaks the modem's `KEY`/`UNKEY` protocol and keys the Flex through the SmartSDR API |

## Prerequisites

1. **Licence and band plan.** Stage 2 transmits: use a data sub-band,
   keep the power reasonable, and ID. `gen --call` puts CW ID at both
   ends of every sounding. Stage 1 into a dummy load needs `--call` only
   if anything leaks off-site.
2. **Protect the Web-888.** Never transmit with the Web-888 on an antenna
   near the Flex's: 100 W next to an SDR front end can destroy it. Stage 1
   runs the Flex into a **dummy load at minimum power**, and the Web-888
   picks up the leakage on a short wire, through an attenuator if
   needed.
3. **TX audio.** DAX TX via SmartSDR for Mac, or the third-party AetherSDR
   client (it shows up as the `AetherSDR TX` device). Set the transmit
   source to DAX. `onair_sounder devices` lists the device names.
4. **PTT.** `tools/onair/flexptt.py --discover`, then
   `flexptt.py --radio <ip>`. It listens on 127.0.0.1:4532, the modem's
   default TCP PTT port, with a 45 s TX watchdog.
5. **Receiver settings** (Web-888 / KiwiSDR):
   - USB, dial frequency = the Flex's dial frequency.
   - Passband covering the family. `gen` prints it; WIDE is 300–2700 Hz.
   - **AGC off, manual gain**, so the signal never pumps.
   - Audio compression off if offered.
   - Record WAV with the web UI's recorder or `kiwirecorder.py`
     (kiwiclient), using `--agc-gain` for manual gain and `--tlimit` for
     length. Check `--help` on your version.

## Known issue to fix before stage 1

**The modem's TX audio clips on any real interface.** The PHY emits
unit-RMS baseband with an 8 dB PAPR allowance, which upconverts to peaks
of about +9 dBFS at `output_gain_db = 0`. BlackHole passes float samples
above 1.0 untouched, which is why ST3 never showed it. DAX, a USB codec,
or a 16-bit WAV clips them. In a clean loopback through a 16-bit WAV,
every M12/M13 frame and some robust frames failed until the level came
down 10 dB.

Separately, `output_gain_db` is applied **twice**
([worker.cpp:856](../src/worker.cpp) and [audio.cpp:109](../src/io/audio.cpp)),
so a −5 dB setting gives −10 dB.

The sounder defaults to `--gain-db -10` (peak about −1 dBFS), so stage 1
and stage 2 are unaffected. The modem itself needs a fixed TX scale
before it goes on air. Until then, `output_gain_db = -5` (−10 dB
effective) is the workaround.

## Stage 1: bench, Flex into a dummy load, Web-888 on leakage

These are the bench checks left open by the pre-on-air review
([ROADMAP.md](ROADMAP.md)), now through the real TX and RX chain.

| Check | Procedure | Pass |
|---|---|---|
| **Drive / ALC / PAPR knee** | `gen --no-id --modes 8,13 --reps 4 --levels 0,-3,-6,-9,-12`. Set the Flex drive and DAX TX gain so the 0 dB step just touches ALC, then play and score. | EVM SNR rises as the level falls and then flattens. The knee is where to set drive, about 3 dB below the first step that loses EVM. M13 0% FER at the chosen level. |
| **Full ladder at high SNR** | `gen --no-id --reps 5` (M4–M13), then play and score | 0 failures across M4–M13. Any failure at high SNR is a chain problem, not propagation. |
| **PTT lead and tail** | Watch the `flexptt` log against the Flex's TX indicator. Score shows whether a segment's first frame is lost (`missed`/`header_fail` on frame 0 of each segment). | No first-frame loss at `--lead-ms 100`. Then find the smallest lead that still passes; that becomes the modem's `ptt.lead_ms`. |
| **Output latency** | `play` prints the TX device's reported output latency. | Recorded. The modem holds PTT that long + 10 ms; a figure that looks wrong (0 ms, or seconds) must be fixed or padded with `ptt.tail_ms`. |
| **Clock (ppm)** | Score's `clock:` line | \|TX − RX\| ≲ 100 ppm. Report the figure; ±10 ppm is the resolution of one run. |
| **CFO** | Score's `CFO:` line. The Flex and Web-888 GPS-locked if available. | Mean offset within ±5 Hz, no drift. Otherwise record the rig's offset for stage 2. |
| **CPU** | `score` time per recording minute, and the modem's `rx decode` stats on the target host | Decode keeps up with real time with margin, as in the shutdown log of a modem run. |

## Stage 2: one-way skywave to remote receivers

The Flex into its antenna; recordings from public Web-888/KiwiSDR
receivers at several distances. Use NVIS (100–400 km) plus single-hop
(500–2000 km). Several receivers can record the same sounding at once:
each one is an independent path.

1. `gen --call <CALL> --reps 5` (WIDE M4–M13, about 3.5 min). Repeat
   with `--family narrow` and `--family mid` when time allows.
2. Start the recordings, then `play --wav ... --device "<TX device>"
   --ptt 127.0.0.1:4532`.
3. `score --wav <recording> --manifest <manifest> --csv <out>.csv` for
   each receiver.
4. Archive under `docs/conformance/onair/<date>_<band>_<rx>/`. Keep the
   recording, manifest, CSV and score output, plus a `NOTES.txt` with the
   band, time (UTC), power, antenna, receiver, distance, and the
   receiver's S-meter and noise floor.

### What each run feeds

| Open item | Data from the score |
|---|---|
| **AMC tuning on Good channels** (ROADMAP: same absolute throughput as v1.1.0, oracle +15%) | FER per mode against the receiver's SNR per frame, under real fading. Compare with `kFloors`/`kFadingFloors` and the class tables: are the floors where the channel says, and does the class implied by `pilot_stab` match? |
| **8.8 CSS drift** (real drift rates) | `CFO:` drift Hz/s and the worst frame-to-frame rate, per path and band, across times of day. For CSS frames add `--modes 0..3` (frames are seconds long, so use fewer reps). |
| **Floor calibration** | The receiver's `snr_dB` against `evm_dB` per mode: how the bias of the SNR estimate behaves on a real channel. |
| **Delay spread / CP policy** | `delay_ms` per path: are real channels inside the CP medium/long assumptions? |
| **Acquisition** | `missed` against `header_fail` against `payload_fail`, by mode and SNR. |

### Pass criteria

There is no fixed gate on the first runs: the purpose is measurement.
The questions to answer before tuning anything:

- At SNRs above a mode's fading floor + 3 dB, is FER at or under the
  conformance limits (≤ 11% fading)? If not, the channel is worse than
  the simulator's presets, or something in the chain degrades it.
- Is the scorer's `clock:` figure stable across receivers? A receiver
  much worse than ±100 ppm needs `--rate` set to its true sample rate.

## Stage 3: two-way (blocked: needs a second station)

Real ARQ/AMC on air needs a second transmitter on a different path.
A one-radio "loop" through a remote receiver was considered and dropped:
both directions would share one path and one radio, so it adds nothing
over stage 2.

When a second station is available: two modems with the TX level fixed,
a `CONNECT` and bulk transfer in both directions, `STATUS JSON` logged
every second, against the stage 2 results for the same band and time.

## Validation of the tooling (2026-10-12)

- Clean loopback (48 kHz WAV, −10 dB): 30/30 frames, M4–M13.
- Simulated KiwiSDR path (12 kHz, +7 Hz, 0.05 Hz/s drift, 40 ppm clock,
  12 dB SNR in 3 kHz): M4–M11 30/30 and M12/M13 failing, as their floors
  (13.5/17 dB) predict. Drift read 0.0498 Hz/s, mean CFO 10.29 Hz (true
  10.3), clock −57 ± 10 ppm.
- Real-time device path (BlackHole, PTT through `flexptt.py --dry-run`):
  6/6 frames, keying 12.3–12.4 s per 12.1 s segment, clock +2.3 ± 4.5 ppm.
- `flexptt.py` against a fake SmartSDR API server:
  - two clients OR-combined; a disconnect while keyed releases;
  - the watchdog trips and refuses re-keying until `UNKEY`;
  - `xmit 0` on exit.
