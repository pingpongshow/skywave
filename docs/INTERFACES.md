# Skywave External Interfaces

## Overview

Three localhost TCP ports plus PTT keying. Radio control (frequency, mode,
rig CAT) is **not** part of the modem — it belongs to the companion app,
which may also host a TCP PTT server the modem keys through.

| Port | Default | Purpose |
|---|---:|---|
| data | `9100` | application byte stream |
| control | `9101` | line-oriented commands, responses, notifications |
| voice | `9102` | framed PCM voice media |

Bind address defaults to `127.0.0.1`. One client per port.

## Data Port

- bytes written → queued for transmission (coalesce window, then the
  payload pipeline in [PROTOCOL.md](PROTOCOL.md))
- received payload bytes → forwarded in order
- when monitor mode is on, decoded third-party frames are also emitted as
  tagged binary records:

```text
byte 0     tag 0xFB
byte 1     frame_type
byte 2     family (0=WIDE 1=MID 2=NARROW 3=ULTRA)
byte 3     mode (ladder index)
bytes 4-6  source callsign hash (24-bit BE)
bytes 7-9  destination hash (24-bit BE; broadcast = 0xFFFFFF)
bytes 10-11 payload length (BE)
bytes 12.. payload bytes
```

(The record self-describes type/family/mode — tooling never needs the
control port to interpret it.)

Binary monitor records are emitted **only while no session is CONNECTED**:
interleaving tagged records with the session's raw payload stream would be
ambiguous (payload bytes can contain `0xFB`, and a client that loses record
framing cannot resynchronize). While connected, monitoring remains fully
available via the control-port `MON:` lines.

## Control Port

One command per line (`\n` terminated), case-insensitive verbs, single-line
responses. Responses start with the verb or `ERROR:<reason>`. Asynchronous
notifications start with `EVT:` or `MON:` and may interleave between
command/response pairs (clients must dispatch on prefix).

### Command Index

#### Identity and Session

| Command | Meaning |
|---|---|
| `MYCALL [call]` | set/query callsign |
| `CONNECT <call>` | initiate connection |
| `DISCONNECT` | orderly teardown |
| `ABORT` | immediate teardown, no `DISC` exchange |
| `LISTEN ON\|OFF` | accept incoming calls |
| `CQ` | transmit CQ, enter listening |
| `BEACON` | transmit telemetry beacon |
| `BCAST <hex>` | transmit broadcast data frame |
| `CWID ON\|OFF` | post-disconnect CW ID |

#### Mode and Link

| Command | Meaning |
|---|---|
| `FAMILY [WIDE\|MID\|NARROW\|ULTRA]` | set/query bandwidth family (IDLE/LISTENING only; `ULTRA` requires `ultra_enable` in config) |
| `MODE [M4-M13\|AUTO]` | pin or free the runtime mode |
| `MAXMODE [M4-M13]` | cap AMC promotion |
| `CPMODE [AUTO\|SHORT\|MEDIUM\|LONG]` | cyclic prefix policy |
| `HARQ [AUTO\|CHASE\|IR]` | HARQ mode |
| `CHANMEM ON\|OFF` | channel memory |
| `PROBE ON\|OFF` | probe-before-promote |
| `MASKING ON\|OFF` | carrier masking |
| `RESUME ON\|OFF` | session resume |

#### Status and Observability

| Command | Meaning |
|---|---|
| `STATUS` | one-line aggregated snapshot |
| `STATUS JSON` | same snapshot as a single-line JSON object |
| `STATE` | FSM state only |
| `BUSY` | channel-busy state |
| `NOISEFLOOR` | latest noise/CCA measurement |
| `BUFFER` | queued TX byte count |
| `AUDIOLEVEL` | input/output level meters (dBFS) |
| `MONITOR ON\|OFF` | passive frame monitoring |
| `TXTEST [seconds\|OFF]` | transmit a representative OFDM calibration signal (IDLE only, max 30 s) |
| `VERSION` | version string |

#### PTT

| Command | Meaning |
|---|---|
| `PTTMETHOD [NONE\|RTS\|DTR\|VOX\|TCP]` | keying method |
| `PTTPORT <path-or-host[:port]>` | serial device or TCP endpoint |
| `PTTBAUD <baud>` | serial baud |
| `PTTLEAD [0-5000]` | key→audio delay ms |
| `PTTTAIL [0-5000]` | audio→unkey delay ms |

#### Security (crypto builds only; [SECURITY.md](SECURITY.md))

| Command | Meaning |
|---|---|
| `ENCRYPTION ON\|OFF` | request encryption for future connects |
| `ENCRYPTKEY <hex>` | load PSK material |
| `KEYROTATE` | rotate epoch on live encrypted session |
| `SECURITY` | auth/encrypt/epoch summary |

#### Voice ([VOICE.md](VOICE.md))

| Command | Meaning |
|---|---|
| `VOICE [OFF\|CONNECTED\|BROADCAST]` | voice mode |
| `VOICECODEC [AUTO\|700C\|1300\|2400\|3200]` | codec selector |
| `VOICESTART` / `VOICESTOP` | open/close voice state |
| `VOICEPTT ON\|OFF` | talkspurt control |
| `VOICESTATUS` | voice state snapshot |

### `STATUS` Fields

```text
STATUS STATE=<fsm> CALL=<my> REMOTE=<peer|-> FAMILY=<f> MODE=<Mx|AUTO(Mx)>
NET=<bps> SNR=<local> RSNR=<remote|-> CFO=<tx_align_hz> RETRY=<ratio>
BUSY=<0|1> LISTEN=<0|1> COMPRESS=<0|1> CRYPTO=<0|1> EPOCH=<n> RESUME=<0|1>
MASK=<count> VOICE=<off|state> TXBUF=<bytes> FOK=<n> FFAIL=<n>
```

`CFO` is the current TX frequency-alignment correction
([PROTOCOL.md](PROTOCOL.md)); a persistently large value means the dial
needs retuning (an `EVT:CFO_WARN` fires past ±40 Hz).

`STATUS JSON` carries the same keys lower-cased in one JSON object — the
GUI and companion app consume this form.

### Notifications

```text
EVT:STATE <IDLE|LISTENING|CALLING|CONNECTING|CONNECTED|DISCONNECTING>
EVT:CONNECTED <remote_call> <family> <initial_mode>
EVT:DISCONNECTED <reason>
EVT:MODE <Mx> <net_bps>
EVT:SNR <local_db> <remote_db|->
EVT:BUSY <0|1>
EVT:TXCOMPLETE
EVT:FILE_RX <name> <size> <crc32ok>
EVT:FILE_ERROR <name> <reason>
EVT:VOICE <state>
MON:<src>:<dst|*>:<frame_type>:<family>:<Mx>:<hex_payload>
```

`MON:` lines carry the real ladder mode — there is no legacy level field
anywhere in the interface.

## Voice Port

Framed PCM records, both directions (see [VOICE.md](VOICE.md) for the codec
path):

- media: `8 kHz` mono `PCM16LE`
- record: version (u8=1), type (u8: 1=uplink, 2=downlink), length (u16 BE),
  sequence (u32 BE), sample counter (u64 BE), PCM payload
- modem performs all Codec2 encode/decode internally; the socket only ever
  carries PCM

## PTT

The modem owns burst-accurate keying only:

| Method | Behavior |
|---|---|
| `NONE` | no keying (external control) |
| `RTS` / `DTR` | serial control line |
| `VOX` | rely on radio VOX |
| `TCP` | send `KEY\n` / `UNKEY\n` to a companion-app PTT server |

Sequence per burst: assert → wait `ptt_lead_ms` (default 50) → audio →
wait `ptt_tail_ms` (default 20) → release. A TX watchdog
([DSP.md](DSP.md)) force-releases PTT if any transmission exceeds
`tx_watchdog_s`.

### TX Drive Calibration

Overdriven TX audio into the rig's ALC is the most common soundcard-modem
failure in the field. The supported calibration procedure:

1. `TXTEST 10` transmits a representative full-drive OFDM signal (same
   PAPR statistics as data frames) while the operator adjusts audio-out
   gain / rig drive for zero-to-minimal ALC action
2. `AUDIOLEVEL` and the GUI meters show output level; the GUI settings
   page documents the target ("raise drive until ALC just moves, back off
   ~10%")
3. `TXTEST` is refused unless `IDLE`, is hard-capped at 30 s, and honors
   the TX watchdog

The peer-side check: `RSNR` in `STATUS` reading far below `SNR` symmetry
expectations, or the peer's rx-quality `DEGRADED` flag while local SNR
looks fine, are the classic signatures of TX overdrive — noted in the
troubleshooting section of the GUI help.

## Companion App Split

| Component | Owns |
|---|---|
| Skywave modem | waveform TX/RX, sessions, timing, audio, `KEY`/`UNKEY` |
| Companion app | rig CAT control, frequency/mode, PTT translation, band planning, logging UI |

The companion app may additionally:

- push dial frequency to the modem (`FREQINFO <hz>` control command,
  informational — it keys channel-memory buckets, nothing else)
- consume `STATUS JSON` polling or `EVT:` streams for its UI

## Built-in GUI

SDL2 + Dear ImGui application in the same process (`--gui`), rendering from
the telemetry snapshot (never touching modem internals):

- waterfall (RX spectrum, family band overlay, mask overlay)
- constellation / despread-symbol view
- ladder panel: current mode, AMC inputs (SNR, retry, selectivity), floor
  markers
- session panel: state, peer, rates, ARQ window occupancy
- settings pages mirroring the INI schema ([CONFIGURATION.md](CONFIGURATION.md))

Everything the GUI can change routes through the same command dispatcher as
the control port — the GUI is a control-port client that happens to live
in-process.
