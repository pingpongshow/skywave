# Skywave Configuration

## Precedence

1. compiled defaults
2. INI file (`--config <path>`)
3. CLI overrides
4. runtime changes (control port / GUI)

Runtime changes persist back to the INI on clean exit when `--config` was
given.

## CLI

| Option | Meaning |
|---|---|
| `--gui` | launch GUI |
| `--callsign <CALL>` | local callsign |
| `--config <path>` | INI load/save path |
| `--family <wide\|mid\|narrow\|ultra>` | bandwidth family |
| `--data-port / --ctrl-port / --voice-port <n>` | TCP ports (9100/9101/9102) |
| `--audio-in / --audio-out <index>` | PortAudio device indices |
| `--list-audio` | list devices and exit |
| `--replay <wav>` | feed RX from a WAV file (test/debug) |
| `--record <wav>` | record RX input to WAV |
| `--log-level <0-4>` | 0=TRACE .. 4=ERROR |
| `--log-file <path>` | log to file |
| `--version` / `--help` | |

## INI Schema

### `[station]`

| Key | Default | Meaning |
|---|---|---|
| `callsign` | `N0CALL` | placeholder blocks all TX |
| `cw_id` | `false` | CW ID after disconnect / periodic in-session |
| `cw_wpm` | `20` | CW ID speed |

### `[audio]`

| Key | Default | Meaning |
|---|---:|---|
| `input_device` / `output_device` | `-1` | PortAudio index, −1 = default |
| `input_gain_db` / `output_gain_db` | `0.0` | trims |

(Sample rate is fixed at 48000 and not configurable — the numerology
depends on it.)

### `[tcp]`

| Key | Default |
|---|---|
| `data_port` | `9100` |
| `ctrl_port` | `9101` |
| `voice_port` | `9102` |
| `bind_addr` | `127.0.0.1` |

### `[ptt]`

| Key | Default | Meaning |
|---|---|---|
| `method` | `none` | `none/rts/dtr/vox/tcp` |
| `target` | empty | serial path or `host[:port]` |
| `baud` | `38400` | serial baud |
| `lead_ms` | `50` | key → audio |
| `tail_ms` | `20` | audio → unkey |

### `[modem]`

| Key | Default | Meaning |
|---|---|---|
| `band` | `hf` | band profile; `hf` is the only v1 value (`vhf-fm` reserved, [WAVEFORM.md](WAVEFORM.md)) |
| `family` | `wide` | `wide` / `mid` / `narrow` / `ultra` (`ultra` needs `ultra_enable`) |
| `ultra_enable` | `false` | permit the opt-in 2.7 kHz ULTRA family (radio must pass ~100–2900 Hz) |
| `family_fallback` | `true` | unanswered ULTRA calls retry at WIDE |
| `mode` | `auto` | `auto` or `m4..m13` pin |
| `max_mode` | `m13` | AMC promotion cap |
| `cp_mode` | `auto` | `auto/short/medium/long` |
| `listen` | `true` | start listening |
| `max_retries` | `10` | ARQ retry cap per frame |
| `harq` | `auto` | `auto/chase/ir` |
| `channel_memory` | `true` | reconnect seeding; OFF disables both seeding and recording. Store: `~/.skywave/chanmem.bin`, overridden by `SKW_CHANMEM_PATH` (empty value = no persistence) |
| `probe_promote` | `true` | probe-before-promote |
| `carrier_masking` | `true` | mask exchange |
| `turn_taking` | `true` | scheduled ISS/IRS turn-taking (`CAP_TURN_SCHED`); `false` = legacy occupancy-gate scheduler, needed only to call a pre-`CAP_TURN_SCHED` build |
| `resume` | `true` | session resume |
| `pre_tx_cca` | `true` | originate-time busy gate |
| `impulse_blanker` | `false` | RX impulse blanker |
| `multi_notch` | `false` | RX adaptive notch bank |
| `freq_align` | `true` | TX frequency alignment to peer-reported CFO |
| `invert_correct` | `true` | auto-correct sideband-inverted RX |
| `tx_watchdog_s` | `120` | hard cap on continuous TX seconds |

### `[crypto]` (crypto builds only)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | request encryption on future connects |
| `psk` | empty | 64 hex chars (32 bytes) |

### `[voice]` (voice builds only)

| Key | Default | Meaning |
|---|---|---|
| `codec` | `auto` | `auto/700c/1300/2400/3200` |
| `broadcast_mode` | `m9` | manual mode for broadcast voice |

### `[gui]`

| Key | Default |
|---|---|
| `enabled` | `false` |
| `width` / `height` | `520` / `720` |
| `waterfall` | `true` |
| `constellation` | `true` |

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
input_device = 1
output_device = 2

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

## Validation Rules

- callsign: 3–12 chars `[A-Z0-9/]`; `N0CALL`/empty blocks TX paths
- `family` changes only in `IDLE`/`LISTENING`
- crypto keys rejected while `CALLING`/`CONNECTING`/`CONNECTED`
- unknown INI keys warn and are preserved on save (forward compatibility)
