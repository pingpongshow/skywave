# Skywave Protocol Specification

## Scope

The on-air link protocol: session model, frame formats, handshake, ARQ/HARQ,
keepalive, resume, and broadcast traffic. Waveform-level formats live in
[WAVEFORM.md](WAVEFORM.md); adaptation policy lives in
[LINK_ADAPTATION.md](LINK_ADAPTATION.md).

## Session Model

Skywave is a reliable, half-duplex, connection-oriented HF link with:

- three-way capability-negotiated connection setup (`CALL` → `CALL_ACK` →
  `START` → `START_ACK`)
- runtime adaptive mode control over the `M4–M13` ladder
- selective-repeat ARQ with HARQ combining
- optional zstd compression, optional encryption, optional resume
- exactly **one** mode surface: ladder positions `M0–M13`

## Identity

- callsigns: 3–12 characters `[A-Z0-9/]`, stored uppercase
- wire representation: full callsign in handshake frames; 24-bit FNV-1a hash
  elsewhere (collision handled by full-call check at connect)
- placeholder `N0CALL` and empty callsigns block all TX paths

## Link Frame Format

Every frame shares one link-layer envelope inside the PHY payload:

```text
byte 0      frame_type (matches PHY header frame_type)
byte 1      flags: [0] compressed  [1] encrypted  [2] more-fragments
                   [3] resume-context  [4..7] reserved
bytes 2-3   session_id (0 for pre-session frames)
bytes 4-5   sequence (per-direction, wraps mod 65536)
bytes 6-7   ack_base (cumulative receive floor, piggybacked)
bytes 8..   type-specific body
last 4      CRC-32 over envelope+body (before crypto framing)
```

Fixed overhead: 12 bytes. When encryption is active the AEAD tag adds 16
bytes ([SECURITY.md](SECURITY.md)).

Sequence-space rule: the envelope `sequence` field of `VOICE_DATA` frames
carries an **independent voice-stream counter**, not an ARQ sequence number.
ARQ sequence numbers, `ack_base`, and gap bitmaps cover non-voice frames
only. This is load-bearing: a lost frame has no readable type, so if voice
consumed ARQ sequence numbers, receivers could not distinguish a lost voice
frame (never retransmitted) from a lost data frame and would NAK it forever.
Voice loss detection uses the voice counter alone ([VOICE.md](VOICE.md));
crypto keeps the spaces apart via a stream id in the nonce
([SECURITY.md](SECURITY.md)).

## Frame Types

| Type | Purpose |
|---|---|
| `CALL` | connection request (full callsigns, quality probe) |
| `CALL_ACK` | connection response with measured quality |
| `START` | capability commit and initial-mode proposal |
| `START_ACK` / `START_NAK` | accept with final initial mode / reject with reason |
| `DATA` | user payload segment (ARQ-tracked) |
| `ACK` / `NAK` | compact ARQ feedback |
| `MODE_CHANGE` / `MODE_ACK` | in-band mode/CP/family-parameter coordination |
| `PROBE` / `PROBE_ACK` | promotion probe exchange |
| `MASK_UPDATE` / `MASK_ACK` | carrier-mask exchange |
| `KEEPALIVE` | idle liveness + SNR refresh |
| `RESUME_REQ` / `RESUME_ACK` | session resume across reconnect |
| `DISC` / `DISC_ACK` | orderly teardown |
| `BEACON` | broadcast telemetry announcement |
| `CQ` | general call |
| `BCAST_DATA` | connectionless broadcast payload |
| `VOICE_OPEN` / `VOICE_OPEN_ACK` / `VOICE_CLOSE` | voice session control |
| `VOICE_PTT_REQ` / `VOICE_PTT_GRANT` / `VOICE_PTT_END` | talkspurt control |
| `VOICE_DATA` | Codec2 voice payload |
| `KEY_UPDATE` | in-session crypto epoch rotation ([SECURITY.md](SECURITY.md)) |

## Handshake

### Transport

All four handshake frames use the calling chirp signature + **robust header**
+ `M4` payload transport for the selected family. Handshake therefore works
at the family's data floor.

### Sequence

```text
Initiator                              Responder
---------                              ---------
CALL  ------------------------------>  measures I->R quality
        <------------------- CALL_ACK  carries R's measurement of CALL
START ------------------------------>  commits caps + proposed initial mode
        <------------- START_ACK/NAK   final initial mode (responder decides)
CONNECTED                              CONNECTED
```

### `CALL` body

- full source callsign, full destination callsign (length-prefixed)
- protocol version, family, capability bitmap (see below)

### `CALL_ACK` body

- measured `CALL` quality: SNR (dB, s8 fixed-point ×2), frequency-selectivity
  index (u8), CFO estimate (s8, 0.5 Hz units)
- responder capability bitmap

### `START` body

- committed capability intersection
- proposed initial mode (from both quality reports, clamped by config caps)
- initial CP mode, max-mode cap, MAC profile
- optional: resume token, crypto KEX public key ([SECURITY.md](SECURITY.md))

### `START_ACK` body

- final initial mode (responder may only lower the proposal)
- optional: crypto KEX response

`START_NAK` reasons: family mismatch, version mismatch, capability conflict,
busy, policy refusal.

**ULTRA fallback**: `ULTRA` is opt-in on both ends and uses its own chirp
span, so a non-ULTRA station will not even detect an ULTRA call. When
`family_fallback` is enabled (default on), an initiator whose ULTRA `CALL`
attempts go unanswered automatically retries the remaining attempts at
`WIDE` — one config knob, no protocol change.

### Capability bitmap (u16)

| Bit | Capability |
|---|---|
| 0 | zstd compression |
| 1 | encryption |
| 2 | session resume |
| 3 | HARQ-IR |
| 4 | carrier masking |
| 5 | burst MAC aggregation |
| 6 | voice |
| 7 | `CAP_CSS_TIER` (reserved deep tier) |
| 8–15 | reserved, must be 0 |

### Timers and Retries

Handshake frames ride the robust tier, where a single frame can take
3.5–7 s of air per family — **fixed timeouts shorter than the exchange
airtime would retry into the peer's reply**. All response timeouts are
therefore computed from airtime, which the PHY reports per frame:

| Timer | Value |
|---|---|
| `CALL` response timeout | `own_airtime + expected_response_airtime + 6 s`, clamped 12–45 s; 4 attempts |
| `START` response timeout | same formula; 4 attempts |
| ACK timeout | adaptive: `burst_airtime + expected_ack_airtime + 3 s`, clamped 4–45 s |
| idle disconnect | 120 s without any valid frame |
| keepalive interval | 20 s idle |
| `DISC` retry | `2 × frame_airtime + 3 s` cadence, 3 attempts, then unilateral teardown |

(`NARROW` robust-tier exchanges sit near the upper clamps by design; the
clamp ceilings exist to bound pathological configurations, not to race the
slow families.)

## Connected Transport

Connected control and data ride the current committed session mode. There are
no per-frame-type transport exceptions except:

- `ACK`/`NAK` for a session in `M4–M6` may be sent at `M4` regardless of the
  session mode (feedback must be at least as robust as forward traffic)
- `MODE_CHANGE` executing a demotion is sent at the **target** (lower) mode

### ARQ

Selective-repeat with cumulative base + gap bitmap:

- per-direction 16-bit sequence space, window size from the MAC profile
- `ACK` body (7 bytes): SNR report (s8 ×2), rx-quality flags (u8),
  `ack_base` (u16), `gap_bitmap` (u16, relative to base+1), CFO report
  (s8, 0.5 Hz units — drives TX frequency alignment, below)
- `NAK` is an `ACK` with at least one gap bit set — one format, one parser
- every data-bearing frame also piggybacks `ack_base` in the envelope
- retransmissions carry the original sequence number and an incremented
  HARQ `rv` in the `DATA` body header

### HARQ

| Mode | Behavior |
|---|---|
| `CHASE` | retransmit identical codewords; receiver soft-combines LLRs |
| `IR` | rate-stepdown combining (below) |
| `AUTO` | IR when both peers advertise it, else chase (default) |

**IR definition.** The 802.11n LDPC rates are independent matrices, not
puncture-compatible — cross-rate incremental parity is impossible with this
code family. Skywave IR is therefore **rate-stepdown with LLR priors**: the
first transmission uses the mode's native code rate; an IR retransmission
re-encodes the same info block with the `r1/2` matrix, and the receiver
seeds the systematic-bit LLRs of the `r1/2` decode with the accumulated
posteriors from earlier failed attempts. For modes already at `r1/2`
(`M4–M8`, `M10`), IR degenerates to chase — which is correct behavior, not
an error.

The receiver keeps soft buffers for up to `window` outstanding codewords in
robust modes and 2× that in `M8+`.

### MAC Burst Profiles

Negotiated at `START`; the AMC may clamp downward at runtime:

| Session mode | ARQ window | Frames per TX burst |
|---|---:|---:|
| `M4–M6` | 2 | 1 |
| `M7–M9` | 4 | 2 |
| `M10–M13` | 8 | 4 |

Multi-frame bursts use the PHY **burst container**
([WAVEFORM.md](WAVEFORM.md)) — one preamble/sync/header for the whole
burst, link frames packed back-to-back inside.

### TX Frequency Alignment

RX-side CFO correction cannot fix the physical problem that a mistuned
signal sits shifted inside the peer's SSB filter (a 50 Hz offset pushes the
`WIDE` band edge from 2662 Hz to ~2712 Hz — into the filter skirt). Each
station therefore steers its **transmit** signal onto the peer's frequency
grid:

- `CALL_ACK` and every `ACK` carry the receiver's measured CFO
- **split correction**: each transmitter shifts by −(reported CFO)/2,
  slewed at ≤ 2 Hz per adjustment, clamped to ±30 Hz per side. The full
  correction has to move the audio spectrum relative to *someone's* filter
  — a one-sided shift of the whole offset would push one station's edge
  carriers into its own TX filter skirt. Splitting the offset caps each
  station's audio excursion at half the mistuning, which fits inside every
  family's edge guards (including `ULTRA`'s thin ones)
- both sides converge within a few exchanges; the residual is tracked CFO
  ≈ 0 and both signals sit centered in both filters
- the current correction is visible in `STATUS` (`CFO=` field) and
  telemetry; mistuning beyond what split correction absorbs is an operator
  problem, surfaced as an `EVT:CFO_WARN` past ±40 Hz total measured offset

### Keepalive

- initiator-only, `M8+` sessions only (robust modes must not burn airtime)
- 20 s idle interval; body: 1-byte remote-SNR report
- sessions in `M4–M7` rely on the idle-disconnect timer alone

## Payload Pipeline (`proto` layer)

TX order, applied above the link layer:

```text
app bytes -> coalesce window (250 ms or window-full)
          -> zstd compress (if negotiated AND saves ≥ 8 bytes; marker byte)
          -> encrypt (if active; SECURITY.md)
          -> segment to DATA bodies sized to the current mode's codeword budget
```

- compression marker: `0x00` stored, `0x5A` zstd; decompress cap 8 MiB
- segment size targets exactly filling an integer number of LDPC codewords
  for the current mode (the link queries the PHY for the per-frame byte
  budget; no fixed segment constant)
- fragmentation for messages larger than one frame uses the `more-fragments`
  flag; reassembly is sequence-ordered and survives retransmission reordering

### File Transfer

- `DATA` payloads carry an inner service tag: `0x01` stream, `0x02` file
- file metadata record: name (UTF-8, ≤ 255 B), size (u64), mtime (u64),
  CRC-32, chunk size
- checkpoint every 32 KiB acknowledged: receiver persists (file_id, offset)
  so resume ([below](#session-resume)) can continue a transfer across
  reconnects
- completion notification surfaces on the control port
  ([INTERFACES.md](INTERFACES.md))

## Session Resume

- negotiated capability; token minted at `START_ACK` (8-byte random ID bound
  to family + callsign pair)
- on reconnect within the resume window (10 min), initiator sends
  `RESUME_REQ(token, last_rx_seq)` after `CALL_ACK`; responder validates and
  answers `RESUME_ACK(last_rx_seq)` — sequence spaces and any file-transfer
  checkpoint continue
- any mismatch falls back to a clean `START` (never a protocol error)
- resume state is invalidated by family change, crypto state change, or
  token expiry

## Broadcast Traffic

Connectionless frames use the calling chirp signature + robust header + `M4`:

- `CQ`: full callsign, capability bitmap — invites any responder
- `BEACON` body: full callsign, local SNR floor estimate (u8, `dB + 20`),
  current family, software version
- `BCAST_DATA`: inner format magic `0x5B 0x01`, u16 length, payload
  (≤ 256 B per frame; larger payloads fragment across frames with a 4-bit
  fragment index/count)

CW ID (optional, config-gated): on-air Morse identification rendered after
disconnect and every 10 min during long connected sessions where required by
regulation. Pure audio-layer feature; no protocol interaction.

## Monitoring (Passive RX)

Because every frame has a self-describing PHY header, a listening Skywave
decodes any frame it can hear. Monitor mode surfaces decoded frames on the
control port (`MON:` lines) and data port (tagged binary records) — formats
in [INTERFACES.md](INTERFACES.md). Encrypted payload bodies surface as
metadata only.

## Version and Compatibility Rules

- `version` (PHY header) gates parsing; mismatched majors `START_NAK`
- unknown frame types are ignored (logged), never fatal
- unknown capability bits must be advertised as 0 and ignored on receive
- reserved `M0–M3` mode indices in any control field are treated as invalid
  in v1 and clamp to `M4`
