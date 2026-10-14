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
| `--record <wav>` | record **TX** audio to WAV (the file backend drains the TX ring; it does not capture RX) |
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
| `bind_addr` | `127.0.0.1` |

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
| `band` | `hf` | band profile: `hf` or `vhf-fm`. VHF-FM ships (176-carrier family through a 9 kHz FM channel, modes to M14 256QAM r5/6, measured 25.5 kbps net) and has its own conformance gate; it is a different radio, not a mode of the HF one. See [WAVEFORM.md](WAVEFORM.md). |
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
| `SKW_NO_TURNSCHED` | (unset) | Drops the turn-scheduling capability from the handshake, so the session falls back to pure CCA/LBT |
| `SKW_TURN_HOLD_RTTS` | `4.0` | Minimum turn hold in round-trip times |
| `SKW_TURN_HOLD_MIN_SEC` | `6.0` | Absolute floor on the minimum turn hold, in seconds |
| `SKW_DRAIN_CAP_MULT` | `2.0` | Cap on drain-before-grant, in handoff intervals |
| `SKW_HF_WINDOW` | `16` | ARQ window for M10 and above |
| `SKW_MID_WINDOW` | `4` | ARQ window for M7–M9 |
| `SKW_RETX_FILL` | `1` | M7–M9 containers gather due retransmits and top up with new segments; `0` one retransmit per container (v1.2.2 behaviour), `2` at every rung (A/B) |
| `SKW_CFO_ACK` | (unset = on) | `0` disables the standalone reverse-path CFO `ACK` (A/B) |
| `SKW_ROBUST_FPB` | (per mode) | Frames per burst on the robust tier |
| `SKW_ROBUST_AIR_CAP` | `10.0` | Airtime clamp, in seconds, on robust-tier codeword growth |
| `SKW_DELIVER_TARGET_SEC` | `0.0` (off) | Mode-following coalesce bound; measured and left disabled |
| `SKW_DATA_STARVE_X` | `3.0` | Data-starvation backstop, in ARQ timeouts |
| `SKW_NO_NAK` | (unset) | Disables negative acknowledgements |
| `SKW_NO_ACK_PIGGYBACK` | (unset) | Disables ACK piggybacking on data frames |
| `SKW_NO_LOSSY_CAP` | (unset) | Removes the lossy-channel cap on the seal |
| `SKW_NO_APPLY_AT_ANNOUNCE` | (unset) | Applies a mode change only on ACK, never at announce |
| `SKW_PROMOTE_AT_ANNOUNCE` | (unset) | Promotes at announce; measured as anti-synergistic |
| `SKW_DUP_RETRY` | (unset) | Restores the old duplicate-retry accounting rule |
| `SKW_RXBAND_MARGIN_HZ` | `50` | RX band-edge margin |
| `SKW_TXBAND_MARGIN_HZ` | `40` | TX band-edge margin |
| `SKW_TX_IMPAIR` | (unset) | Injects transmit impairments for testing |
| `SKW_MAX_PENDING_RX` | `8` | Capture backlog cap (oldest evicted first) |
| `SKW_AMC_PROMOTE_MARGIN` / `SKW_AMC_DEMOTE_MARGIN` | `-1.5` / `-3.5` | AMC margins relative to the active floor table (measurement A/B) |
| `SKW_AMC_PROMOTE_MARGIN_HIGH` | `0.5` | promote margin into M11-M13 (measurement A/B; LINK_ADAPTATION.md) |
| `SKW_CSS_DRIFT_SEARCH` | (unset = on) | `0` disables the CSS payload drift-rate search after a failed decode (ROADMAP 8.8; A/B) |
| `SKW_CHANCLASS_SPREAD_EMA` | (unset) | set: the Severe gate reads the delay-spread EMA (pre-v1.2.2) instead of the 9-frame median (A/B) |

Phase 8 receiver stages (v1.2.0, GAP_CLOSURE_PLAN.md). Each is ON by
default; setting it to `0` restores the v1.1.0 behaviour for A/B
measurement - never needed in operation:

| Variable | Stage |
|---|---|
| `SKW_CFO_BRANCH` | CFO branch search over ±55 Hz, header-verified (R1) |
| `SKW_MULTIPATH_MARGIN` | FFT window backoff + tracker taps ahead of the window (R4) |
| `SKW_QNORM` | Kalman process noise from frame-mean channel power (R5) |
| `SKW_RTS` | RTS smoothing over each buffered walk (R6) |
| `SKW_FLL` | In-frame FLL; `SKW_FLL_GAIN_FAST` / `SKW_FLL_GAIN_ROBUST` override the 0.2 / 0.1 gains (R2) |
| `SKW_FLL_DRIFT_RETRY` | Re-walk a failed OFDM frame at the drift FLL gain (0.3; `SKW_FLL_GAIN_DRIFT` overrides) - extreme in-frame drift (v1.2.2) |

Test-harness only: `SKW_ST2_AMBIENT` (ST2 ambient floor amplitude, `0` =
digital silence), `SKW_ST3_LOGDIR` (ST3 log directory).

### Debug logging

Each of these enables one category of trace output on stderr when set
to any value. They are read once and cost nothing when unset.

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
| `SKW_VOICE_ARTIFACTS` | (unset) | Directory for decoded voice artifacts from `test_voice` |
| `SKW_ST3_SOAK` | `60` | ST3 hardware soak duration in seconds |
| `SKW_ST3_IMPAIR` | (unset) | ST3 impairment string |
| `SKW_ST3_LOGDIR` | `.` | Where the ST3 scripts write their per-station logs |

`SKW_HARDWARE_TESTS` is **not** an environment variable. The ST3 gate
is a CMake cache option (`cmake -B build -DSKW_HARDWARE_TESTS=ON`),
because a test configured `DISABLED` cannot be enabled from the
environment at ctest time. See RELEASE.md gate 4.

## Validation Rules

- callsign: 3–12 chars `[A-Z0-9/]`; `N0CALL`/empty blocks TX paths
- `family` changes only in `IDLE`/`LISTENING`
- crypto keys rejected while `CALLING`/`CONNECTING`/`CONNECTED`
- unknown INI keys warn and are preserved on save (forward compatibility)
