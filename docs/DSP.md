# Skywave Signal Processing Design

## Scope

The sample-domain design: RX and TX chains, synchronization algorithms,
equalization, AGC, interference tools, TX levels, and clear-channel
assessment. Waveform parameters are in [WAVEFORM.md](WAVEFORM.md).

## Sample Domain Overview

- external interface: `48 kHz` mono float32 (PortAudio), real passband
- internal PHY domain: complex baseband at `12 kHz`, produced by a −1500 Hz
  complex mixer followed by 2-stage polyphase decimation (×2 then ×2); the
  sharp stage-2 lowpass (at 24 kHz: pass ±1400 Hz, stop ±1650 Hz, 70 dB)
  simultaneously rejects the real input's negative-frequency image (whose
  modem content begins at −1687 Hz for ULTRA) and anti-aliases the final
  decimation. TX runs the mirror chain (2-stage interpolation with short
  image filters, mix +1500 Hz, take real part)
- rationale: 4× less work in every downstream block (FFT becomes 320-point
  at 12 kHz — implemented as the 1280-point equivalent grid: FFT size 320,
  spacing 37.5 Hz), and the complex domain simplifies CFO handling
- all DSP blocks are fixed-size, allocation-free, and process in hops of one
  OFDM symbol

## RX Chain

```text
audio in (48k real)
  -> DC block -> input gain trim
  -> AGC (slow, headroom-targeting)
  -> mix(-1500 Hz) + 2-stage decimate (x2,x2)  ->  12k complex baseband
  -> impulse blanker            [optional, config]
  -> adaptive multi-notch       [optional, config]
  -> sync engine (chirp detector | tracking)
  -> CFO rotator (from sync/tracking loop)
  -> symbol windowed FFT (320)
  -> pilot processor -> channel estimator -> MMSE equalizer
  -> despreader (MRC over S copies)
  -> soft demapper (LLRs, per-carrier noise weighting)
  -> deinterleave -> LDPC decode -> dewhiten -> CRC check
  -> link layer
```

### AGC

- target: −18 dBFS RMS with 12 dB headroom
- attack 10 ms / release 500 ms; freezes during detected frames so gain
  pumping never modulates an equalized frame
- reports input level to telemetry (drives the GUI level meter and the
  `AUDIOLEVEL` control query)

### Sync Engine

States: `SEARCH → COARSE → FINE → TRACK → (FLYWHEEL) → SEARCH`

- `SEARCH`: continuous chirp matched filter at 12 kHz on a decimated
  correlation grid; two correlators per family (up-chirp, down-chirp) give
  the signature bit; families are searched in parallel (3 families × 2
  polarities = 6 cheap correlators). An all-inverted signature pattern
  (e.g., down–down where only up–up is valid) indicates **sideband
  inversion** ([WAVEFORM.md](WAVEFORM.md)): the engine conjugates the
  baseband, re-runs the match, and proceeds normally with an
  `inverted-spectrum` telemetry flag set
- `COARSE`: correlation peak → timing ±1 ms; CFO from slope diversity plus
  inter-rep phase → <1 Hz. Capture range: ±5 Hz for connected preambles;
  **±50 Hz for calling preambles** — real SSB stations mistune by tens of
  Hz. An LFM frequency offset masquerades as a timing shift (Δt = Δf·T/B);
  up-reps and down-reps shift in opposite directions, so the peak-time
  difference between slopes yields coarse CFO and the mean yields true
  timing (this is why the robust and calling signatures both contain both
  slopes); inter-rep correlation phase refines within its ±3.1 Hz ambiguity
- `FINE`: ZC autocorrelation (Schmidl–Cox metric) refines timing to
  ±2 samples @ 12 k and CFO residual < 0.5 Hz. Fast header class: one ZC
  pair, second symbol seeds the channel estimate. Robust class: two ZC
  pairs; the two independent channel estimates are averaged (+3 dB) —
  single-shot estimation is too noisy at the robust floor and directly
  erodes MRC despreading gain
- `TRACK`: per-symbol pilot phase innovations drive a 2nd-order PLL for
  residual CFO/sampling-clock drift; timing drift corrected by ±1 sample
  slips at symbol boundaries
- `FLYWHEEL`: on lost pilots mid-frame, hold last CFO/timing for up to 8
  symbols before declaring frame loss (rides through short fades/impulses)

### Channel Estimation and Equalization

- initial estimate: ZC symbol → per-carrier complex gain
- update: scattered pilots (1/8 per symbol, full sweep every 8 symbols)
  update the estimate with 2D interpolation — linear in frequency between
  pilot carriers, 1st-order IIR in time. The IIR constant is
  **mode-dependent**: α = 0.4 (≈ 120 ms memory, tracks ~1 Hz fading) for
  unspread modes; α = 0.15 with full-sweep averaging for spread modes
  (`M4–M7`), where per-pilot SNR is too low for fast tracking and the
  longer integration trades fading agility for estimate quality — the right
  trade at the robust floor
- equalizer: per-carrier MMSE — `ŝ = conj(H)·y / (|H|² + 1/SNR)` — with the
  noise term from the per-carrier noise tracker; MMSE (not zero-forcing)
  matters in deep selective notches
- outputs per-carrier SNR to: soft demapper weighting, selectivity index
  (AMC), carrier-mask candidate weights
- **delay-spread estimate**: the IFFT of the channel frequency response
  yields the power-delay profile; its 90%-energy span is the delay-spread
  estimate that drives `CPMODE AUTO` — CP steps up when the spread exceeds
  80% of the current CP and steps down (via `MODE_CHANGE`) when it stays
  under 40% for 30 s. The link layer peak-holds the estimate (1%/frame
  decay) from **decoded** frames only — on a failed frame the tap state
  is whatever the noise left it with, and it read 4.4–4.6 ms on a 1 ms
  CCIR-Moderate channel, which the peak-hold then kept as a LONG-CP
  verdict — and acts on a verdict once 25 received frames agree, **or** 8
  agree and the verdict has held for a minute — the frame count alone was
  slowest exactly when it mattered (ISI forces the robust tier, where an
  exchange takes ~20 s; measured: streak 21 after 670 s with the transfer
  stalled at 0 bytes)

### Despreading

For spread factor `S`: the `S` equalized copies are combined by maximum-ratio
combining using per-copy post-equalization SNR weights, then passed to the
demapper as one symbol with summed weight. Copy phases (known PRN) are
derotated before combining.

### Soft Demapper

- Gray-mapped QPSK/16QAM/64QAM max-log-MAP LLRs
- LLRs scaled by per-carrier equalizer confidence, quantized to 5-bit for
  the LDPC decoder
- constellation samples exported to telemetry ring for the GUI plot

## TX Chain

```text
link frame bytes
  -> CRC-32 -> whiten -> LDPC encode -> interleave -> map -> spread
  -> pilot insert -> IFFT (320) -> CP prepend
  -> raised-cosine edge window (2 ms overlap between symbols)
  -> preamble/ZC prepend (frame builder)
  -> soft clipper (PAPR ceiling 8 dB) -> occupied-band FIR
  -> TX level normalizer
  -> mix(+1500 Hz) + interpolate 1:4 -> real 48k
  -> final headroom scale (0.5) -> audio out
```

### TX Frequency Pre-Compensation

The TX mixer applies the link-alignment shift from
[PROTOCOL.md](PROTOCOL.md) (half the peer-reported CFO — split correction —
slewed ≤ 2 Hz per step, clamped ±30 Hz per side) so both signals converge
to center without either station's audio spectrum leaving its own filter's
edge guards. The shift is a phase-ramp on the complex baseband before the
final mix — zero extra cost.

### TX Watchdog

A hard airtime cap protects the transmitter and the band against stuck
logic: any single continuous transmission exceeding `tx_watchdog_s`
(default 120 s, config `[modem]`) forces audio mute + PTT release + an
`EVT:TX_WATCHDOG <seconds>` notification. The cap must exceed the longest
legitimate burst (a `NARROW` robust handshake frame ~15 s, a deep-tier CSS
handshake 35–55 s, a long voice talkspurt) — it is a safety net, not a
scheduler.

Implemented in the modem worker, which owns keying: the timer starts when
PTT is asserted and is checked every audio chunk, so it fires whether the
stall is in the engine, the converters, or the audio device. On trip it
releases the key, discards the staged TX audio, and cancels any running
`TXTEST`.

### TX Level Normalization

Consistent loudness across frame types (chirps are near-constant-envelope,
OFDM is not):

- target RMS: 0.22 (pre-headroom), measured over the whole burst
- scale clamp: 0.3–1.15; peak clamp 1.9 pre-headroom (≈ 0.95 after final 0.5)
- chirp/ZC segments are attenuated to the OFDM RMS budget rather than OFDM
  being boosted

## Interference Tools

Config-gated, default off, both purely RX-side
(see [CONFIGURATION.md](CONFIGURATION.md)):

### Impulse Blanker

- detector: sample magnitude > 6 × sliding MAD estimate over 20 ms window
- action: replace impulse span (≤ 1 ms) with linear interpolation; longer
  events are left alone (blanking long spans hurts more than it helps)
- effective against lightning static and ignition-type QRN

### Adaptive Multi-Notch

- up to 6 IIR biquad notches, Q 25–70, tracked from a 0.5 s averaged
  spectrum; tones ≥ 10 dB above local median for ≥ 1 s get a notch
- protected zones: active carriers of the current session's family are
  depth-limited (max 6 dB cut) so the notch bank cannot eat the modem's own
  band; out-of-family tones may be cut to 40 dB
- update cadence 100 ms; notch changes are slewed to avoid clicks

Carrier masking ([LINK_ADAPTATION.md](LINK_ADAPTATION.md)) is the preferred
in-band defense; the notch bank mainly protects AGC and sync from strong
out-of-band/in-passband carriers.

## Channel Activity and CCA

### Busy Detector

Fused indication, exported as `BUSY`:

- RMS gate: band-limited (300–2700 Hz) short-time RMS > threshold
  (default −38 dBFS) with 50 ms window
- spectral gate: 0.5 s FFT average flags coherent occupancy (peak/median
  > 8 dB within the family band)
- either gate trips busy; 1 s hold after both clear

### Noise Floor Estimator

Per-family-band median spectrum tracker (10 s memory) reporting:

- noise floor dBFS, recent peak dBFS, occupied-bandwidth estimate
- surfaces via `NOISEFLOOR` on the control port and telemetry

### Pre-TX CCA Gate

Implemented INSIDE the link engine (`LinkEngine::setChannelBusy` +
`ccaDataGated`): the worker feeds the fused energy detector's verdict
each chunk, and the engine holds DATA-class emissions (new segments AND
retransmissions, which otherwise burn ACK timers parked in the TX
queue) while the channel is busy.

- DATA bursts blocked while busy, up to a 5 s fairness cap, then one
  burst proceeds anyway (HF politeness, not CSMA perfection)
- on channel-clear after a defer: 50-300 ms randomized backoff (seeded
  per callsign) so the two ends of a saturated bidirectional link
  interleave instead of keying together the instant the other stops -
  without this, two-way bulk collapses into a mutual retransmission
  storm (zero delivered payload at +25 dB; found by the ST2 soak)
- bypasses: everything that is not `DATA` - ACK/handshake/mode/mask/
  probe/disconnect control, voice frames, broadcast/beacon/CW - short,
  reactive, naturally gap-timed; a gated DATA frame never blocks a
  control frame queued behind it (no priority inversion)
- engine-level tests and bertool never feed the busy flag: the gate is
  inert there by construction

## CW ID Renderer

- straight-key Morse at config WPM (default 20), 700 Hz tone, −6 dB
  relative to modem RMS budget, shaped 5 ms raised-cosine edges
- queued as a lowest-priority TX burst; never interrupts a frame

## Numerical Conventions

- internal sample type: `float` (32-bit); accumulators `double` where drift
  matters (NCOs, energy estimators)
- FFT: KissFFT (vendored, mixed-radix 320 = 2^6·5 at 12 kHz domain)
- all buffers symbol-aligned; symbol hop = 320 + CP/4 samples at 12 kHz
  (CP samples divide by 4 exactly: 96/192/288 → 24/48/72)
- fixed seeds for all PRN sequences (whitener, spreading phases, pilot
  phases) — defined constants in one header, shared TX/RX
