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
bytes 10-11 frame length (BE)
bytes 12.. the complete link frame: 8-byte envelope, body, CRC-32
           ([PROTOCOL.md](PROTOCOL.md) "Link Frame Format")
```

Byte 12 onward is the **raw link frame**, not a decoded body: a client that
wants the body parses the envelope itself (and may verify the CRC). The
callsign hashes are one-way, so the human-readable callsigns come either
from the frame body (`CALL`/`CQ`/`BEACON` carry them) or from the control
port's `MON:` lines, which report the modem's cached callsign context.

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

Commands whose feature is compiled out or not yet active in this build
(crypto without `ENABLE_CRYPTO`, voice before Phase 5) answer
`ERROR:UNSUPPORTED` — distinct from `ERROR:UNKNOWN` so clients can tell a
gated feature from a typo.

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
| `PROBE [ON]` | probe-before-promote: always on (query answers `PROBE ON`; `PROBE OFF` is refused with `ERROR:STATE`) |
| `MASKING ON\|OFF` | carrier masking |
| `RESUME ON\|OFF` | session resume |
| `ULTRA ON\|OFF` | permit the opt-in 2.7 kHz ULTRA family (`ultra_enable`); `FAMILY ULTRA` is refused while this is OFF |

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
| `FREQINFO [hz]` | query, or report the rig's dial frequency; answers `CENTER=<hz> SHIFT=<hz> CFO=<hz>` either way |
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
([PROTOCOL.md](PROTOCOL.md)): the offset of the path this station
transmits on, clamped ±50 Hz and reset at every session end. Frames
decode anywhere within ±55 Hz regardless (since v1.2.0); a persistently
large value means the dial needs retuning before that margin runs out,
and an `EVT:CFO_WARN` fires past ±40 Hz.

`STATUS JSON` carries the same keys lower-cased in one JSON object — the
GUI and companion app consume this form.

### Notifications

```text
EVT:STATE <IDLE|LISTENING|CALLING|CONNECTING|CONNECTED|DISCONNECTING>
EVT:CONNECTED <remote_call> <family> <initial_mode>
EVT:DISCONNECTED <reason>   # see the reason table below
EVT:MODE <Mx> <net_bps>
EVT:SNR <local_db> <remote_db|->
EVT:BUSY <0|1>
EVT:TXCOMPLETE
EVT:STREAM_REPLAY <bytes>    # re-sent after a peer restart; see below
EVT:FILE_RX <name> <size> <crc32ok>
EVT:FILE_ERROR <name> <reason>
EVT:VOICE <state>
MON:<src>:<dst|*>:<frame_type>:<family>:<Mx>:<hex_payload>
```

`EVT:DISCONNECTED` reasons. A session that reached `CONNECTED` reports
`NORMAL` (either end asked) or `LINK` (anything else), unchanged. A
FAILED ATTEMPT — one that never reached `CONNECTED` — used to report
nothing at all, leaving an application to infer from `EVT:STATE` alone
that its call had failed and with no way to tell "nobody answered" from
"the peer refused". Those now report as well:

| Reason | Meaning |
|---|---|
| `NORMAL` | either end asked to disconnect |
| `LINK` | an established session dropped (timeout, retries, fault) |
| `NO_ANSWER` | called, the CALL retry budget ran out unanswered |
| `NO_START_ACK` | the peer answered the CALL but never acknowledged the START |
| `REFUSED` | the peer rejected the START (version, family, busy, policy) |
| `TIMEOUT` | answered a CALL, the peer's START never arrived |

`EVT:STREAM_REPLAY <bytes>` is emitted when the peer restarted and could
not honour a session-resume token. The sender re-sent that many bytes
from the last message boundary the dead peer had acknowledged, so
delivery for those bytes is AT-LEAST-ONCE: discard them from the front
of what follows if repeats matter to you, or ignore the event if your
consumer is idempotent. See PROTOCOL.md, "Reconnecting to a Peer That
Lost Its Session".

```text
EVT:BROADCAST <src> <hex_payload>
EVT:SNR <local_db> <remote_db|->
EVT:CFO_WARN <tx_align_hz>
EVT:TX_WATCHDOG <seconds_keyed>
```

`MON:` carries the **complete link frame** as hex (envelope + body + CRC),
so a client can verify it and reassemble multi-frame broadcasts; an
encrypted or undecodable frame reports an empty payload field and metadata
only. `frame_type` is the numeric type from
[PROTOCOL.md](PROTOCOL.md).

`EVT:BROADCAST` fires when the monitor has reassembled a complete
`BCAST_DATA` fragment chain — the payload an operator actually sent. It is
the control-port counterpart of the data port's binary records, and unlike
them it keeps flowing while a session is connected.

`EVT:SNR` is emitted whenever the measured link SNR moves by ≥ 1 dB while
connected (not on a timer), so an idle link costs nothing.

`EVT:TX_WATCHDOG` reports that the TX watchdog force-released PTT; it is
always a bug, never a normal operating event.

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
| `RTS` / `DTR` | assert/release the named serial control line (`PTTPORT` device, `PTTBAUD`); the port is opened raw with hardware flow control disabled, since `CRTSCTS` would fight the driver for RTS |
| `VOX` | rely on radio VOX — the burst's lead/tail silence padding is what keys the rig |
| `TCP` | send `KEY\n` / `UNKEY\n` to a companion-app PTT server (`PTTPORT host[:port]`, default port 4532) |

The key is asserted just before the first sample of a burst enters an empty
TX ring and released once the ring has drained **and** the audio driver has
had time to play what it already took: the release waits the stream's
output latency as PortAudio reports it (logged at start-up as `output
latency N ms`) plus 10 ms. `ptt_lead_ms` covers the rig's keying latency
and `ptt_tail_ms` adds trailing silence on top. (Before 2026-10 the key
dropped as soon as the ring was empty, cutting up to the driver latency
- 10-100+ ms on real interfaces - off the end of every burst; virtual
cables hid it.) A serial
port or PTT server that cannot be opened is reported and the modem runs
**without keying** rather than refusing to start — losing the radio is
better than losing the modem.

Sequence per burst: assert → wait `ptt_lead_ms` (default 50) → audio →
wait `ptt_tail_ms` (default 20) → wait the output latency + 10 ms →
release. A TX watchdog
([DSP.md](DSP.md)) force-releases PTT if any transmission exceeds
`tx_watchdog_s` (default 120 s), discards the staged audio, and emits
`EVT:TX_WATCHDOG`. It is a safety net against a wedged modem holding a
transmitter down, so it is deliberately far above any legitimate burst.

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
  informational — it keys channel-memory buckets into 1 kHz slots so the
  store distinguishes the same peer worked on different bands; `FREQINFO 0`
  clears it)
- consume `STATUS JSON` polling or `EVT:` streams for its UI

A reference companion (`tools/companion.py`, Python/Tk) implements the
split: chat over the DATA port, session control + status/event display
over the CONTROL port, and dial-frequency reporting. One instance per
modem: `tools/companion.py --data 9200 --ctrl 9201 --title W1AW`.

## Built-in GUI

SDL2 + Dear ImGui application in the same process (`--gui`), rendering from
the telemetry snapshot (never touching modem internals):

- waterfall (RX spectrum, family band overlay, mask overlay)
- constellation / despread-symbol view
- ladder panel: current mode, AMC inputs (SNR, retry, selectivity), floor
  markers
- session panel: state, peer, rates, ARQ window occupancy
- settings pages mirroring the INI schema ([CONFIGURATION.md](CONFIGURATION.md)),
  including a Security page on crypto builds (PSK entry is masked and the
  buffer is wiped once installed; a non-crypto build shows the modem's own
  `ERROR:UNSUPPORTED` answer rather than a dead control)

Everything the GUI can change routes through the same command dispatcher as
the control port — the GUI is a control-port client that happens to live
in-process.
