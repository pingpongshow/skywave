# Skywave Waveform Specification

## Design Summary

One spread-OFDM waveform covers the entire operating range. Robustness at low
SNR comes from three composable tools — low-order constellation, LDPC coding,
and **frequency-domain spreading** — rather than from a second waveform
family. Acquisition uses a chirp preamble whose signature also selects the
PHY header class.

## Band Profiles

All numerology below — sample-domain plan, FFT size, subcarrier spacing,
CP set, chirp spans, family tables — constitutes the **`HF` band profile**,
the only profile in v1. The implementation must treat these values as one
profile table (`src/core/constants.hpp` + mode tables), not as scattered
constants: a future profile (see *VHF-FM Band Profile* at the end of this
document) is then a second table plus tuning, never engine surgery. Band
profiles are a local configuration concept — stations on different bands
never hear each other, so no wire-format field is needed.

## Numerology (`HF` profile)

| Parameter | Value | Rationale |
|---|---|---|
| Sample rate | `48000 Hz` | universal soundcard rate |
| FFT size | `1280` (2^8 × 5, mixed radix) | subcarrier spacing 37.5 Hz |
| Subcarrier spacing | `37.5 Hz` | ~2× more Doppler-tolerant than a 2048 grid while keeping CP overhead moderate |
| Waveform center | `1500 Hz` | industry-standard HF modem center; bin 40 falls exactly on it |
| Useful symbol time | `26.667 ms` | 1280 / 48000 |

### Cyclic Prefix

| CP mode | Samples | Duration | Symbol rate | Use |
|---|---:|---:|---:|---|
| `SHORT` | 96 | 2.0 ms | 34.88 sym/s | benign single-hop paths |
| `MEDIUM` | 192 | 4.0 ms | **32.61 sym/s** | default; covers typical multipath |
| `LONG` | 288 | 6.0 ms | 30.61 sym/s | severe multihop / NVIS spread |

All rate tables in this document assume `MEDIUM` CP. CP mode is negotiated at
connect and may be changed with `MODE_CHANGE`.

## Bandwidth Families and Carrier Plans

Subcarrier `k` sits at `k × 37.5 Hz`. Bin 40 = 1500 Hz exactly.

| Family | Active bins | Carriers | Carrier span | Occupied BW |
|---|---|---:|---|---:|
| `ULTRA` | `5..76` | 72 | 187.5 – 2850.0 Hz | ~2.7 kHz |
| `WIDE` | `8..71` | 64 | 300.0 – 2662.5 Hz | ~2.4 kHz |
| `MID` | `27..53` | 27 | 1012.5 – 1987.5 Hz | ~1.0 kHz |
| `NARROW` | `34..46` | 13 | 1275.0 – 1725.0 Hz | ~0.5 kHz |

`MID` and `NARROW` use odd carrier counts so the center carrier lands exactly
on bin 40 (1500 Hz). `WIDE`, `MID`, and `NARROW` fit inside a 300–2700 Hz
stock SSB channel.

`ULTRA` is the **opt-in extended tier**: it requires a radio whose TX/RX
audio passband reaches ~100–2900 Hz (modern DSP rigs in data mode, SDRs)
and must be explicitly enabled in configuration on **both** stations —
`WIDE` remains the default wideband family. ULTRA buys ~12.5% more
throughput at every ladder rung in exchange for hardware selectivity; its
edge guards are thinner (≈ 88 Hz low, 50 Hz high), so it leans on the
split-correction rule of TX frequency alignment (see
[PROTOCOL.md](PROTOCOL.md)) to stay inside real filters.

### Pilots

Scattered pilots, not fixed tones:

- pilot carrier positions satisfy `index ≡ (symbol_number mod 8)` stepping by 8
  across the family's active carriers
- over any 8 consecutive symbols, every carrier is sounded once
- pilot overhead: `1/8` of carriers per symbol (avg 8.0 / 3.375 / 1.625
  pilots for `WIDE` / `MID` / `NARROW`)
- pilots are boosted `+2 dB` relative to data carriers and carry a known
  length-matched Zadoff-Chu-derived phase sequence

Average data carriers per symbol: `ULTRA` 63, `WIDE` 56, `MID` 23.6,
`NARROW` 11.4.

## Frequency-Domain Spreading

Robust modes repeat each modulated symbol across `S` carriers before IFFT:

- spreading factors: `S ∈ {1, 2, 4, 8, 16}`
- the `S` copies are placed on maximally separated positions **within a
  spread group's pooled data carriers** (stride = pooled_carriers / S). A
  group is the smallest run of consecutive OFDM symbols whose pooled data
  carriers can hold at least one spread-S source symbol — one symbol for
  almost every family/S combination, but `NARROW` at `S16` (11–12 data
  carriers per symbol) pools **two consecutive symbols**, making its
  deepest mode frequency+time spread. The scattered pilot positions rotate
  every symbol, so the pool is computed per group, identically at TX and RX
- each copy is rotated by a per-copy pseudorandom phase (whitening the PAPR
  and decorrelating narrowband interference)
- the receiver maximum-ratio-combines the `S` copies **after** equalization,
  producing one soft symbol with up to `10·log10(S)` dB of SNR gain on flat
  noise and large diversity gain on selective fading

Spreading multiplies robustness and divides throughput by `S`. It changes no
other part of the chain — same FFT, same pilots, same LDPC.

## FEC and Bit Pipeline

TX bit pipeline per frame:

```text
payload bytes -> CRC-32 -> whitener (LFSR x^17+x^12+1) -> LDPC encode
  -> bit interleave (frequency + time) -> constellation map (Gray)
  -> spread (xS) -> pilot insertion -> IFFT -> CP -> window/overlap -> samples
```

### LDPC

IEEE 802.11n LDPC matrices (public, well-tested, table-driven):

| Codeword | Rates | Use |
|---|---|---|
| `N=1944` | 1/2, 2/3, 3/4, 5/6 | data frames (payload ≥ ~100 bytes) |
| `N=648` | 1/2, 2/3, 3/4 | control frames, PHY header, short data |

Decoder: normalized min-sum, 5-bit soft LLRs, ≤ 30 iterations with early
termination on parity check. Codeword shortening (zero-pad info bits) and
puncturing (for HARQ-IR) follow standard 802.11n procedure.

### Interleaving

- **bit interleaver**: within a codeword, a row/column block interleaver maps
  adjacent code bits onto non-adjacent carriers and symbols
- **time span**: one codeword spans at most ~1.2 s of symbols in robust
  spread modes (S8/S16); deep multi-second interleaving is deliberately
  avoided to keep half-duplex ARQ turnaround interactive — time diversity at
  low SNR comes from HARQ retransmission combining instead
- **whitener**: LFSR scrambler breaks payload periodicity before coding

## Mode Ladder

One ladder shape for all families; only the rate numbers differ. `M0–M3`
are the CSS deep tier (Phase 6, gated on `CAP_CSS_TIER`); `M14` is the
VHF-FM 256QAM top rung (Phase 7, unreachable on HF). The ladder is
ordered strictly by robustness.

### Common Mode Definitions

| Mode | Waveform | Constellation | LDPC rate | Spread | Notes |
|---|---|---|---|---:|---|
| `M0` | CSS | — | LDPC 1/2 (soft) | SF8 | deep tier; CAP_CSS_TIER-gated |
| `M1` | CSS | — | Hamming(7,4) | SF7 | |
| `M2` | CSS | — | Hamming(7,4) | SF6 | |
| `M3` | CSS | — | Hamming(7,4) | SF5 | |
| `M4` | OFDM | QPSK | 1/2 | 16 | robust floor; handshake transport |
| `M5` | OFDM | QPSK | 1/2 | 8 | |
| `M6` | OFDM | QPSK | 1/2 | 4 | |
| `M7` | OFDM | QPSK | 1/2 | 2 | |
| `M8` | OFDM | QPSK | 1/2 | 1 | first unspread mode |
| `M9` | OFDM | QPSK | 3/4 | 1 | |
| `M10` | OFDM | 16QAM | 1/2 | 1 | |
| `M11` | OFDM | 16QAM | 3/4 | 1 | |
| `M12` | OFDM | 64QAM | 2/3 | 1 | |
| `M13` | OFDM | 64QAM | 5/6 | 1 | HF top mode |
| `M14` | OFDM | 256QAM | 5/6 | 1 | VHF-FM only (floor sentinels on HF) |

### ULTRA Rates and Floors (opt-in tier)

| Mode | GROSS bps | NET bps | Floor |
|---|---:|---:|---:|
| `M4` | 257 | 128 | −11.0 dB |
| `M5` | 514 | 257 | −8.5 dB |
| `M6` | 1027 | 514 | −5.5 dB |
| `M7` | 2054 | 1027 | −3.0 dB |
| `M8` | 4109 | 2054 | +1.0 dB |
| `M9` | 4109 | 3082 | +3.5 dB |
| `M10` | 8218 | 4109 | +6.0 dB |
| `M11` | 8218 | 6163 | +10.0 dB |
| `M12` | 12326 | 8218 | +14.0 dB |
| `M13` | 12326 | 10272 | +17.5 dB |

(ULTRA floors sit 0.5 dB above WIDE's at each rung — same Eb/N0, more bits
per second in the 3 kHz reference bandwidth.)

### WIDE Rates and Floors

NET = post-FEC payload rate; GROSS = uncoded rate. Floors are the design
entry SNR on **AWGN** (3 kHz noise reference, ≤10% FER target) and are
the `kFloors` table in `core/constants.hpp` (re-measured for v1.2.0,
1.5–3 dB below the v1.1.0 values; campaign in
`docs/conformance/floors_v1.2/`). The CCIR fading floors are a
separate and much higher table (`kFadingFloors`, "Measured FADING floors"
below) — the AMC keys off those by default. AMC adds hysteresis margins
on top of whichever table is active
([LINK_ADAPTATION.md](LINK_ADAPTATION.md)).

Because the conformance sweep tests **floor + 1 dB** against the ≤10%
spec, a table entry sits about 1 dB below the mode's true 10%-FER point.
That convention is what makes an entry verifiable; changing an entry
without re-running `bertool` breaks it.

| Mode | GROSS bps | NET bps | Floor |
|---|---:|---:|---:|
| `M4` | 228 | 114 | −11.0 dB |
| `M5` | 457 | 228 | −8.5 dB |
| `M6` | 913 | 457 | −6.0 dB |
| `M7` | 1826 | 913 | −3.0 dB |
| `M8` | 3652 | 1826 | +0.5 dB |
| `M9` | 3652 | 2739 | +3.0 dB |
| `M10` | 7305 | 3652 | +5.5 dB |
| `M11` | 7305 | 5478 | +9.5 dB |
| `M12` | 10957 | 7305 | +13.5 dB |
| `M13` | 10957 | 9131 | +17.0 dB |

### MID Rates and Floors

| Mode | GROSS bps | NET bps | Floor |
|---|---:|---:|---:|
| `M4` | 96 | 48 | −15.0 dB |
| `M5` | 193 | 96 | −13.0 dB |
| `M6` | 385 | 193 | −10.0 dB |
| `M7` | 770 | 385 | −7.5 dB |
| `M8` | 1540 | 770 | −3.5 dB |
| `M9` | 1540 | 1155 | −0.5 dB |
| `M10` | 3079 | 1540 | +2.0 dB |
| `M11` | 3079 | 2309 | +6.0 dB |
| `M12` | 4619 | 3079 | +9.5 dB |
| `M13` | 4619 | 3849 | +13.5 dB |

### NARROW Rates and Floors

| Mode | GROSS bps | NET bps | Floor |
|---|---:|---:|---:|
| `M4` | 46 | 23 | −17.5 dB |
| `M5` | 93 | 46 | −16.0 dB |
| `M6` | 186 | 93 | −13.0 dB |
| `M7` | 371 | 186 | −10.5 dB |
| `M8` | 742 | 371 | −6.5 dB |
| `M9` | 742 | 557 | −3.5 dB |
| `M10` | 1484 | 742 | −1.0 dB |
| `M11` | 1484 | 1113 | +2.5 dB |
| `M12` | 2226 | 1484 | +6.5 dB |
| `M13` | 2226 | 1855 | +10.5 dB |

`*` Narrow-family decode floors below ~−12 dB exceed the chirp acquisition
floor; in practice connection establishment limits `NARROW` to ~−12 dB, with
`M4/M5` retaining their full depth for an already-synchronized session.

Note: narrower families reach deeper 3 kHz-referenced floors because the same
transmit power concentrates into less bandwidth — this replaces (part of) the
role the reference design's NB ladder played.

## Acquisition and Synchronization

### Chirp Preamble

Linear chirps spanning the family's carrier band:

| Family | Sweep | Duration per rep |
|---|---|---:|
| `ULTRA` | 200 → 2850 Hz | 160 ms |
| `WIDE` | 300 → 2700 Hz | 160 ms |
| `MID` | 1000 → 2000 Hz | 160 ms |
| `NARROW` | 1275 → 1725 Hz | 160 ms |

Preamble signatures (chirp direction pattern) carry one bit of blind
information — the **header class**:

| Signature | Meaning | Reps |
|---|---|---:|
| up–up | fast header follows | 2 |
| up–down–up–down | robust header follows | 4 |
| up–up–down–down | calling/beacon (robust header) | 4 |
| up–down–down–up | CSS frame follows (waveform=1) | 4 |

Every multi-rep signature that must support CFO measurement contains both
chirp slopes: an LFM chirp's ambiguity function is a delay–Doppler ridge,
so frequency offset is observable only as the *opposing* peak-time shifts
of up-reps versus down-reps. All eight patterns (four signatures × normal /
spectrum-inverted) remain mutually distinct; `up–up` is a prefix of the
calling signature and the middle of the inverted CSS signature, so a
detector that has seen two up-reps checks the surrounding rep slots before
classifying fast vs a containing 4-rep signature. This signature IS the
sync arbitration: the preamble alone announces OFDM vs CSS, so the
receiver never trial-decodes both.

The robust class uses 4 reps so preamble detection always retains ≥ 3 dB of
margin over the deepest payload floor it announces (a 2-rep preamble would
sit within 0.5 dB of the `WIDE M4` floor — too thin). The fast class (`M8+`
sessions, floors ≥ +2 dB) keeps the cheap 2-rep form.

Detection: matched filter (complex correlator against the analytic chirp),
normalized by local energy; threshold set for < 1 false alarm / 10 min in
noise. 4-rep acquisition floors (3 kHz ref): `ULTRA`/`WIDE` ~−12 dB,
`MID` ~−14 dB, `NARROW` ~−15 dB (narrow families concentrate preamble power exactly as they
do payload power). The correlation peak gives coarse timing (±1 ms) and the
inter-rep phase slope gives coarse CFO. CFO capture: **±50 Hz for every
signature** (since v1.2.0; real SSB stations mistune by tens of Hz and
drift mid-session). CFO is measured from slope diversity — up-rep and
down-rep correlation peaks shift oppositely under CFO (Δt = ±Δf·T/B), so
their difference yields coarse CFO and their mean yields true timing;
inter-rep correlation phase then refines to <1 Hz within its ambiguity
(1/(gap·T)). The up–up fast signature has no slope diversity, and
multipath biases the coarse estimate of the others, so the receiver
resolves the ambiguity branch by ZC-scored, header-verified search over
the whole ±55 Hz ridge (DSP.md `BRANCH`). (Before v1.2.0 the fast
signature captured only ±3.1 Hz - the documented ±5 Hz and its "peak-shift
disambiguation" never existed - and any retune or 0.02 Hz/s of drift
killed a session.)

### Fine Sync Symbols

Immediately after the chirp:

- **fast class — 3 identical ZC symbols** (~92 ms): a Zadoff-Chu sequence
  (length = family carrier count, root chosen per family) mapped onto the
  active carriers. The receiver matched-filters each symbol against the
  known sequence and estimates CFO from the phase progression of the
  per-symbol correlations — blind pair autocorrelation is useless at low
  SNR where the noise×noise term dominates. Three symbols (two phase
  intervals + a lag-2 pair) are the minimum that meets the < 0.5 Hz CFO
  gate with margin at the fast floors; two would sit exactly on the
  boundary.
- **robust class — 6 ZC symbols** (~184 ms): a single ZC symbol is far too
  noisy for a per-carrier channel estimate at the robust floor (−8 dB
  in-band), and the CFO phase-slope variance over fewer intervals lands on
  the 0.5 Hz gate rather than under it. Six symbols give σ ≈ 0.26 Hz CFO
  at the WIDE M4 floor and average six channel-estimate snapshots
  (+~4 dB). Cost: ~184 ms against a multi-second robust header —
  negligible.

Sync symbols always use `MEDIUM` CP regardless of the payload's CP mode:
they precede the PHY header that announces it.

### PHY Header

A self-describing header follows the sync symbols so the payload can be
decoded without prior negotiation (enables monitoring and simplifies
handshake):

| Field | Bits | Meaning |
|---|---:|---|
| `magic` | 8 | `0x5C` |
| `version` | 4 | wire version, v1 = `1` |
| `waveform` | 4 | `0=OFDM`, `1=CSS (reserved)` |
| `family` | 2 | `0=WIDE, 1=MID, 2=NARROW, 3=ULTRA` |
| `mode` | 4 | ladder index of the **payload** |
| `cp_mode` | 2 | payload CP |
| `frame_type` | 6 | link frame type ([PROTOCOL.md](PROTOCOL.md)) |
| `length` | 12 | payload bytes |
| `seq_low` | 8 | low 8 bits of link sequence (dedup aid) |
| `css_hdr1` | 1 | CSS frames only: `1` = a single header copy is on air (connected-session short layout, capability-gated), `0` = two copies (the v1 time-diversity layout). Former pad bit, so v1 frames are byte-identical. The receiver decodes copy 1 before it needs the payload position, so the layout never has to be known in advance |
| `pad` | 5 | zero |
| `crc16` | 16 | header CRC |

Header transport (fixed, family-wide, never negotiated):

- **fast header**: QPSK, LDPC `N=648 r1/2`, spread ×2 — ~0.35 s (`WIDE`),
  decodes to ~−1 dB; used when the session mode is `M8+`
- **robust header**: QPSK, LDPC `N=648 r1/2`, spread ×16 — decodes to the
  family floor; used for handshake, beacons, and sessions in `M4–M7`.
  Durations: ~2.5 s `ULTRA`, ~2.8 s `WIDE`, ~6.8 s `MID`, ~14 s `NARROW`
  (the price of the narrow families' very low coded rates; see the NARROW
  acquisition caveat)

The chirp signature announces which header class follows, so the receiver
never trial-decodes both.

### Header Codeword Payload Piggyback

The `N=648 r1/2` header codeword carries 324 info bits and the header needs
only ~66 + CRC. The remaining **~30 bytes carry the leading link-payload
bytes**, transported at the header's own robust parameters. Consequences:

- ACKs, keepalives, probes, and most control frames fit **entirely inside
  the header codeword** — one codeword after sync, no separate payload
  section. At `M4` this halves control-frame airtime (an ACK is ~3.5 s
  instead of ~5.6 s including preamble and sync).
- `length` still states the total link payload; payload codewords at the
  mode's own parameters follow only for the bytes beyond the piggyback
  capacity.
- The piggyback region is part of the link payload for CRC and crypto
  purposes; the split is purely a transport detail.

### Frame-on-Air Layout

```text
[chirp preamble][ZC block][header codeword(s)][payload codewords][tail pad]
```

Payload symbols continue the scattered-pilot pattern; the equalizer tracks
from the ZC estimate through the frame (see [DSP.md](DSP.md)).

### Burst Container (Aggregated Transmissions)

A TX burst carrying multiple link frames (MAC aggregation,
[PROTOCOL.md](PROTOCOL.md)) pays the preamble + ZC + header cost **once**:
the PHY payload is a container of back-to-back link frames, each delimited
by its own envelope and CRC, and the PHY header `length` covers the total.
Without this, per-frame acquisition overhead is ~17% at `M13` with 4-frame
bursts; with it, ~5%. Rules:

- all frames in a container share the burst's mode/CP (they already must —
  one transmission, one waveform)
- a container is still one acquisition event: if the header fails, the
  whole burst is lost (acceptable — the fast header decodes ≥ 3 dB below
  any mode that uses aggregation)
- receivers parse greedily: envelope `length` fields walk the container;
  a CRC failure skips to the next envelope boundary candidate only via the
  known length chain (no resync heuristics mid-container)
- voice talkspurt streaming ([VOICE.md](VOICE.md)) is the degenerate case:
  an open-ended container

### Sideband-Inversion Tolerance

If one station operates LSB against USB, the spectrum flips around the
center: every up-chirp is received as a down-chirp. All three preamble
signatures remain **unambiguous under inversion** (up–up ↔ down–down,
up–down–up–down ↔ down–up–down–up, 4×up ↔ 4×down — none collide with a
valid signature). The sync engine therefore detects inversion for free and
**auto-corrects** by conjugating the complex baseband before demodulation,
logging the condition and flagging it in telemetry so the operator can fix
the radio setting. Auto-correction is RX-only; the modem never transmits
inverted.

## PAPR and TX Shaping

- per-copy pseudorandom spreading phases and the whitener keep PAPR near the
  Gaussian baseline
- a soft clipper at 8 dB PAPR ceiling followed by the occupied-band filter
  bounds out-of-band regrowth
- TX level normalization targets consistent RMS across frame types
  ([DSP.md](DSP.md))

## CSS Deep Tier (`M0–M3`)

Modem layer (ROADMAP 6.1, `src/dsp/css.{hpp,cpp}`) and full PHY/link
integration (6.2: `waveform=1` frames, sync arbitration by preamble
signature, `CAP_CSS_TIER` negotiation, handshake-on-CSS, AMC ladder
extension).

**Frame on air** (`waveform=1`): chirp preamble (signature up–down–down–up)
→ `kCssSyncSymbols` known shift-0 SF8 symbols (fine timing to the sample;
CFO↔delay coupling makes residual CFO a pure common shift absorbed here;
multipath tap profile for RAKE) → PHY header **twice** (Hamming(7,4) bit
layer at fixed SF8, two independently interleaved copies ~2 s apart —
time diversity where a short section cannot interleave through a fade;
CRC-16 arbitrates) → payload as **LDPC r1/2 (N=648 only)** over
soft-demodulated symbols (dual-max bit metric), whitened, trailing
CRC-16. A known SF8 pilot precedes every `kCssPilotInterval` (8) data
symbols of every section: it re-anchors timing (sample-clock drift) and
refreshes the persistent RAKE tap tracker (Rayleigh taps trade dominance
on ~1 s scales while deep frames run 10+ s; a tap's remembered strength
decays over a few pilots rather than vanishing the instant it fades).
Hard-decision Hamming remains the header transport and the aligned
modem-layer API; measured ~4–6 dB shy of soft LDPC for long payload
sections under fading (the 1-error-per-codeword cliff).

**Header copies — measured (2026-09-05).** The second SF8 header copy is
~1.9 s per frame on WIDE and ~9.6 s on NARROW, so a single-copy
"connected short layout" (`css_hdr1` header bit, `bertool --css-frames
--css-hdr 1`) was built and swept at the fading floors, CCIR Moderate,
24 trials/point: it saves ~11% of frame airtime (WIDE M1 17.2 → 15.3 s,
MID M0 57.2 → 52.6 s) and costs it back many times over in header loss —
MID M0 FER 42% → 71% at the floor and 12.5% → 42% at +3 dB, MID M1 0% →
29% at +3 dB, WIDE M1 8% → 12.5% at +3 dB — and a lost header is a lost
35–57 s frame. Net loss everywhere the tier operates; the time-diversity
argument above stands. The layout bit and sweep remain as measurement
infrastructure; no frame is sent single-copy.

**Symbol layer.** LoRa-style chirp spread spectrum: a symbol is the base
up-chirp cyclically shifted by the symbol value, `2^SF` chips per symbol
carrying SF bits, SF8 (`M0`, deepest) … SF5 (`M3`). The chip rate is the
family bandwidth snapped so a chip is an exact integer number of 12 kHz
baseband samples: WIDE 2400 Hz (5 samples/chip), MID 1000 Hz (12),
NARROW 480 Hz (25), ULTRA 2400 Hz (5 — snapped DOWN so the chirp stays
inside the 2700 Hz occupied band). `kModes.spread` holds the SF.

**Demodulation.** Non-coherent matched-filter bank over all `2^SF` cyclic
shifts, computed as one FFT cross-correlation
(`IFFT(FFT(sym)·conj(FFT(base)))`, magnitude sampled at the chip-aligned
lags). This is the AWGN-optimal detector; the classic dechirp +
chip-rate-decimate + small FFT was measured ~2–3 dB worse at the
oversampled rate (large shifts alias across the chip-rate Nyquist, and
split-bin folding doubles the noise). Symbol quality proxy: peak/mean
magnitude ratio over the lag bank (~0.5·2^SF on a clean channel).

**Bit layer.** Bin index = `fromGray(bits)` so an adjacent-bin demod error
costs exactly one data bit; Hamming(7,4) per nibble; depth-7 block
interleaver spreads each codeword across 7 symbols, so one wholly wrong
symbol costs each touched codeword at most one (corrected) bit for
SF ≤ 7.

**Measured AWGN floors** (dB SNR, 3 kHz noise reference; one-shot FER
≤ ~15% at the floor, `cssFloorDb`, bertool `--css`, 2026-07-29 —
`docs/conformance/skw_css_awgn.csv`). The CSS band is the noise
bandwidth, so narrow families reach deepest:

| Family | `M0`/SF8 | `M1`/SF7 | `M2`/SF6 | `M3`/SF5 |
|---|---:|---:|---:|---:|
| WIDE | −13.0 | −10.5 | −8.0 | −5.0 |
| MID | −16.5 | −14.0 | −12.0 | −9.0 |
| NARROW | −16.0 ¹ | −15.5 ¹ | −15.0 | −12.0 |
| ULTRA | −12.5 | −11.0 | −8.0 | −5.0 |

¹ NARROW `M0`/`M1` are FULL-FRAME floors (v1.2.1). The payload alone
reaches −19.5/−17.5 dB, but the chirp preamble, sync and header cannot
follow it: with `bertool --css-frames`, M0 loses 30% of frames at
−16.5 dB and M1 23%. The table holds what a frame achieves, because the
AMC steers on it. MID/WIDE/ULTRA `M0` meet floor+1 ≤ 10% on full frames
as published. Recovering NARROW's 3 dB is an acquisition problem
(ROADMAP).

The original forward estimate ("floors −9 → −14 dB") predates the
per-family measurement; the −14 dB tier target is met by MID/NARROW `M0`
(FER ≈ 0 at −14 dB). CFO tolerance: ±half the connected-preamble capture
range (chirp CFO↔delay coupling; the sync symbols absorb the residual).
**LO drift:** the receiver removes the preamble CFO once, so drift leaves
a residual r·t, and a chirp turns that into a timing offset proportional
to 2^SF. The SF8 pilots move the RAKE taps 2^(8−SF) times further than an
SF5–SF7 payload needs. The same coupling makes drift visible: the pilot
peaks walk linearly, at r·symH·spc/fs samples per second. If a header or
payload fails, the receiver estimates r from the pilot offsets it has
just read, derotates the quadratic phase, re-reads (twice, to refine) and
decodes the best-correlating candidate within ±0.02 Hz/s of the estimate
under the CRC. Every family's `M0–M3` holds floor+3 to ±0.3 Hz/s
(ROADMAP 8.8 and the 2026-10-13 remaining-gaps round).

**Measured FADING floors** (one-shot FER ≤ ~10%, full frames, designated
channels per the `kFadingFloors` convention — CCIR Moderate, NARROW on
CCIR Good; 2026-07-30): WIDE −3/0/NA/NA, MID −10/−7/−5/−3, NARROW
−11/−9/−8/−7, ULTRA −4/−2/NA/NA (`M0..M3`; NA = flat outage tail where
the 1 ms delay spread spans 2.4 of the 2400 Hz families' chips — those
rungs are probe-gated near-static modes, ARQ territory otherwise).
One-shot fading sits well above the AWGN floors for the same Rayleigh
outage reasons as the OFDM tier; the LINK operates below them: measured
two-modem connect + transfer at −13 dB CCIR Moderate on MID rides ~50%
per-frame FER on handshake retries, deep ARQ budgets, and CSS HARQ chase
(payload codewords are LDPC — retransmission LLRs soft-combine).

**Link integration** (6.2): `CAP_CSS_TIER` negotiated in the handshake;
the CALL ladder alternates transports (OFDM calling first, then two CSS
`M0` attempts per OFDM one) and the responder mirrors whatever transport
the CALL arrived on; the committed session extends the AMC ladder to
`M0` (kFadingNA rungs probe-gated); all session control (ACK, MODE, KEY,
DISC, keepalive) rides the session's CSS mode below `M4`.

## VHF-FM Band Profile (Phase 7)

Implemented as a **profile table** (`kVhfFmProfile`), selected by
`[modem] band = vhf-fm` once at startup. The forward spec's 150 Hz
numerology sketch was evaluated and REJECTED in favor of keeping the
proven 320-point / 37.5 Hz grid (see ROADMAP Phase 7 expansion):
throughput comes from carrier COUNT on the wideband path, and every DSP
subsystem is reused unchanged.

**Works today, no profile needed:** Skywave over a VHF/UHF FM rig's voice
audio path (mic/speaker, ~300–3000 Hz) is just the `HF` profile on a
benign channel. The dedicated profile targets the *wideband* data port.

Profile contents:

- **channel**: the flat ~300–6900 Hz audio of a 9600-capable FM rig's
  data port; 48 kHz soundcard I/O, same 12 kHz complex domain shifted to
  a **3.6 kHz center** (bin 96)
- **family**: ONE plan — `VHF`, 176 carriers (audio bins 8–183,
  ~6.6 kHz occupied); every 2-bit wire family index resolves to it
- **CP set**: 0.67 / 2 / 4 ms (SHORT default — VHF multipath is
  microseconds; CP is sync slack, and the tight SHORT buys symbol rate)
- **ladder**: full OFDM ladder plus **`M14` = 256QAM r5/6**, the top
  rung added after the Phase-7 evaluation (M13 alone cannot clear
  25 kbps against per-burst overhead within the 4095-byte PHY cap).
  Measured flat-channel floors (AWGN ≡ fading, `kVhfFloors`):
  −4 … 24.5 (M4–M13), 33.0 (M14). M14 is table-active but unreachable
  on HF (floor sentinels) and above `kInitialModeCap`
- **CSS tier / masking**: OFF (profile gates) — the deep tier is HF
  physics, and the 9-byte wire mask bitmap covers ≤ 72 carriers
- **sync**: same chirp + ZC machinery with **40 ms chirp reps** (4× less
  preamble overhead; the SNR-rich path needs no deep integration),
  ZC block 2/4 symbols, fast-header spread ×1 (fast tier floors ≥
  +6.5 dB)
- **TX**: PAPR ceiling 11 dB (256QAM EVM budget; 8 dB clip noise tipped
  whole frames content-dependently — including every identical
  retransmission)
- **CCA**: the worker's level-based floor+6 dB busy logic (RSSI-style,
  as specified)
- **throughput (M7 gate)**: ≥ 25 kbps net sustained measured end-to-end
  over the flat-channel sim at pinned `M14` — 25.5 kbps including
  preamble/sync/header, MAC container packing, batched ACKs,
  turnarounds, ARQ (`test_vhf`)

## Conformance Targets

*(Revised at M1 — the original single-channel targets predate the
measured fading campaign; see the annex below and ROADMAP 1.13.)*

The per-family **floor tables above are the AWGN reference**: each active
mode must demonstrate, in the Watterson simulator ([TESTING.md](TESTING.md)):

- ≤ 10% FER at its table floor + 1 dB on AWGN
- ≤ ~5% FER at floor + 3 dB on AWGN
- NET throughput within structural allowances of table values
  (codeword padding; NARROW-S16 group pooling)

These are verified by the full `bertool --preset awgn` ladder and the CI
quick profile.

### Fading Conformance Annex (measured, M1)

A one-shot frame shorter than the fade dwell faces **Rayleigh outage**:
FER ≈ P(fade < decode threshold), which costs ~8 dB at 10% outage for
the diversity order these bands achieve (χ²₂–χ²₄). The fading floors are
therefore separate, **measured** values (`kFadingFloors` in
`constants.hpp`), each on the mode's *designated channel*:

- **M4–M7 (robust tier):** CCIR Moderate (1 ms / 0.5 Hz)
- **M8–M13 (fast tier):** CCIR Good (0.5 ms / 0.1 Hz) — AMC only raises
  into these modes on good channels
- **NARROW, all modes:** CCIR Good — a 500 Hz band sees a 2-path channel
  as flat Rayleigh (no frequency diversity); Moderate-Doppler operation
  relies on link-layer ARQ
- **NARROW M13:** no designated fading point below 30 dB; since v1.2.0
  WIDE, MID and ULTRA M13 have one (25.0 / 23.0 / 26.0 dB)

**Per-class tables (v1.2.0).** Every mode is also measured on CCIR
Moderate (`kModerateFloors`) and CCIR Poor (`kPoorFloors`); the AMC uses
the table of the channel class the classifier reports (LINK_ADAPTATION.md)
instead of the v1.1.0 hard caps. On those tables `kFadingNA` means the
mode never reaches 15% FER on that channel inside the measured range
(up to AWGN floor + 28.5 dB) and is ineligible there:

| Family | Moderate M4…M13 (dB) | Poor M4…M13 (dB) |
|---|---|---|
| WIDE | −5.5 −3.0 −0.5 4.0 10.0 13.5 14.0 19.0 22.0 28.5 | −4.0 −2.5 0.5 2.0 11.5 19.0 NA NA NA NA |
| MID | −9.0 −7.5 −5.0 −2.5 4.5 7.5 10.5 14.5 17.0 24.5 | −8.0 −6.5 −4.5 0.0 NA NA NA NA NA NA |
| NARROW | −10.0 −9.5 −6.5 −5.0 3.5 6.5 8.5 13.0 16.0 NA | −10.0 −9.0 −7.0 −5.0 10.5 NA NA NA NA NA |
| ULTRA | −5.0 −2.5 1.0 2.5 8.5 12.5 15.0 19.0 22.5 26.5 | −4.0 −1.5 0.0 3.5 12.5 20.0 NA NA NA NA |

All fading tables use FER ≤ 15% at the floor (the conformance base),
measured with `tools/floor_campaign.sh` (100 trials per 1.5 dB) and
derived by `tools/floor_tables.py`, which also requires the measured
curve to meet the sweep's gate levels at floor+1 and floor+3 without
statistical slack (an outage-limited plateau, like M10 on Poor,
otherwise yields a floor from one lucky point).

Targets at the fading floors: ≤ 10% FER at floor, ≤ ~10% at floor + 3
(outage decays gently, unlike the AWGN waterfall), both with one-sided
binomial slack for the trial count. `bertool` applies the correct floor
table and gates per preset automatically. The design response to the
outage penalty is ARQ + soft-combining (Phase 2) and interleaving depth
review (Phase 3) — do not chase one-shot fading FER with SNR.

Measured campaign CSVs are archived in `docs/conformance/`. Known
provisional entries and open estimator work are listed in ROADMAP 1.13.
