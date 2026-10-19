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

The ports bind `127.0.0.1` only (there is no bind-address setting). One
client per port.

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

Commands whose feature is compiled out of this build (crypto without
`ENABLE_CRYPTO`, voice without Codec2) answer `ERROR:UNSUPPORTED` —
distinct from `ERROR:UNKNOWN` so clients can tell a gated feature from a
typo.

With the placeholder callsign (`N0CALL` or empty — see
[CONFIGURATION.md](CONFIGURATION.md)) every command that would transmit
— `CONNECT`, `CQ`, `BEACON`, `BCAST`, `TXTEST <s>`, `VOICESTART`,
`VOICEPTT ON` — answers `ERROR:STATE` and nothing is keyed. Set `MYCALL`
first.

A setting command without its argument is a query and answers in the
same form (`LISTEN` → `LISTEN ON`).

### Command Index

#### Identity and Session

| Command | Meaning |
|---|---|
| `MYCALL [call]` | set/query callsign |
| `CONNECT <call>` | initiate connection |
| `DISCONNECT` | orderly teardown |
| `ABORT` | immediate teardown, no `DISC` exchange |
| `LISTEN [ON\|OFF]` | accept incoming calls (saved on exit) |
| `CQ` | transmit CQ and turn listening on — the same saved switch as `LISTEN ON` |
| `BEACON` | transmit a beacon (callsign and family; its SNR-floor byte is sent as `0xFF`, unknown: the modem has no calibrated SNR floor to report) |
| `BCAST <hex>` | transmit broadcast data frame |
| `CWID [ON\|OFF]` | post-disconnect CW ID |

#### Mode and Link

| Command | Meaning |
|---|---|
| `FAMILY [WIDE\|MID\|NARROW\|ULTRA]` | set/query bandwidth family (IDLE/LISTENING only; `ULTRA` requires `ultra_enable` in config) |
| `MODE [M4-M13\|AUTO]` | pin or free the runtime mode. The range is the band's OFDM modes: `M4`–`M13` on HF, `M4`–`M14` on VHF-FM; anything else is `ERROR:BADARG` |
| `MAXMODE [M4-M13]` | cap AMC promotion; the same range as `MODE` (`M14` on VHF-FM) |
| `CPMODE [AUTO\|SHORT\|MEDIUM\|LONG]` | cyclic prefix policy |
| `HARQ [CHASE\|IR]` | HARQ offered to the next session: `IR` (default; incremental redundancy, chase combining with a peer that lacks it) or `CHASE` (IDLE only) |
| `CHANMEM ON\|OFF` | channel memory |
| `PROBE [ON]` | probe-before-promote: always on (query answers `PROBE ON`; `PROBE OFF` is refused with `ERROR:STATE`) |
| `MASKING ON\|OFF` | carrier masking |
| `RESUME ON\|OFF` | session resume |
| `ULTRA ON\|OFF` | permit the opt-in 2.7 kHz ULTRA family (`ultra_enable`); `FAMILY ULTRA` is refused while this is OFF, and `ULTRA OFF` while ULTRA is the family |

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
| `MONITOR [ON\|OFF]` | passive frame monitoring |
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
| `ENCRYPTION [ON\|OFF]` | request encryption for future connects; without an argument it answers the `SECURITY` line |
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
| `VOICESTATUS` | voice state snapshot; while voice is open it adds `UNIT=<s>` and `RT=YES\|NO` (the voice plan's unit length and whether it keeps up with real time), and `BCASTRX=ON` while a broadcast plays out ([VOICE.md](VOICE.md)) |

### `STATUS` Fields

```text
STATUS STATE=<fsm> CALL=<my> REMOTE=<peer|-> FAMILY=<f> MODE=<Mx|AUTO(Mx)>
NET=<bps> SNR=<local> RSNR=<remote|-> CFO=<tx_align_hz> RETRY=<ratio>
BUSY=<0|1> LISTEN=<0|1> COMPRESS=<0|1> CRYPTO=<0|1> EPOCH=<n> RESUME=<0|1>
MASK=<count> VOICE=<off|open|talking|bcastrx> TXBUF=<bytes> FOK=<n> FFAIL=<n>
TURN=<ISS|IRS|->
```

`TURN` is this station's role in the scheduled turn-taking MAC: `ISS`
(sending), `IRS` (receiving), `-` when no turn schedule is active.

`MODE` is `Mx` when the mode is pinned (`MODE Mx`), including while idle
(the pin applies from the next session), and `AUTO(Mx)` when the AMC
chooses, with the mode in use.

`CFO` is the current TX frequency-alignment correction
([PROTOCOL.md](PROTOCOL.md)): the offset of the path this station
transmits on, clamped ±50 Hz and reset at every session end. Frames
decode anywhere within ±55 Hz regardless (since v1.2.0); a persistently
large value means the dial needs retuning before that margin runs out,
and an `EVT:CFO_WARN` fires past ±40 Hz.

`STATUS JSON` carries the same keys lower-cased in one JSON object, for
clients that prefer it (the built-in GUI and Skywave Link parse the plain
line).

### Notifications

```text
EVT:STATE <IDLE|LISTENING|CALLING|CONNECTING|CONNECTED|DISCONNECTING>
EVT:CONNECTED <remote_call> <family> <initial_mode>
EVT:DISCONNECTED <reason>   # see the reason table below
EVT:MODE <Mx> <net_bps>
EVT:SNR <local_db> <remote_db|->
EVT:BUSY <0|1>
EVT:STREAM_REPLAY <bytes>    # re-sent after a peer restart; see below
EVT:VOICE <state>             # OPEN, TALKING, STOPPED, CLOSED
EVT:VOICE CODEC <700C|1300|2400|3200>  # the voice codec stepped mid-talkspurt
EVT:VOICE BCAST <tag>        # a broadcast started playing out (sender tag, hex)
EVT:VOICE BCAST END          # ...and went quiet for 3 s
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
| `LINK` | an established session dropped: ARQ retries exhausted, idle timeout, the peer disconnected for a fault, or the peer called afresh (it lost the session) |
| `NO_ANSWER` | called, the CALL retry budget ran out unanswered |
| `NO_START_ACK` | the peer answered the CALL but never acknowledged the START |
| `REFUSED` | the peer rejected the START (version, family, capability conflict, failed key exchange) |
| `TIMEOUT` | answered a CALL, the peer's START never arrived |

`EVT:STREAM_REPLAY <bytes>` is emitted when the peer restarted and could
not honour a session-resume token. The sender re-sent that many bytes
from the last message boundary the dead peer had acknowledged. That is an
UPPER bound on the repeat: at most that many of the following bytes
duplicate what the application already received (an unconfirmed message
may or may not have reached the dead peer). An application that cannot
tolerate a repeat de-duplicates within that span by content or its own
sequence numbers - blindly discarding all of it can drop bytes the dead
peer never delivered; an idempotent consumer ignores the event. See PROTOCOL.md, "Reconnecting to a Peer That
Lost Its Session".

```text
EVT:BROADCAST <src> <hex_payload>
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
2. `AUDIOLEVEL` and the GUI meters show output level; the GUI's Audio
   settings page has the test-burst buttons and states the target
   (adjust the rig so ALC just stays quiet)
3. `TXTEST` is refused unless `IDLE` (and with the placeholder callsign),
   is hard-capped at 30 s, and honors the TX watchdog

The peer-side check: `RSNR` in `STATUS` reading far below what `SNR`
symmetry would suggest while local SNR looks fine is the classic
signature of TX overdrive.

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

SDL2 + Dear ImGui window in the same process (`--gui`). It reads only
what the worker publishes (status strings, the telemetry snapshot) and a
copy of the input audio, so it never waits on the modem:

- top bar: state (with `TX` / `BUSY`), a `SET CALLSIGN` button while the
  callsign is the placeholder, `ENCRYPTED` during an encrypted session,
  the last action's outcome, and callsign, remote, family, mode, net rate
  and SNR
- session card: remote callsign, Connect / Disconnect, CQ / Beacon / Abort
- link card: mode and cyclic-prefix selectors (the mode list is the
  band's, as `MODE` accepts), local and remote SNR, CFO alignment, retry
  ratio, and a mode history plot
- audio card: RX, TX and noise-floor level bars (dBFS) and an RX level
  history with the noise floor drawn over it
- waterfall: input spectrum from 0 Hz to twice the band centre, with the
  active family's band edges and its masked carriers marked
- constellation: equalized data symbols of the last decoded frame
- activity log: every command the window sent and the modem's answer
  (the PSK argument is masked)
- Settings window: Station (callsign, family, highest mode, listen,
  monitor, CW ID, allow ULTRA, the TCP ports), Audio (devices by name,
  test burst, levels), PTT (method, port, lead/tail) and Security (the
  `SECURITY` line, masked PSK entry wiped once installed, request
  encryption, rotate keys; a non-crypto build shows the modem's own
  `ERROR:UNSUPPORTED` answer rather than a dead control)

Everything the GUI can change routes through the same command dispatcher as
the control port — the GUI is a control-port client that happens to live
in-process.
