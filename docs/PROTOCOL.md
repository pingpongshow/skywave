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
bytes 2-3   session_id (0 for pre-session frames; chosen non-zero by the
            initiator and carried in its START envelope, adopted by the
            responder, echoed in START_ACK/RESUME_ACK, kept by a resumed
            session)
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
| `BCAST_DATA` | connectionless broadcast payload (envelope `session_id` carries the **sender's** 16-bit callsign-hash tag, so a monitor can demultiplex overlapping broadcasts) |
| `VOICE_OPEN` / `VOICE_OPEN_ACK` / `VOICE_CLOSE` | voice session control |
| `VOICE_PTT_REQ` / `VOICE_PTT_GRANT` / `VOICE_PTT_END` | talkspurt control |
| `VOICE_DATA` | Codec2 voice payload |
| `KEY_UPDATE` | in-session crypto epoch rotation ([SECURITY.md](SECURITY.md)) |
| `TURN_GRANT` | turn-taking: hand the data-sender (ISS) role to the peer (body: u8 grant epoch) |

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
busy, policy refusal, **resume-invalid**. The responder sends the reason it
actually computed - it used to send `busy` for every refusal regardless,
which made a refused connect undiagnosable from the calling end. Only
`resume-invalid` is non-fatal: the caller falls back to a clean `START` on
the same call (see "Reconnecting to a Peer That Lost Its Session").

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
| 7 | `CAP_CSS_TIER` (deep tier: M0–M3 modes + handshake-on-CSS) |
| 8 | `CAP_TURN_SCHED` (scheduled ISS/IRS turn-taking, below) |
| 9–15 | reserved, must be 0 |

### Timers and Retries

Handshake frames ride the robust tier, where a single frame can take
3.5–7 s of air per family — **fixed timeouts shorter than the exchange
airtime would retry into the peer's reply**. All response timeouts are
therefore computed from airtime, which the PHY reports per frame:

| Timer | Value |
|---|---|
| `CALL` response timeout | `own_airtime + expected_response_airtime + 6 s`, clamped 12–600 s; 4 attempts (8 with the CSS tier enabled) |
| `START` response timeout | same formula; 4 attempts (8 with CSS) |
| ACK timeout | adaptive: `burst_airtime + expected_ack_airtime + 3 s` |
| idle disconnect | 120 s without any valid frame; sessions COMMITTED to a CSS mode scale to `max(120 s, 10 × CALL airtime)` — deep links have routine multi-minute decode droughts, OFDM sessions keep tight hang detection |
| keepalive interval | 20 s idle |
| `DISC` retry | `2 × frame_airtime + 3 s` cadence, 3 attempts, then unilateral teardown |

(`NARROW` robust-tier exchanges sit near the upper clamps by design; the
clamp ceiling exists to bound pathological configurations, not to race the
slow families. With `CAP_CSS_TIER` the timing formulas take the SLOWER of
the two CALL transports — a deep-tier control frame runs 20–50 s of air.)

Deep-tier handshake behaviors (6.2, all measured live at −13 dB CCIR
Moderate on MID):

- **CALL transport ladder**: attempt 0 rides OFDM calling; with the tier
  enabled, two of every three subsequent attempts ride CSS `M0`. The
  responder answers on whichever transport the CALL arrived on, and the
  whole handshake stays there.
- **Responder hold**: after CALL_ACK, the responder holds the answered
  state through the initiator's ENTIRE START retry ladder (each leg runs
  ~50% one-shot FER at the floor; a one-window hold expires exactly when
  the first START retry lands).
- **Duplicate START re-ack**: a connected responder receiving a duplicate
  START repeats its stored START_ACK verbatim — never re-runs session
  setup or the KEX.
- **Call-collision tie-break**: two stations calling each other
  simultaneously resolve deterministically — the lexically greater
  callsign yields and answers as responder.
- **Deep ARQ**: sessions committed below `M4` raise the retry budget to
  ≥ 20 (retry exhaustion at ~50% FER is normal operation, not a dead
  peer) and CSS payload codewords chase-combine like OFDM ones.

## Connected Transport

Connected control and data ride the current committed session mode. There are
no per-frame-type transport exceptions except:

- `ACK`/`NAK` for a session in `M4–M6` may be sent at `M4` regardless of the
  session mode (feedback must be at least as robust as forward traffic)
- `MODE_CHANGE` / `MODE_ACK` executing a **demotion** ride the robust
  tier (`M4`; a CSS session's own CSS mode): the channel just failed the
  mode being left, and an exchange that cannot cross would strand the
  session above its channel
- `MODE_CHANGE` / `MODE_ACK` executing a **promotion** (OFDM to OFDM) ride
  the **target** mode: the AMC has already judged it viable with margin,
  and a header-only frame that cannot cross there says the data could
  not either, so the missing `MODE_ACK` holds the promotion. The one
  re-announcement rides the mode being left. Before 2026-10-15 promotions
  rode the current mode, so a NARROW `M7 -> M8` step was two robust
  20.7 s frames (the robust header alone is ~20 s there)
- **mode-control ownership**: the session *initiator* runs the AMC and is
  the only sender of `MODE_CHANGE`; the responder's channel view reaches
  the initiator through ACK/keepalive SNR reports. Two independent
  controllers would collide mid-air (crossing MODE_CHANGEs diverge the
  committed mode). Responder-side `MODE` pinning is a v1 limitation.
- **mode-change commit (2026-09-05)**: a **demotion** is applied by the
  initiator **at the announcement**, not on `MODE_ACK`; the responder
  applies on receiving `MODE_CHANGE` as before, so a lost `MODE_ACK` no
  longer leaves the initiator one rung above the responder for the whole
  retry window with feedback riding the wrong transport. A **promotion**
  still applies on `MODE_ACK`: its channel is healthy by definition and
  the ACK arrives, while applying early made the initiator run ahead of
  the responder's ARQ window and ACK batching for one announcement's
  propagation (measured as a ~1 s transient on the VHF-FM pipe). The
  `MODE_ACK` is re-announced once if missing, with its deadline armed
  from the mode in force after the announcement.
  Responder adoption of the mode from `DATA` PHY headers was measured
  and rejected: it never fired across a 900 s half-duplex churn soak
  with ten mode changes, and cost the deep-tier throughput gate
  19% → 11%. Re-labelling already-queued `DATA` to the new mode was
  likewise rejected (frames are sized to their mode's codeword budget;
  it regressed the fading ramp 0.84 → 0.61).

### ARQ

Selective-repeat with cumulative base + gap bitmap:

- per-direction 16-bit sequence space, window size from the MAC profile
- `ACK` body (7 bytes): SNR report (s8 ×2), rx-quality flags (u8),
  `ack_base` (u16), `gap_bitmap` (u16, relative to base+1), CFO report
  (s8, 0.5 Hz units — drives TX frequency alignment, below)
- `rx_flags` bits: `0x80` + low nibble = responder mode request
  (responder-side pinning), `0x40` = turn-taking break request
  ("Turn-Taking", below), `0x20` = **explicit NAK**: a DATA-class burst
  from the peer was heard (PHY header decoded) but its payload failed
  every decode attempt. The receiver sends this ACK immediately; the
  sender retransmits the ACK's `ack_base` — the receiver's next-expected
  frame — at once instead of waiting out its retransmission timer
  (PACTOR's CS-NAK; measured 50–90 s per lost frame under fading
  without it). Sent at most once per failed burst
- the receiver finalizes a failing capture as soon as the header-implied
  burst airtime (plus pre-roll and a 0.5 s tail margin) is in hand,
  rather than re-decoding the same samples at ever-longer windows: HARQ
  combining, FFAIL/AMC accounting and the NAK all happen at burst end
- `NAK` is an `ACK` with at least one gap bit set — one format, one parser
- every data-bearing frame also piggybacks `ack_base` in the envelope, and
  receivers **consume** it: a session frame from the current session
  releases everything below the reported floor and resets retry budgets,
  exactly like a dedicated `ACK`. It reports no holes, so it never queues a
  retransmission and never re-arms the retransmission timer on its own.
  Pre-session frames (`CALL`/`START`…) and connectionless broadcasts carry
  `ack_base = 0` and are excluded
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
| `M10–M13` (HF band profile) | 16 | 4 |
| `M13–M14` (VHF-FM band profile) | 16 | 4 |

The HF fast-tier window was 8 until 2026-09-06. The per-turn statistic
(`SKW_TURN_DEBUG` `TURNSTAT`) showed ~2.4 KB in flight per round trip at
M10–M13 against a 3–8 KB RTT × rate product: window-limited. At 16 the
one-way half-duplex test went 86,016 → 106,496 B / 300 s and the coupled
two-way churn 91.2 → 196.7 KB — above listen-before-talk CCA on the
same schedule for the first time. The old caution ("the ARQ timeout
budgets one container of airtime, a deeper window times out its own
head-of-line burst") predated the measured-RTT timeout and the TX-queue
delay term, and did not reproduce.

Multi-frame bursts use the PHY **burst container**
([WAVEFORM.md](WAVEFORM.md)) — one preamble/sync/header for the whole
burst, link frames packed back-to-back inside. At `M7`–`M9` a container
gathers every retransmission that is due and tops up with new segments
while the window allows, so a retransmission does not fly alone paying a
whole preamble (frames are never held unacknowledged beyond the window
to fill one). Containers carry frames of one mode only, so an IR
step-down retransmission airs on its own rung. The window follows the
COMMITTED mode across `MODE_CHANGE` (resizing mid-session; shrink only
limits new sends). ACK deadlines arm as `timeout + TX-queue delay`:
the worker reports how much already-emitted audio is still waiting in
its TX path, since a frame sent behind that backlog cannot possibly be
ACKed before the backlog airs (without this, sustained TX made every
timer fire early and snowballed retransmissions). On the **VHF-FM band
profile** the top rungs
(`M13+`) additionally use a deep-pipe MAC: window 16, data codeword
targets that fill the 4095-byte PHY cap (per-burst preamble overhead
dominates top-rung throughput), and BATCHED feedback — one cumulative
ACK per 12 received data frames with an airtime-scaled tail flush when
the sender goes quiet — at 25 kbps the reverse channel would otherwise
consume ~25% of the airtime. Sender ACK timeouts scale to the batch.
This profile is deliberately NOT applied to HF `M13`: HF traffic is
interactive/small-transfer, the ARQ timeout budgets one container of
airtime, and a lost tail-flush ACK would stall the sender for the
batch-scaled timeout (observed as a 50 s data freeze in the real-device
endurance soak).

### Reconnecting to a Peer That Lost Its Session

Session resume restores a dropped session in place: the token, the ARQ
position and any file checkpoint all survive, and the byte stream
continues exactly. That requires the peer to still hold the session.

A peer that **restarted** holds nothing. It cannot honour the token, so
it refuses the `RESUME_REQ` with `START_NAK{ResumeInvalid}` and the
caller falls back to a clean `START` on the same call. `ResumeInvalid`
is never fatal — treating it as a hard refusal is what used to make
reconnect-after-peer-death impossible.

What happens to the data already queued matters, and the guarantee
changes:

- **Nothing is lost.** The sender re-sends from the last coalesced
  message the dead peer had fully acknowledged. It restarts at a
  MESSAGE boundary, never at an ARQ sequence boundary — a fresh peer
  has an empty assembly buffer, so a more-fragments continuation whose
  head it never saw would be dropped as malformed.
- **Delivery becomes at-least-once**, bounded by one message. A peer
  that dies between handing a message to its application and getting
  its acknowledgement back to us is indistinguishable, from the
  sender's side, from one that never received it. Replaying is the only
  choice that cannot lose data, so those bytes go again.
- **The uncertainty is published, not hidden.** The sender emits
  `EVT:STREAM_REPLAY <bytes>` naming exactly how many bytes it re-sent.
  An application that cannot tolerate a repeat discards that many bytes
  from the front of what follows; one that is idempotent ignores it.

Exactly-once across a peer restart is not achievable by any protocol
against a peer that keeps no state — it is the two-generals problem.
The design choice here is to prefer a bounded, reported repeat over
silent loss.

File transfers are not covered by stream replay: they have their own
checkpoint/resume keyed to a session the peer no longer has, so an
active transfer aborts with `EVT:FILE_ERROR <name> peer-restart`.

### Turn-Taking

Negotiated via `CAP_TURN_SCHED` (both peers must advertise it; a session
without it falls back to the legacy occupancy-gate scheduler). This is the
arbitration model every proven HF ARQ system uses — PACTOR/VARA/ARDOP's
ISS/IRS split — because occupancy sensing cannot arbitrate a half-duplex
link: each end is deaf while keyed and deaf again inside a fade, so a
busy-gate wedges in exactly the conditions that need arbitration most.

**Roles.** At session establishment the connect **initiator** holds the
**ISS** (information-sending station) role; the responder is the **IRS**.
The same rule applies to a resumed connect, which naturally gives the
first turn to whichever end had reason to re-establish the link.

- only the ISS sends `DATA` frames (new or retransmitted)
- the IRS sends feedback (`ACK`/`NAK`) and any control traffic; its ARQ
  retransmission timers are **suspended** (it is forbidden to resend, so
  a running timer would only burn retry budget); piggybacked `ack_base`
  on the new sender's frames keeps covering its in-flight frames
- control frames (`MODE_CHANGE`, `PROBE`, `MASK_UPDATE`, keepalive,
  voice, teardown) are role-independent; mode-control ownership stays
  with the session initiator regardless of the current turn role

**Break request.** An IRS with data waiting sets `rx_flags` bit `0x40`
(**break request**) in every ACK it sends, and arms a standalone ACK at a
round-trip-scaled cadence when the ISS has gone idle and no ACK is
otherwise due. The bit mirrors current state: a later ACK without it
withdraws the request.

**Session filter.** In-session frames (everything except CALL, CALL_ACK,
START, START_ACK, START_NAK, RESUME_REQ, RESUME_ACK, CQ, BEACON,
BCAST_DATA) are processed only while our session is up and only with its
`session_id`; others are dropped before they can touch ARQ, timers or the
TX frequency (every session used to run as id 0, so another pair's DISC
or ACK on the channel was acted on). Handshake frames are state-gated:
CALL_ACK only while Calling; START_ACK, RESUME_ACK and START_NAK only
while awaiting them and with our id; a CALL from a third station is
ignored while busy. A peer MASK_UPDATE excluding more carriers than our
own detector ever proposes (30%) is refused, and the PHY ignores any mask
leaving under half the carriers.

**Poll.** The ISS sends a window of bursts back to back and listens only
after the last, so it marks that burst with the PHY header's `poll` bit
([WAVEFORM.md](WAVEFORM.md)): the window is full, or nothing more is
queued or waiting. The IRS answers the burst that polls — with the gap
bitmap and any `rx_fail` flag its earlier bursts earned — and holds its
feedback otherwise. Answering every burst, as before, keyed the ACK into
the ISS's next burst while the ISS was transmitting and deaf (two-station
world, NARROW Moderate: every ACK lost, the AMC blind for 55 s, the
session idled out). The bit is in the PHY header because a burst whose
payload failed must still say whether another follows. Backstops: the
tail flush (airtime-scaled silence, measured from the last burst HEARD,
decoded or not) and the sender's ARQ timer, whose open-loop value covers
a whole window of bursts plus the ACK, since the first frame's ACK now
comes back a window later. After a `MODE_CHANGE` the ISS also holds DATA
until the `MODE_ACK` arrives (at most one robust control frame plus 2 s),
because the responder answers the announcement the moment it decodes
it. A burst from the peer whose PHY header decoded refreshes the idle
clock even when its payload failed: the peer is alive and keyed toward
us, and a session at a mode the channel just outgrew must not idle out
before the demotion reaches the air.

**Handover.** When the peer has requested the turn, the ISS keeps it for
a **minimum hold** of 4 measured round trips (two send-and-acknowledge
cycles; at 2 the ISS used 7–40% of its own turn — one window, its ACK
wait, then a handover whose cost is itself ~1 RTT; at 4 the robust tier
reached ~80%), **and at least 6 s** — at the fast tier a round trip is
only ~3 s, so four of them buy one burst and its ACK, which TURNSTAT
showed as 15 s turns carrying 4.6 frames at M13. Then it stops pulling new
segments and sends `TURN_GRANT` (u8 epoch, incremented per handover) at
the **next window boundary** — the moment nothing is in flight and
nothing already pulled is waiting to air. An ISS with no backlog hands
over as soon as its window clears. A window that will not clear during
the drain means a lost ACK or a lost frame; since the drain pulls no new
segments, the ISS fast-retransmits the head-of-line frame once after one
handoff interval (RTT-paced) rather than waiting for that frame's ARQ
timer — 90 s at the robust tier, measured as 110 s of dead air with one
frame in flight and the gate open. The only fixed timer is the deaf-peer
backstop, also RTT-paced: two handoff intervals (≥ 12 s), after which
the turn is granted with the frame stranded for the peer's turn — a
turn cycle, against the 180 s (two ARQ timeouts) the old cap cost the
schedule. (Earlier: a fixed `max(45 s, 3 × ARQ timeout)`
quantum, whose drain test also required the proto backlog to be empty —
so under continuous load every handover waited out the full backstop.
Measured as the whole of turn-taking's throughput deficit against CCA on
the two-station world.) On sending the grant the ISS becomes the IRS
immediately; the peer's first `DATA` frame confirms the handover. An
unanswered grant is retried at the ARQ-timeout cadence up to 3 times,
after which the granting station **reclaims** the ISS role — a link must
never be left with two listeners because one short control frame faded.
The epoch makes retried grants idempotent at the receiver.

**Divergence healing.** If a lost grant series ever leaves both ends
believing they hold the ISS role, receiving a `DATA` frame while holding
ISS resolves it deterministically: the **responder yields**, the
initiator holds. Crossing grants (both ends yielding at once) cancel the
receiver's own outstanding grant when a grant is accepted.

**What stays physical.** The scheduler replaces occupancy sensing for
DATA arbitration only. Unchanged: at most one burst in the TX path at a
time, the post-burst listen window sized to the measured round trip
(ended early by feedback), and the busy-hold that keeps **any** frame —
the ISS's DATA included — from keying over a burst still airing. That
last one is half-duplex physics, not arbitration: a keyed peer is deaf.
Measured without it, the new ISS keyed the instant it decoded
`TURN_GRANT` while the rest of the granting burst (ACK and probe frames
bundled behind it) was still on the air, the peer never heard the
confirming DATA, every retransmit collided the same way, and the granter
reclaimed into a two-sender minute. Same runaway cap as the control
hold (30 × the mode's airtime scale); no post-clear jitter, since the
ISS is the only sender.

**Scope — where listen-before-talk stays.** The schedule governs the
**connected session** only. Everything without an ARQ partner to
schedule with keeps listen-before-talk: pre-session traffic (`CALL` and
the rest of the handshake, with the jittered retry backoff), and
connectionless frames (`CQ`, `BEACON`, `BCAST_DATA`) — the busy-detector
hold with anti-collision jitter and the courtesy cap. This is the PACTOR
/ VARA split exactly: listen for a clear channel, call, then run the
scheduled exchange.

### TX Frequency Alignment

RX-side CFO correction cannot fix the physical problem that a mistuned
signal sits shifted inside the peer's SSB filter (a 50 Hz offset pushes the
`WIDE` band edge from 2662 Hz to ~2712 Hz — into the filter skirt). Each
station therefore steers its **transmit** signal onto the peer's frequency
grid:

- `CALL_ACK` and every `ACK` carry the receiver's measured CFO
- **connect-time bootstrap**: connected-signature frames capture only a
  few Hz of CFO, so the correction must land before data traffic starts.
  The calling-signature handshake measures the offset (±50 Hz capture):
  at CONNECTED, the initiator applies the peer's `CALL_ACK` report in
  full and the responder applies its own measurement via reciprocity
  (if I hear you X high, you hear me X low), both clamped ±50 Hz
- **tracking**: thereafter each transmitter moves its shift by
  −(reported CFO)/2 per report, clamped to ±50 Hz. The ½ is a loop gain,
  not a split of the offset: the two stations' loops see each other's
  corrections, and gain ½ converges in a few exchanges without overshoot.
  In steady state each station's shift equals the full offset of the path
  it transmits on (each station moves its signal onto the other's
  frequency).
- **reverse path**: a report rides only in an `ACK`, and in a one-way
  flow the data sender sends none, so after a retune the receiver's
  transmissions stayed off by the whole step. A station that measures
  the peer's frames ≥ 2 Hz off, with no `ACK` sent since that
  measurement, sends a standalone `ACK` (at most one per 15 s; `OFDM`
  modes only). It is the ordinary `ACK` format, so the peer's loop
  converges as above and older peers interoperate unchanged.
  (Documentation before 2026-10 described this as a "split
  correction" capping each station's excursion at half the mistuning;
  the implementation never did that.)
- since v1.2.0 alignment is filter hygiene, not a decode precondition:
  every frame signature acquires within ±55 Hz (GAP_CLOSURE_PLAN.md 8.2)
  and tracks in-frame drift (8.6), so a retune or LO drift is decoded
  first and re-aligned after
- the current correction is visible in `STATUS` (`CFO=` field) and
  telemetry; a shift past ±40 Hz raises `EVT:CFO_WARN` — the dial needs
  retuning before the ±55 Hz acquisition margin runs out

### Keepalive

- initiator-only, `M8+` sessions only (robust modes must not burn airtime)
- 20 s idle interval; body: 1-byte remote-SNR report
- sessions in `M4–M7` rely on the idle-disconnect timer alone

## Payload Pipeline (`proto` layer)

**Adaptive stream message bound.** Stream bytes coalesce to a message
that is delivered to the application only when every fragment has
arrived. On a clean link OFDM modes coalesce to the full 4 KiB (a flat
cap measurably costs throughput). Once the sender's retry ratio says the
link is lossy (enter > 0.15, leave < 0.05, hysteresis) the bound drops
to ~8 frames' worth — the deep tier's permanent rule — so one stuck
head-of-line fragment cannot zero delivery of a whole 4 KiB message
while later fragments keep arriving. TX-local policy; receivers
reassemble whatever sizes arrive.

A further **airtime** bound — cap a message at what the link carries in
N seconds, so the bound follows the mode rather than the tier — was
built and measured, and is off (`kDeliverTargetSec = 0`). Delivery gaps
under turn-taking are floored by the turn cycle (~85 s: a station
delivers only what arrived while the peer held the turn), so messages
smaller than that buy nothing; measured, the bound never helped and cost
up to 20% on a seed. `SKW_DELIVER_TARGET_SEC` re-enables it and
`SKW_DELIV_DEBUG` reports the gaps it is meant to shrink.

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

- negotiated capability; token minted at `START_ACK` (8-byte ID bound to
  family + callsign pair) and **carried in the `START_ACK` body** — both
  ends hold it, so either may initiate the resumed reconnect
- on reconnect within the resume window (10 min, running from the drop),
  initiator sends `RESUME_REQ(token, last_rx_seq[, file_ckpt])` in the
  `START` slot after `CALL_ACK`; responder validates and answers
  `RESUME_ACK(last_rx_seq[, file_ckpt])` — sequence spaces continue, and
  each side's optional `file_ckpt` (file_id, contiguous offset) tells the
  peer's paused file TX where to rewind (overlap with already-received
  bytes is absorbed by the receiver)
- an invalid/expired token draws `START_NAK(reason=RESUME_INVALID)`; the
  initiator falls back to a clean `START` on the same call (never a
  protocol error)
- resume state is invalidated by family change, crypto state change,
  token expiry — or a **clean disconnect**: only failure-mode teardowns
  (ARQ retry exhaustion, idle timeout, a peer `DISC` with a for-cause
  reason) pause the pipeline for resume; an operator `DISC(normal)` ends
  the session for real, and the `DISC` reason field carries the
  distinction to the peer

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
- **known incompatibility**: builds up to v1.0.0 enforced "reserved caps
  must be 0" by *rejecting* the frame, so they drop a `CALL` advertising
  `CAP_TURN_SCHED` (bit 8) instead of negotiating it away. Calling such a
  build requires suppressing the bit (`SKW_NO_TURNSCHED=1`)
- reserved `M0–M3` mode indices in any control field are treated as invalid
  in v1 and clamp to `M4`
