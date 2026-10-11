> **CORRECTION (2026-10, GAP_CLOSURE_PLAN.md R1).** The explanation
> below - "the channel decorrelates *within* the frame", an outage floor
> no SNR can fix - is refuted. A receiver given the TRUE channel from the
> simulator still failed WIDE headers 40-47%; every failed header carried
> a residual CFO error of 4.9-8.9 Hz, whole multiples of the chirp's
> fine-CFO ambiguity. Two-path multipath swapping dominance between up
> and down reps biases the chirp's coarse CFO (~15 Hz on WIDE's sweep at
> 2 ms), and the ZC stage (+-3 Hz fit, 0.7 Hz prior) could not repair a
> wrong branch. With header-verified branch resolution (v1.2.0, step 8.2
> alone) WIDE M4 on Poor reaches 10% FER at 3.3 dB (was: never) and
> header failures over the ladder fall 488 -> 117 of 1000; the remaining
> fading loss is addressed by 8.3-8.6. The tables below are the v1.1.0
> measurements and are kept as the historical record.

# CCIR Poor Conformance — measured 2026-08-17

The Poor channel (1 Hz Doppler spread, 2 ms delay spread) had no
conformance row: the robust-class tracker was *designed* at 1 Hz but its
floors were never measured (ROADMAP 1.13 known issue 3).

Sweeps: `bertool --preset poor` — `skw_poor.csv` (offsets +3…+14 relative
to the fading table, 25 trials/point) and `skw_poor_low.csv` (offsets
−6…+2 for the robust tier).

## Headline finding: Poor is a FAMILY problem, not a mode problem

| Family | Modes that reach ≤10% FER | Best FER seen above that |
|---|---|---|
| `WIDE` | **none, at any SNR** | 28% (M4), 52% (M9) |
| `MID` | M4 (−4.0 dB), M5 (−3.5 dB), M6 | M7+ never below 32% |
| `NARROW` | M4 (−9.0 dB), M5, M6, M7 (all ≤0 dB) | M8+ never below 68% |

WIDE's failures are dominated by `hdrFail` (13/25 at M4) — the channel
decorrelates *within* the frame, so more SNR does not help. This is an
outage floor, not a noise floor, and it is why no amount of promotion or
retransmission rescues a WIDE session on a Poor path.

## Consequences implemented

1. **`ChannelClass::Severe`** (`link/chanclass.hpp`) detects a Poor-class
   path from evidence the receiver already produces — pilot residual
   (Poor 1.77–1.83 vs Moderate 0.38–0.55 vs Good 0.010–0.057), delay
   spread, SNR variance and FER. Any two signals agree ⇒ Severe.
2. **`AmcConfig::severeMaxMode = 7`** — the ladder stops at the robust
   tier on a Severe channel. Promoting past it is guaranteed waste.
3. No `kFadingFloors` row is published for Poor, deliberately: for WIDE
   there is no floor to publish, and a table implying one would be worse
   than the honest cap above.

## Open

The correct response to a Poor path is to **narrow the family**, not
just the mode — MID/NARROW work where WIDE cannot. Family is fixed per
session today, so that requires a reconnect. Mid-session family
renegotiation is the natural follow-on, and the receiver-side family
scan added 2026-08-17 is the enabling half.
