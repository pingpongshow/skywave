# Skywave Configuration

## Precedence

1. compiled defaults
2. INI file (`--config <path>`)
3. CLI overrides
4. runtime changes (control port / GUI)

Runtime changes persist back to the INI on clean exit when `--config` was
given.

### Saving on exit: three-way merge

The exit-time save does not blindly overwrite the file, so the INI can be
hand-edited while the modem runs. Three versions take part, compared key
by key (`section.key`):

- **base**: the file as it was loaded at start-up (before CLI overrides);
- **live**: the configuration the modem is running at exit (control
  port / GUI changes, plus CLI overrides);
- **disk**: the file as it is on disk at exit.

For each key:

| live vs base | result |
|---|---|
| changed (set at runtime, or by a CLI override) | the live value is written |
| unchanged | the disk value is written, verbatim (a manual edit survives) |

- **Conflict:** if a key was changed at runtime *and* edited on disk to a
  different value, **the runtime value wins** and a `WARN` naming the key
  is logged (the value itself is not logged: it may be the PSK). Re-apply
  the manual edit after exit if it was the one you wanted.
- Keys the operator **added** to the file while running (known or
  unknown) are kept. An unknown key the operator **deleted** while running
  stays deleted; a deleted schema key is written back with the running
  value (the saved file always carries the full schema).
- Unknown keys are preserved as on any save.
- If the file **no longer exists or cannot be parsed** (no `key = value`
  lines at all), the live configuration is written in full, as before.
- The write is atomic (temp file + `rename`) and the file is mode `0600`
  because it holds `crypto.psk`, exactly as for a plain save.

A CLI override differs from the loaded base, so it is persisted on exit
(and wins over an edit of the same key made on disk meanwhile).

## CLI

| Option | Meaning |
|---|---|
| `--gui` | launch GUI |
| `--callsign <CALL>` | local callsign (same rule as the INI key: validated, stored uppercase; invalid = placeholder + `WARN`) |
| `--config <path>` | INI load/save path |
| `--family <wide\|mid\|narrow\|ultra>` | bandwidth family |
| `--data-port / --ctrl-port / --voice-port <n>` | TCP ports (9100/9101/9102) |
| `--audio-in / --audio-out <index\|name>` | PortAudio device, by name (preferred) or index, as the INI keys below |
| `--list-audio` | list devices and exit |
| `--replay <wav>` | feed RX from a WAV file (test/debug) |
| `--record <wav>` | record **TX** audio to WAV while replaying (the file backend drains the TX ring; it does not capture RX). Requires `--replay`: file mode has no control port, so without a replay there is nothing to run and `skywave` exits with an error |
| `--log-level <0-4>` | 0=TRACE .. 4=ERROR |
| `--log-file <path>` | log to file |
| `--version` / `--help` | |

## INI Schema

### `[station]`

| Key | Default | Meaning |
|---|---|---|
| `callsign` | `N0CALL` | 3–12 characters `[A-Z0-9/]`, stored uppercase. `N0CALL` (or empty) is the placeholder: every transmitting command — `CONNECT`, `CQ`, `BEACON`, `BCAST`, `TXTEST`, `VOICESTART`, `VOICEPTT ON` — is refused with `ERROR:STATE` and nothing is keyed, and incoming calls are not answered. An invalid value loads as the placeholder with a `WARN` |
| `cw_id` | `false` | CW ID after disconnect / periodic in-session |
| `cw_wpm` | `20` | CW ID speed |

### `[audio]`

| Key | Default | Meaning |
|---|---:|---|
| `input_device` / `output_device` | `-1` | device **name** (preferred, e.g. `AetherSDR TX`; exact match, else the first name starting with it) or PortAudio index (−1 = default). Indices shift whenever any audio device appears or disappears, so a saved index can silently point at another device; a name is resolved each time the stream opens, and a missing named device stops start-up instead of falling back. The GUI saves its choice by name. |
| `input_gain_db` / `output_gain_db` | `0.0` | trims. At `output_gain_db = 0` a data burst leaves at 0.22 RMS (−13.2 dBFS) with peaks near −1 dBFS; positive trims clip (samples are limited at full scale and a `tx: ... clipped` warning is logged). Set drive with the radio, not by raising this. Applied once, by the worker (before 2026-10 the device backend applied it a second time). |

(Sample rate is fixed at 48000 and not configurable — the numerology
depends on it.)

### `[tcp]`

| Key | Default |
|---|---|
| `data_port` | `9100` |
| `ctrl_port` | `9101` |
| `voice_port` | `9102` |

The ports always bind `127.0.0.1` (loopback only); there is no bind
address setting.

### `[ptt]`

| Key | Default | Meaning |
|---|---|---|
| `method` | `none` | `none/rts/dtr/vox/tcp` |
| `target` | empty | serial path or `host[:port]` |
| `baud` | `38400` | serial baud |
| `lead_ms` | `50` | key → audio |
| `tail_ms` | `20` | trailing silence after the burst; the release additionally waits the device's reported output latency + 10 ms |

### `[modem]`

| Key | Default | Meaning |
|---|---|---|
| `band` | `hf` | band profile: `hf` or `vhf-fm`. VHF-FM ships (176-carrier family through a 9 kHz FM channel, modes to M14 256QAM r5/6, measured 25.5 kbps net) and has its own conformance gate; it is a different radio, not a mode of the HF one. Set the rig's data-port level so bursts peak at its maximum deviation; M14 needs a port flat below 300 Hz. See [WAVEFORM.md](WAVEFORM.md). |
| `family` | `wide` | `wide` / `mid` / `narrow` / `ultra` (`ultra` needs `ultra_enable`) |
| `ultra_enable` | `false` | permit the opt-in 2.7 kHz ULTRA family (radio must pass ~100–2900 Hz) |
| `family_fallback` | `true` | narrower-family fallback, for every family: a caller sends the second half of its `CALL` attempts one family narrower (ULTRA→WIDE→MID→NARROW), and a listener also decodes calls one family narrower than its own. `false` keeps calls and listening on the configured family only |
| `css_tier` | `true` | CSS deep tier (`M0`–`M3`) and the deep handshake below the OFDM floor, advertised as `CAP_CSS_TIER`; HF only (ignored on `vhf-fm`). `false` keeps sessions on the OFDM ladder |
| `mode` | `auto` | `auto` or a pinned OFDM mode: `m4..m13` on `hf`, `m4..m14` on `vhf-fm` (the range `MODE` accepts). Anything else — including the CSS rungs `m0..m3` — is refused with a `WARN` and stays `auto` |
| `max_mode` | `m13` | AMC promotion cap, same range as `mode`; `auto` = the band's highest mode (`m13` HF, `m14` VHF-FM); values outside the range clamp into it with a `WARN` |
| `cp_mode` | `auto` | `auto/short/medium/long` |
| `listen` | `true` | start listening |
| `max_retries` | `10` | ARQ retry cap per frame |
| `harq` | `ir` | `ir`: offer incremental redundancy (`CAP_HARQ_IR`; a peer without it gets chase combining); `chase`: chase combining only. Older files' `auto` loads as `ir` (it behaved identically) with a `WARN` |
| `channel_memory` | `true` | reconnect seeding; OFF disables both seeding and recording. Store: `~/.skywave/chanmem.bin`, overridden by `SKW_CHANMEM_PATH` (empty value = no persistence) |
| `carrier_masking` | `true` | mask exchange |
| `resume` | `true` | session resume |
| `tx_watchdog_s` | `120` | hard cap on continuous TX seconds |

Scheduled turn-taking (`CAP_TURN_SCHED`), TX frequency alignment and
sideband-inversion correction are always on and have no keys.

### `[crypto]` (crypto builds only)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | request encryption on future connects |
| `psk` | empty | 64 hex chars (32 bytes), either case. Anything else loads as **no key** with a `WARN` (the value is not logged) |

### `[voice]` (voice builds only)

| Key | Default | Meaning |
|---|---|---|
| `codec` | `auto` | `auto/700c/1300/2400/3200`: the startup `VOICECODEC` (a `VOICECODEC` change is saved on exit). Parsed but ignored before v1.2.7 |
| `broadcast_mode` | `m9` | mode for broadcast voice (`m8`..., or `auto` = the most robust mode with a real-time voice plan). Parsed but ignored before v1.2.7 |

### `[gui]`

| Key | Default |
|---|---|
| `enabled` | `false` |
| `width` / `height` | `0` / `0` | window size in points; `0` = fit to the display (1400 x 880, or 92 % of a smaller display). A request is kept on the display and is never smaller than the layout minimum, 980 x 640 |

The waterfall and constellation are always drawn (they have no keys).

### Retired keys

`tcp.bind_addr`, `modem.impulse_blanker`, `modem.multi_notch`,
`modem.freq_align`, `modem.invert_correct`, `modem.turn_taking`,
`modem.probe_promote`, `modem.pre_tx_cca`, `gui.waterfall` and
`gui.constellation` were written by earlier builds and are no longer
used (most never did anything; `turn_taking = false` selected a
legacy scheduler that is no longer selectable). They load with a `WARN`, are dropped, and
disappear from the file at the next save.

### `[log]`

| Key | Default |
|---|---|
| `level` | `2` (INFO) |
| `file` | empty |

## Example INI

```ini
[station]
callsign = W1AW
cw_id = true

[audio]
input_device = DAX RX 1
output_device = DAX TX

[tcp]
data_port = 9100
ctrl_port = 9101
voice_port = 9102

[ptt]
method = tcp
target = 127.0.0.1:4533

[modem]
family = wide
mode = auto
listen = true

[crypto]
enabled = false
psk =

[gui]
enabled = true
```

## Environment Variables

| Variable | Default | Meaning |
|---|---|---|
| `SKW_ADVISOR` | `shadow` | `off` / `shadow` / `assist` |
| `SKW_ADVISOR_AMC_CONF` | `0.6` | AMC-domain confidence floor |
| `SKW_ADVISOR_CFO_CONF` | `0.6` | CFO-domain confidence floor |
| `SKW_ADVISOR_MASK_CONF` | `0.55` | mask-domain confidence floor |
| `SKW_CHANMEM_PATH` | `~/.skywave/chanmem.bin` | channel-memory store; set empty (or `-`) to run without persistence, which is what the test suite does |
| `SKW_CHANCLASS` | (unset) | `off` pins the channel classifier to `UNKNOWN`, i.e. always-fading floors, for A/B diagnosis |

The Advisor deliberately has no INI/GUI/TCP surface
([LINK_ADAPTATION.md](LINK_ADAPTATION.md)).

### Diagnostic A/B levers

These exist so a shipped default can be measured against its
alternative on a live link, not because either setting is a supported
configuration. Every one of them defaults to the behaviour the modem
ships with; setting one selects the losing side of an experiment that
has already been run and recorded in ROADMAP.md. **Do not set these in
production.** They are listed here because an undocumented environment
variable that changes MAC behaviour is worse than a documented one.

| Variable | Default | Selects when set |
|---|---|---|
| `SKW_TURN_HOLD_RTTS` | `4.0` | Minimum turn hold in round-trip times |
| `SKW_TURN_HOLD_MIN_SEC` | `6.0` | Absolute floor on the minimum turn hold, in seconds |
| `SKW_DRAIN_CAP_MULT` | `2.0` | Cap on drain-before-grant, in handoff intervals |
| `SKW_HF_WINDOW` | `16` | ARQ window for M10 and above |
| `SKW_CFO_ACK` | (unset = on) | `0` disables the standalone reverse-path CFO `ACK` (A/B) |
| `SKW_ROBUST_FPB` | (per mode) | Frames per burst on the robust tier |
| `SKW_ROBUST_AIR_CAP` | `10.0` | Airtime clamp, in seconds, on robust-tier codeword growth |
| `SKW_DATA_STARVE_X` | `3.0` | Data-starvation backstop, in ARQ timeouts |
| `SKW_NO_NAK` | (unset) | Disables negative acknowledgements |
| `SKW_NO_ACK_PIGGYBACK` | (unset) | Disables ACK piggybacking on data frames |
| `SKW_NO_LOSSY_CAP` | (unset) | Removes the lossy-channel cap on the seal |
| `SKW_RXBAND_MARGIN_HZ` | `50` | RX band-edge margin |
| `SKW_TXBAND_MARGIN_HZ` | `40` | TX band-edge margin |
| `SKW_TX_IMPAIR` | (unset) | Injects transmit impairments for testing |
| `SKW_AMC_PROMOTE_MARGIN` / `SKW_AMC_DEMOTE_MARGIN` | `-1.5` / `-3.5` | AMC margins relative to the active floor table (measurement A/B) |
| `SKW_AMC_PROMOTE_MARGIN_HIGH` | `0.5` | promote margin into M11-M13 (measurement A/B; LINK_ADAPTATION.md) |
| `SKW_CSS_SYNC_FACTOR` | `3.0` | CSS sync acceptance, correlation peak over profile median (was 3.5; A/B) |
| `SKW_CSS_BRANCH_MIN_CHIPS` | `2` | branch check applies where a 6.25 Hz branch moves the SF8 sync peak by more than this many chips |
| `SKW_CSS_TAP_SPAN_MS` | `3` | CSS RAKE tap search span either side of the anchor, at least this many ms (and >= 3 chips) |
| `SKW_CSS_FAR_TAP_PEAK` / `SKW_CSS_FAR_TAP_NOISE` | `0.35` / `3.5` | a CSS tap beyond 3 chips must reach this fraction of the peak and this multiple of the noise median |

Receiver tuning (the in-frame FLL and its drift retry are always on;
these only change their gains, for measurement):

| Variable | Default | Meaning |
|---|---|---|
| `SKW_FLL_GAIN_FAST` / `SKW_FLL_GAIN_ROBUST` | `0.2` / `0.1` | in-frame FLL gain on the fast / robust tier |
| `SKW_FLL_GAIN_DRIFT` | `0.3` | FLL gain of the re-walk of a failed OFDM frame (extreme in-frame drift) |

Parsing rule for every variable read through `core/env.hpp` (all
receiver/PHY knobs above and the PHY debug flags): read once and cached;
unset or empty means the default; a flag set to `0`, `off` or `false`
(any case) is off; a number that does not parse falls back to the
default.

Test-harness only: `SKW_ST2_AMBIENT` (ST2 ambient floor amplitude, `0` =
digital silence), `SKW_ST3_LOGDIR` (ST3 log directory).

### GUI debugging

Read by `skywave --gui`:

| Variable | Default | Meaning |
|---|---|---|
| `SKW_GUI_SCREENSHOT` | (unset) | save the window as BMP to this path after ~2 s |
| `SKW_GUI_SCRIPT` | (unset) | drive the real widgets from a script file, one `<frame> <verb> [args]` per line: `move X Y`, `click X Y` (window points), `type TEXT`, `key enter\|backspace\|selectall`, `shot FILE.bmp`, `quit`. Frames are display refreshes (120/s on a ProMotion screen). End-to-end GUI checks without accessibility permissions |

### Debug logging

Each of these enables one category of trace output on stderr. They are
read once and cost nothing when unset. `SKW_RX_DEBUG`, `SKW_CFO_DEBUG`,
`SKW_EQ_DEBUG` and `SKW_SOUNDER_DEBUG` (tools/onair sounder) follow the
`core/env.hpp` rule above (`0`/`off`/`false` = off); the others are on
when set to any value.

`SKW_AMC_DEBUG`, `SKW_ARQ_DEBUG`, `SKW_CCA_DEBUG`, `SKW_CHANCLASS_DEBUG`,
`SKW_CP_DEBUG`, `SKW_CSS_DEBUG`, `SKW_DELIV_DEBUG`, `SKW_EMIT_DEBUG`,
`SKW_EQ_DEBUG`, `SKW_FRAME_DEBUG`, `SKW_MASK_DEBUG`, `SKW_MODE_DEBUG`,
`SKW_PROTO_DEBUG`, `SKW_PUMP_DEBUG`, `SKW_REPLAY_DEBUG`, `SKW_RX_DEBUG`,
`SKW_RXLADDER_DEBUG`, `SKW_TURN_DEBUG`, `SKW_TX_DEBUG`, `SKW_TXA_DEBUG`,
`SKW_CFO_DEBUG` (per-frame CFO branch candidates).

`SKW_REPLAY_DEBUG` traces the peer-restart replay bookkeeping: how many
message-end sequences are outstanding, how many messages are held
unconfirmed and their size, and the ARQ's oldest in-flight sequence.

### Test-harness only

Read by the test and soak binaries, never by `skywave` itself.

| Variable | Default | Meaning |
|---|---|---|
| `SKW_SOAK` | (unset) | Enables the long-running soak paths in the ST2 suites |
| `SKW_SOAK_SCENARIO` | (harness) | Scenario name: churn, floor, blackout |
| `SKW_SOAK_SEED` | (harness) | Seed; soak results are chaotic, so compare seed spreads, never single runs |
| `SKW_SOAK_SIMSEC` | (harness) | Simulated seconds per soak run |
| `SKW_SOAK_SNR` | (harness) | Channel SNR for the soak world |
| `SKW_SOAK_WINDOW` | `4096` | Offered-load feed window |
| `SKW_SOAK_COUPLED` | (unset) | Couples the two directions of the ST2 world |
| `SKW_SOAK_DEBUG` | (unset) | Soak trace output |
| `SKW_FULL_DUPLEX` | (unset) | Restores the old full-duplex ST2 world; the default is half duplex, which is what turn-taking exists to handle. `SKW_HALF_DUPLEX` is still accepted as a no-op. |
| `SKW_MONKEY_SIMSEC` | `900` | Phase-1 duration of the monkey soak |
| `SKW_HARQ_TRIALS` | `12` / `10` | Trial counts in `test_harq` |
| `SKW_GUI_WINDOW` | (unset) | Runs the real-window GUI test; headless path only when unset |
| `SKW_NO_KEEPALIVE` | (unset) | A/B: disable session keepalives (PROTOCOL.md) |
| `SKW_VOICE_ARTIFACTS` | (unset) | Directory for decoded voice artifacts from `test_voice` |
| `SKW_ST3_SOAK` | `60` | ST3 hardware soak duration in seconds |
| `SKW_ST3_IMPAIR` | (unset) | ST3 impairment string |
| `SKW_ST3_LOGDIR` | `.` | Where the ST3 scripts write their per-station logs |

`SKW_HARDWARE_TESTS` is **not** an environment variable. The ST3 gate
is a CMake cache option (`cmake -B build -DSKW_HARDWARE_TESTS=ON`),
because a test configured `DISABLED` cannot be enabled from the
environment at ctest time. See RELEASE.md gate 4.

## Validation Rules

- callsign: 3–12 chars `[A-Z0-9/]` (the rule the link layer applies to
  every frame), stored uppercase; `N0CALL`/empty refuses every TX path
- `mode` / `max_mode` / `MODE` / `MAXMODE`: the band's OFDM modes only
- `crypto.psk`: 64 hex digits or nothing
- `family` changes only in `IDLE`/`LISTENING`
- crypto keys rejected while `CALLING`/`CONNECTING`/`CONNECTED`
- unknown INI keys warn and are preserved on save (forward compatibility),
  including through the exit-time merge; retired keys (above) warn and
  are dropped
