# Skywave Live Voice Design

## Overview

Live digital voice is a service layered on a connected session (or a
broadcast state), using the open-source **Codec2** vocoder. The local voice
port only ever carries PCM; the modem performs all codec work internally.

- voice port media: `8 kHz` mono `PCM16LE`, framed records
  ([INTERFACES.md](INTERFACES.md))
- on-air media: Codec2 frames inside `VOICE_DATA` link frames
- voice requires an unspread OFDM mode — there is no robust-tier voice

## Talkspurt Streaming (Key Design Constraint)

A per-packet preamble + PHY header costs ~0.4–3 s of airtime — more than the
speech it would carry. Voice therefore **streams**: a granted talkspurt is
one continuous transmission that pays the preamble + header cost once, then
sends `VOICE_DATA` codewords back-to-back until PTT end. The receiver's
tracking loop and scattered pilots hold sync across the whole spurt.
Streaming overhead (envelope + body headers amortized over a spurt) is ~5%
of the codec rate.

## Codec Set and Mode Mapping

Codec2 modes used: `700C` (700 bps), `1300`, `2400`, `3200`.

`VOICECODEC AUTO` resolves from family + current session mode, choosing the
highest codec whose streamed rate (codec + ~5% overhead) fits the mode's NET
rate with margin:

| Family | Session mode | Effective codec |
|---|---|---|
| `ULTRA` | `M8` | `1300` |
| `ULTRA` | `M9` | `2400` |
| `ULTRA` | `M10+` | `3200` |
| `WIDE` | `M8` | `1300` |
| `WIDE` | `M9` | `2400` |
| `WIDE` | `M10+` | `3200` |
| `MID` | `M8`–`M9` | `700C` |
| `MID` | `M10`–`M11` | `1300` |
| `MID` | `M12+` | `2400` |
| `NARROW` | `M11+` | `700C` |

Voice does not auto-open below the first row of its family. Manual codec
selection is clamped to what the current mode can carry.

If AMC demotes below the active codec's requirement mid-talkspurt, the codec
steps down at the next frame boundary (with an `EVT:VOICE codec` event); if
the session drops below the family's voice floor, voice closes cleanly.

## Framing

- within a talkspurt stream, Codec2 frames are packed into `VOICE_DATA`
  units sized to one LDPC codeword each (typically 160–480 ms of speech
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

One-way announcement without a session: the `VOICE BROADCAST` state transmits
`VOICE_DATA` frames after a beacon-style header at a fixed manual mode
(config choice among the family's voice-capable modes). Any monitoring
Skywave can play it out.

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
TXQ=0 RXQ=960 TRANSPORT=M9
```

## Build Integration

- Codec2 linked as an optional dependency (`ENABLE_VOICE`, default `ON`;
  library via system package or vendored subtree)
- voice compiles out cleanly when disabled: no port 9102, no voice commands,
  no voice frame handling (peer voice frames are ignored with a log line)
- encryption applies to `VOICE_DATA` like any payload frame when active
