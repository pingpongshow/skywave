# ULTRA robust-tier deficit — investigated 2026-08-17

ROADMAP 1.13 known issue 2 recorded: *"ULTRA robust tier ~3 dB worse than
WIDE (M6/M7 floors provisional at 16.0/12.5): outermost carriers ride the
TX band-FIR transition (40 Hz margin); try a wider margin for ULTRA."*

## The stated cause is refuted

`familyTxBandTaps()` gained `SKW_TXBAND_MARGIN_HZ` so the guard between
the outermost carrier and the Kaiser passband edge could be swept. ULTRA
M5/M6 on CCIR Moderate, 40 trials/point:

| margin | M5 sweep | M6 sweep |
|---|---|---|
| 40 Hz (shipped) | 0.5:30% 2.5:18% 4.5:12% 6.5:12% | 10:5% 12:15% 14:18% 16:10% |
| 120 Hz | *identical* | *identical* |
| 200 Hz | *identical* | 14:15% (1 pt, within noise) |

The filter margin is **not** the mechanism. Pilot extrapolation at the
band edge is ruled out too: `isPilotCarrier()` spaces pilots evenly across
every family's full carrier set, so ULTRA's outermost carriers are
interpolated exactly like WIDE's.

About 0.5 dB of the gap is simple noise bandwidth — 72 carriers vs 64 at
equal total power is 10·log10(72/64). The remaining ~3 dB is **still
unexplained**; the two recorded hypotheses are now both eliminated, which
is progress of a kind.

## Third hypothesis also refuted: the RX front end

`rxStage2Taps()` puts its passband edge at `half + 50` = 1400 Hz, and
ULTRA's outermost carrier sits at exactly `half` = 1350 — only 50 Hz
inside a Kaiser knee, where WIDE's is 200 Hz inside. A plausible
family-specific penalty, and **wrong**.

Note the method trap: bertool runs entirely at 12 kHz baseband and never
instantiates the 48 k↔12 k converters, so sweeping `SKW_RXBAND_MARGIN_HZ`
through bertool changed nothing *because it measured nothing*. Retested
with a probe that pushes each frame through the real `Upconverter` /
`Downconverter` pair, M6 on CCIR Moderate at 8 dB, 40 trials:

| family | baseband only | through converters |
|---|---:|---:|
| `WIDE` | 38/40 | 38/40 |
| `ULTRA` | 33/40 | 34/40 |

The converter chain costs **nothing** for either family. It also shows
the gap is present at baseband, i.e. before any of the filters that were
suspected.

## Where that leaves it

Eliminated by measurement: TX band-FIR margin, pilot extrapolation, RX
front-end margin. Explained: ~0.5 dB from 72 vs 64 carriers sharing the
same total power. Remaining: ~3 dB with no filter to blame.

The leading candidate is now the **channel estimator**, not the signal
path: ULTRA spans 2.7 kHz against a ~1 kHz coherence bandwidth at 1 ms
delay spread, so the tap-domain Kalman model has proportionally more
phase rotation to fit across the band. That would make this deficit and
the recorded −8 dB residual tracking floor (ROADMAP 1.13 known issue 1)
the same underlying problem, which is worth treating as one investigation
rather than two.

## What was actually wrong: the table

Re-measured with 60 trials on each mode's *designated* channel (M4–M7 on
Moderate, M8–M9 on Good), archived as `skw_ultra_remeasure_{mod,good}.csv`:

| mode | old floor | measured | note |
|---|---:|---:|---|
| M4 | 4.0 | 4.0 | unchanged |
| M5 | 6.5 | **4.5** | 2 dB pessimistic |
| M6 | 16.0 | **10.0** | 6 dB pessimistic |
| M7 | 12.5 | 12.5 | knee; no clean 10% point (outage tail) |
| M8 | 8.5 | **10.5** | 2 dB optimistic |
| M9 | 13.5 | **15.5** | 2 dB optimistic |

The old M6/M7 pair read 16.0/12.5 — **decreasing as the mode gets
faster**, which is physically backwards and was the visible symptom of a
bad measurement rather than a bad waveform. Correcting it hands ULTRA
back ~6 dB of usable ladder at M6, and raising M8/M9 stops the AMC
promoting into modes that do not hold up.
