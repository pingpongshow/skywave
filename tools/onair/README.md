# On-air test tooling

One-way PHY tests through a real transmitter, receiver and ionosphere.
The procedure, pass criteria and what each run feeds are in
[docs/ONAIR_TEST_PLAN.md](../../docs/ONAIR_TEST_PLAN.md). Nothing here is
part of the modem: `onair_sounder` links the PHY library like `bertool`,
and `flexptt.py` is a standalone script.

Build: `cmake --build build --target onair_sounder` (needs `BUILD_TOOLS`,
on by default).

## Typical session

```bash
# 1. PTT server (find the radio first)
tools/onair/flexptt.py --discover
tools/onair/flexptt.py --radio 192.0.2.182 --rigctl 127.0.0.1:4532 --max-rfpower 1

# 2. Build a sounding: WIDE M4-M13, 5 rounds, CW ID
build/onair_sounder gen --call N0CALL --reps 5 --out snd.wav

# 3. Start the receiver recording(s), then transmit
tools/onair/kiwirec.py --host 192.0.2.210 --freq 14105.0 --seconds 240 --out kiwi_rec.wav &
build/onair_sounder devices
build/onair_sounder play --wav snd.wav --device "AetherSDR TX" --ptt 127.0.0.1:4600

# 4. Score each recording
build/onair_sounder score --wav kiwi_rec.wav --manifest snd.manifest --csv run1.csv \
    --rate $(cat kiwi_rec.rate)
```

## Notes

- **Level.** `gen` defaults to `--gain-db -10`, which puts the peak at
  about −1 dBFS: the same level the modem transmits at
  `output_gain_db = 0`. Set drive with the radio's DAX TX gain and RF power, not
  by raising this: above 0 dBFS the interface clips.
- **Segments.** Frames are grouped into keyed segments (`--segment 30`
  s) with unkeyed pauses (`--pause 3` s). This bounds continuous key-down
  and stays under the bridge's 45 s watchdog. `play --from-segment N`
  resumes an interrupted run.
- **Recordings.** Any sample rate, mono (channel 0 of a stereo file).
  Use `--rate` when the receiver's true rate differs from the WAV header
  (some KiwiSDRs run near 12001 Hz). Use `--shift` when the receiver was
  tuned off the transmit dial.
- **Frames.** Frame type 61 (unassigned), so a Skywave modem listening on
  the channel ignores them. The payload is a known sequence keyed by
  frame number, so the scorer needs only the manifest.
- **Clock figure.** Fitted within keyed segments, because the player's
  PTT waits add time between segments that the WAV doesn't contain, and
  split again at any timing jump. Expect ±1–15 ppm resolution per run.
- **Timing jumps.** `score` lists steps of more than 5 ms between
  consecutive frames in one keyed segment: audio inserted (an underrun) or
  removed upstream of the transmitter. These are TX-chain glitches.
  `SKW_SOUNDER_DEBUG=1` lists every detection and decode.
- **CW ID** goes at the end of the sequence, plus at a segment boundary
  before each 10-minute mark in longer runs. `--no-id` is only for runs
  that are not radiated (into a dummy load or a loopback cable).
- **Station config.** `station-flex6700.ini` holds the measured settings
  for N0CALL's FLEX-6700/AetherSDR station. Copy it out of the repo
  before use, because the modem rewrites its `--config` file on exit.
- **`kiwirec.py`** is receive-only and uses only the Python standard
  library. It tunes USB at the dial frequency with AGC off (`--gain`,
  default 50) and compression off, and writes the true sample rate to
  `<out>.rate`.
- **`flexptt.py` backends.** `--radio` uses the SmartSDR API (TCP 4992,
  `xmit 1/0`), which runs alongside SmartSDR for Mac. `--kenwood
  host:port` speaks a generic Kenwood TCP CAT (`TX;`/`RX;`). `--dry-run`
  only logs. The modem can use the same server: `PTTMETHOD TCP`,
  `PTTPORT 127.0.0.1:4600`.
