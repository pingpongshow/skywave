# Skywave Live Voice Design

## Overview

Live digital voice is a service layered on a connected session (or a
broadcast state), using the open-source **Codec2** vocoder. The local voice
port only ever carries PCM; the modem performs all codec work internally.

- voice port media: `8 kHz` mono `PCM16LE`, framed records
  ([INTERFACES.md](INTERFACES.md))
- on-air media: Codec2 frames inside `VOICE_DATA` link frames
- voice requires an unspread OFDM mode — there is no robust-tier voice

## Talkspurt Streaming and Real Time

The original design streamed a talkspurt as one continuous transmission
that paid the preamble + PHY header once. That was never built: every
`VOICE_DATA` unit is its own PHY frame with its own chirp preamble, ZC
sync and header. On HF that fixed cost is 0.6-1.3 s per frame, so a codec
that merely fits a mode's NET rate can still fall behind real time - WIDE
`M10` at `2400` aired 160% of the speech it carried, and a long spurt
lagged further every second (found 2026-10-18; ROADMAP).

Voice therefore plans the **codec and the unit size together**
(`planVoiceStream`): the highest codec, and the smallest unit of 1-4 LDPC
codewords holding at most 3 s of speech, whose frame airtime is at most
85% of the speech it carries. AUTO uses only such real-time plans. A
manual `VOICECODEC` that no plan can stream is still honoured (4-codeword
units) and `VOICESTATUS` reports `RT=NO`.

Latency follows from the unit: first audio arrives one unit of speech
plus its airtime after PTT - about 4-6 s on HF (2-3 s units), 1-2 s on
VHF-FM (0.4-1 s units). A fade loses a whole unit of speech, never the
session.

## Codec Set and Mode Mapping

Codec2 modes used: `700C` (700 bps), `1300`, `2400`, `3200`.

`VOICECODEC AUTO` (MEDIUM CP; unit = speech per frame):

| Family | Mode | Codec | Unit | Airtime / speech |
|---|---|---|---|---|
| `WIDE` | `M8` | `700C` | 2.5 s | 74% |
| `WIDE` | `M9` | `1300` | 3.0 s | 81% |
| `WIDE` | `M10` | `1300` | 2.1 s | 76% |
| `WIDE` | `M11` | `2400` | 2.5 s | 76% |
| `WIDE` | `M12` | `2400` | 1.6 s | 82% |
| `WIDE` | `M13` | `3200` | 2.0 s | 75% |
| `ULTRA` | `M8`-`M13` | `700C`/`1300`/`1300`/`2400`/`2400`/`3200` | 1.5-2.5 s | 68-81% |
| `MID` | `M12` | `700C` | 3.0 s | 71% |
| `MID` | `M13` | `700C` | 2.1 s | 81% |
| `NARROW` | - | none (no plan keeps up; a manual codec is allowed at `M11+`, `RT=NO`) | | |
| `VHF` | `M8`-`M14` | `3200` | 0.4-0.9 s | 55-82% |

Voice does not open where AUTO has no plan. Manual codec selection is
clamped to what the mode's NET rate can carry.

If AMC moves the session mid-talkspurt, the plan is redone at the new
mode: the codec steps at the next frame boundary (`EVT:VOICE CODEC <name>`); if
no plan exists any more, voice closes cleanly.

## Framing

- Codec2 frames are packed into `VOICE_DATA` units sized by the voice
  plan above (1-4 LDPC codewords, at most 3 s of speech; ~0.4-3 s
  depending on codec and mode)
- `VOICE_DATA` body: codec id (u8), frame count (u8), timestamp (u32,
  8 kHz sample units), packed codec frames
- voice frames are **not ARQ-tracked** (no retransmission — late voice is
  worthless) and live in their **own sequence space**: the envelope
  sequence field of `VOICE_DATA` carries an independent per-session voice
  counter, invisible to ARQ windows and gap bitmaps
  ([PROTOCOL.md](PROTOCOL.md)); gaps in the voice counter trigger
  concealment, never NAKs
- packet-loss concealment: Codec2 frame repeat with 3 dB attenuation, up to
  2 repeats, then silence

## Session Control

### Connected Voice

Half-duplex talkspurt discipline over the existing session:

```text
A: VOICE_OPEN(codec)        -> B: VOICE_OPEN_ACK(codec)
A: VOICE_PTT_REQ            -> B: VOICE_PTT_GRANT
A: VOICE_DATA ...           (B's data TX defers; ACKs still flow)
A: VOICE_PTT_END
B may now VOICE_PTT_REQ ...
A or B: VOICE_CLOSE
```

- `VOICE_PTT_GRANT` arbitration prevents both stations talking past each
  other on a half-duplex channel
- data transfer continues between talkspurts; voice preempts data in the TX
  queue but never preempts urgent control
- jitter buffer: 240 ms target depth downlink, adaptive ±120 ms

### Broadcast Voice

One-way talkspurts without a session (`VOICE BROADCAST`, `VOICESTART`,
`VOICEPTT ON|OFF`). Units go out as `BCAST_VOICE` link frames - a
`VOICE_DATA` body whose envelope `session_id` carries the sender's 16-bit
callsign tag (as `BCAST_DATA`) and whose `sequence` is the sender's own
voice counter - at the fixed mode `voice.broadcast_mode` (default `m9`;
`auto` = the most robust mode with a real-time plan, WIDE `M8`), never
encrypted.
There is no grant: the operator keys it like a voice transceiver, so
listen before talking.

Any **listening** Skywave that is not in a session plays a broadcast out
on its voice port. It locks onto one sender (`EVT:VOICE BCAST <tag>`),
ignores others keying over it, conceals gaps in the sender's counter,
and releases after 3 s of silence (`EVT:VOICE BCAST END`). `VOICESTATUS`
shows `BCASTRX=ON` meanwhile. Implemented 2026-10-18: before that,
`VOICE BROADCAST` only set a flag.

### Capability

Voice is negotiated: each modem built with voice advertises `CAP_VOICE`,
and `VOICESTART` (and an incoming `VOICE_OPEN`) needs it on both ends.
Before 2026-10-18 the bit was sent by nobody and read by nothing.

## Control Surface

| Command | Meaning |
|---|---|
| `VOICE [OFF\|CONNECTED\|BROADCAST]` | select voice mode |
| `VOICECODEC [AUTO\|700C\|1300\|2400\|3200]` | codec selector |
| `VOICESTART` / `VOICESTOP` | open/close voice for the selected mode |
| `VOICEPTT ON\|OFF` | talkspurt request/end |
| `VOICESTATUS` | one-line snapshot |

```text
VOICESTATUS MODE=CONNECTED CODEC=AUTO(1300) STATE=OPEN PTT=OFF GRANTED=OFF
TXQ=0 RXQ=960 TRANSPORT=M9 UNIT=3.0s RT=YES
```

`UNIT`/`RT` appear while voice is open (the plan's unit length and
whether it keeps up with real time); `BCASTRX=ON` while a broadcast is
playing out. INI: `voice.codec` (`auto`/`700c`/`1300`/`2400`/`3200`,
the startup `VOICECODEC`; a runtime change is saved on exit) and
`voice.broadcast_mode` (`auto` or `m8`...).

With the placeholder callsign (`N0CALL`/empty) `VOICESTART` and
`VOICEPTT ON` answer `ERROR:STATE`: voice transmits like any other
traffic. A malformed record on the voice port is discarded and logged
(`WARN voice port: N malformed record(s) discarded`).

## Testing

`test_voice` (codec, framing, FSM, jitter buffer, real-time plans,
capability, broadcast between engines); `voice_st2` (two full modems:
CCIR Good and Moderate, both directions, encrypted, VHF-FM through the FM
radio with flutter and at 700C, broadcast on static and fading channels);
`st3_hardware` (live voice A->B through both TCP voice ports over real
audio devices). Not yet tested: on air, and by a human listener - the
test signal is a synthetic voiced buzz, judged by correlation against a
local Codec2 round trip.

## Build Integration

- Codec2 linked as an optional dependency (`ENABLE_VOICE`, default `ON`;
  library via system package or vendored subtree)
- voice compiles out cleanly when disabled: no port 9102, no voice commands,
  no voice frame handling (peer voice frames are ignored with a log line)
- encryption applies to `VOICE_DATA` like any payload frame when active
