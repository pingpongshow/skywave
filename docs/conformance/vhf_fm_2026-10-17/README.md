# VHF-FM through an FM radio (2026-10-17)

`tools/fmsweep` runs each VHF mode through the FM radio emulator
(`src/sim/fmradio.hpp`): worker TX level, data-port filters, frequency
modulator, flat flutter, RF offset, AWGN in the receiver's IF, IF filter,
limiter-discriminator, data-port filters. The FM threshold and its click
noise come out of the discriminator, not out of a noise model.

Command (per preset, 20 frames per point, 2 dB CNR steps, payload 60 B
robust / 300 B fast):

```
fmsweep --preset NAME --trials 20 --cnr-from 0 --cnr-to 50 --csv NAME.csv
```

Each `NAME.txt` holds the per-mode trace; `NAME.csv` the raw points.

## Floors: lowest CNR (dB, in the IF) with FER <= 10%

| Mode | flat | lowedge | narrow | offset | hot | quiet | walk | mobile | uhf | voice |
|---|---|---|---|---|---|---|---|---|---|---|
| M4 | 8 | 8 | 10 | 8 | 4 | 16 | 18 | 20 | 18 | 6 |
| M5 | 10 | 10 | 12 | 10 | 6 | 20 | 20 | 26 | 20 | 6 |
| M6 | 12 | 12 | 14 | 12 | 6 | 22 | 20 | 24 | 20 | 8 |
| M7 | 14 | 14 | 16 | 14 | 8 | 24 | 22 | 24 | 26 | 10 |
| M8 | 18 | 18 | 20 | 18 | 12 | 28 | 24 | 32 | 32 | - |
| M9 | 20 | 20 | 24 | 20 | 14 | 30 | 32 | 38 | 34 | - |
| M10 | 22 | 22 | 26 | 22 | 16 | 32 | 36 | 36 | 36 | - |
| M11 | 28 | 26 | 30 | 28 | 22 | 38 | 34 | 44 | - | - |
| M12 | 30 | 30 | 34 | 30 | 24 | 40 | 40 | 48 | - | - |
| M13 | 36 | 34 | 40 | 36 | 30 | 46 | 46 | 48 | - | - |
| M14 | - | 40 | - | - | - | - | - | - | - | - |

`-` = not reached by 50 dB CNR.

Presets (all others are `flat` plus the one change):

| Preset | Radio |
|---|---|
| flat | 25 kHz channel (16 kHz IF), 3 kHz deviation at full scale, data port 300-7000 Hz (the profile's stated channel), static |
| lowedge | data port flat down to 20 Hz (a DC-coupled 9600 port) |
| narrow | 12.5 kHz channel: 11 kHz IF, 2.5 kHz deviation |
| offset | 700 Hz RF offset + 100 ppm soundcard clock |
| hot | +6 dB drive into a 3.5 kHz deviation limiter |
| quiet | -10 dB drive (1 kHz peak deviation) |
| walk / mobile / uhf | Rayleigh flutter 0.7 / 15 / 50 Hz (2 m walking, 2 m at ~110 km/h, 70 cm at ~120 km/h) |
| voice | rig on its voice/1200 path: 750 us pre/de-emphasis, 300-3000 Hz audio, 3.5 kHz limiter |

## Findings

- **The FM threshold sits where theory puts it** (~10 dB CNR); the robust
  tier (M4-M7) works down to it and below with deviation to spare.
- **Deviation is the operator's biggest lever.** -10 dB of drive costs
  8-10 dB of CNR on every mode; +6 dB into a limiter gains 4-6 dB and the
  clipping costs almost nothing below M13. Set the data-port level so a
  burst's peaks reach the rig's maximum deviation (~3 kHz on a 25 kHz
  channel) - with Skywave's 11 dB crest factor its RMS deviation is then
  only ~0.5 kHz.
- **M14 needs a port flat below 300 Hz.** The VHF plan's lowest carrier is
  at 300 Hz, on the edge of the 300 Hz data-port response the profile
  assumes. With that edge the receiver's SNR saturates at ~32 dB with no
  noise at all (the carriers in the roll-off), below M14's 33 dB floor;
  M11-M13 lose 2 dB of ceiling. With a port flat to 20 Hz M14 decodes from
  40 dB CNR. The AMC reads the saturated SNR and does not climb to M14, so
  the cost on a 300 Hz port is throughput, not failures.
- **Flutter costs 10-12 dB** (flat Rayleigh fading, no diversity); at
  50 Hz (70 cm mobile) M11+ never reaches 10% FER.
- **A 12.5 kHz channel costs 2-4 dB**; RF offset and soundcard clock
  error cost nothing.
- **The voice path carries M4-M7 only**: its 3 kHz audio filter removes
  most of the 6.6 kHz VHF signal and the robust tier's spreading recovers
  it. Use the HF profile on a rig's voice path (WAVEFORM.md).

## Receiver bug found and fixed by this sweep

Every first run failed at all CNRs. A 1-2 audio-sample shift of a clean
VHF frame was enough: the chirp detector correlated on a 4-sample grid,
and VHF's ~6.6 kHz chirp has a ~1.8-sample correlation peak, so a frame
whose start fell between grid points was missed (3-5 of 8 sample phases
failed in every mode; HF's ~2.4 kHz chirps were unaffected). The grid
step is now per band (`BandProfile::chirpGridStep`: HF 4, VHF 1; VHF's
reps are 4x shorter, so the cost is HF's). False detections on noise:
13 per 30 min (HF 7, VHF on the old grid 2). bertool's VHF floors were
measured at a phase the old grid happened to hit and still pass at 0% FER.
Test: `test_vhf` `vhf_detects_at_every_sample_phase`.

## Worker bug found and fixed by the ST2 VHF world

The VHF two-modem world (`SKW_ST2_BAND=vhf`, the first test of the VHF
profile through the whole worker) never connected. The worker's
Upconverter and Downconverter are members, built before its
`rebuildEngine()` selects the band, and a band switch only reset their
phase: a vhf-fm modem kept HF's 1500 Hz centre and the converter filters
of whatever band was active at construction. Its bursts were the VHF
baseband shifted to 1500 Hz and folded over 0 Hz, and its receiver kept
HF's +-1.4 kHz filter, so the real `skywave` binary has never been able to
send or receive VHF. Nothing caught it: `test_vhf` pairs engines at
baseband, with no converters at all.
`rebuildEngine()` now rebuilds both converters after selecting the band,
and `main` selects the band before building anything. Test: `test_ptt`
`vhf_worker_transmits_decodable_vhf`.

## Two-modem endurance through the FM radio (ST2 VHF world)

900 sim-s each, `SKW_ST2_BAND=vhf`, both directions loaded (floor: one
way), byte-exact verification throughout:

| Scenario | Seed | A->B | B->A | Reconnects |
|---|---|---|---|---|
| churn (CNR 45 -> 0 -> 45, static/walk/mobile flutter) | 11 | 99 KB | 94 KB | 0 |
| churn | 12 | 198 KB | 196 KB | 0 |
| churn | 13 | 186 KB | 165 KB | 0 |
| floor (22 dB CNR, 15 Hz flutter) | 1 | 9.1 KB | - | 2 |
| floor | 2 | 17.7 KB | - | 2 |
| floor | 3 | 8.5 KB | - | 2 |

All pass. The floor sits 2 dB above M4's mobile-flutter floor: sessions
drop in deep fades and the caller reconnects, as specified.
