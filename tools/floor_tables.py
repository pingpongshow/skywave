#!/usr/bin/env python3
"""Derive floor tables from tools/floor_campaign.sh output (8.9).

    floor_tables.py DIR

Definitions (unchanged from the v1.x tables):
  AWGN floor     = SNR(3 kHz ref) where FER first falls to 10%, minus
                   1 dB (conformance tests floor+1 <= 10%), rounded UP
                   to the 0.5 dB grid.
  fading floor   = SNR where FER first falls to 15%, rounded UP to 0.5.
  designated     = Moderate for the robust tier (M4-M7), Good for the
                   fast tier (M8-M13); NARROW uses Good for every mode.
  NA (99)        = never reaches the level inside the measured range.
Every floor is then checked against bertool's own conformance gates on
the campaign curve (interpolated at floor+1 and floor+3) and moved up the 0.5 dB
grid until both pass; a mode that never passes is NA.

Also prints, per family, the estimator bias (bertool `est` minus the set
point) over AWGN points within 0..3 dB above the 10% point - the
kOfdmSnrCalDb input - and per channel the median pilot excess over
decoded frames 3..10 dB above each mode's fading floor - the classifier
threshold input.
"""
import csv
import math
import re
import statistics
import sys
from pathlib import Path

FAMS = ["wide", "mid", "narrow", "ultra"]
MODES = list(range(4, 14))
NA = 99.0


def load(path):
    if not path.exists():
        return []
    with open(path) as f:
        return sorted(((float(r["snr3k"]), float(r["fer"])) for r in csv.DictReader(f)))


def crossing(pts, level, slack_sigma=0.0, trials=100):
    """Interpolated SNR of the level crossing after which the curve STAYS
    down: every later point <= level + slack_sigma * binomial sigma (the
    conformance gate's own slack). Without the sustain rule an
    outage-limited plateau (Poor M10: 12-25% at every SNR) produced a
    floor from one lucky dip."""
    lim = level + slack_sigma * math.sqrt(level * (1.0 - level) / trials)
    start = None
    for i in range(len(pts)):
        if all(f <= lim for _, f in pts[i:]) and pts[i][1] <= level:
            start = i
            break
    if start is None:
        return None
    s, fer = pts[start]
    if start == 0:
        return s  # already below at the first point
    s0, f0 = pts[start - 1]
    return s0 + (f0 - level) / (f0 - fer) * (s - s0)


def up_half(x):
    return math.ceil(x * 2.0 - 1e-9) / 2.0


def fer_at(pts, snr):
    """Linearly interpolated FER at snr (None outside the measured span)."""
    for (s0, f0), (s1, f1) in zip(pts, pts[1:]):
        if s0 <= snr <= s1:
            return f0 if s1 == s0 else f0 + (f1 - f0) * (snr - s0) / (s1 - s0)
    return None


def gate_ok(pts, floor, awgn, trials):
    """bertool's conformance levels at the offsets its sweep runs: the
    curve's FER at floor+1 <= p0 and at floor+3 <= p3."""
    p0 = 0.10 if awgn else 0.15
    p3 = (0.04 if awgn else 0.10) + 1.0 / trials
    # NO slack here: the campaign derives the floor, an independent
    # sweep with fresh seeds verifies it, and the verification keeps the
    # binomial slack. Deriving with the slack let an outage plateau (Poor
    # WIDE M10, 13-23%) pass on one seed and fail on the next.
    g = lambda p: p
    f1, f3 = fer_at(pts, floor + 1.0), fer_at(pts, floor + 3.0)
    return f1 is not None and f3 is not None and f1 <= g(p0) and f3 <= g(p3)


def gated_floor(pts, first, awgn, trials):
    """Lowest 0.5 dB grid value >= first that passes both gates."""
    if first is None:
        return None
    top = max(s for s, _ in pts) - 3.0
    f = first
    while f <= top:
        if gate_ok(pts, f, awgn, trials):
            return f
        f += 0.5
    return None


def txt_rows(path):
    """(snr, fer, est, pilot) per bertool table row."""
    out = []
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        m = re.match(r"\s*\w+\s+M\d+\s+\S+\s+(\S+)\s*\|.*?([\d.]+)%\s*\|.*?est\s+(\S+)\s+evm\s+(\S+)\s+pilot\s+(\S+)", line)
        if m:
            out.append(tuple(float(m.group(i)) for i in (1, 2, 3, 5)))
    return out


def main():
    d = Path(sys.argv[1])
    tables = {}
    for preset in ["awgn", "good", "moderate", "poor"]:
        for fam in FAMS:
            row = []
            for m in MODES:
                pts = load(d / f"{preset}_{fam}_M{m}.csv")
                if not pts:
                    row.append(None)
                    continue
                if preset == "awgn":
                    x = crossing(pts, 0.10, 1.3, 200)
                    fl = gated_floor(pts, None if x is None else up_half(x - 1.0), True, 200)
                else:
                    x = crossing(pts, 0.15, 2.0, 100)
                    fl = gated_floor(pts, None if x is None else up_half(x), False, 100)
                row.append(NA if fl is None else fl)
            tables[(preset, fam)] = row
    for fam in FAMS:
        g, mo = tables[("good", fam)], tables[("moderate", fam)]
        tables[("designated", fam)] = [mo[i] if m <= 7 and fam != "narrow" else g[i]
                                       for i, m in enumerate(MODES)]

    def fmt(v):
        return "  -  " if v is None else ("  NA " if v >= NA else f"{v:5.1f}")

    for name in ["awgn", "designated", "good", "moderate", "poor"]:
        print(f"== {name} floors (M4..M13)")
        for fam in FAMS:
            row = tables[(name, fam)]
            mono = all(a is None or b is None or a >= NA or b >= NA or b >= a
                       for a, b in zip(row, row[1:]))
            print(f"  {fam:7}" + "".join(fmt(v) for v in row) + ("" if mono else "   NON-MONOTONIC"))

    print("== estimator bias (est - set point), AWGN, 0..3 dB above the 10% point")
    for fam in FAMS:
        biases = []
        for m in MODES[:7]:  # M4-M10, the floor region the AMC decides in
            pts = load(d / f"awgn_{fam}_M{m}.csv")
            x = crossing(pts, 0.10) if pts else None
            if x is None:
                continue
            for snr, fer, est, _ in txt_rows(d / f"awgn_{fam}_M{m}.txt"):
                if x <= snr <= x + 3.0 and est > -90:
                    biases.append(est - snr)
        if biases:
            print(f"  {fam:7} median {statistics.median(biases):+.2f}  range {min(biases):+.2f}..{max(biases):+.2f}  (n={len(biases)})")

    print("== pilot excess median over modes, 3..10 dB above each mode's floor on that channel")
    for preset in ["awgn", "good", "moderate", "poor"]:
        for fam in FAMS:
            vals = []
            for i, m in enumerate(MODES):
                fl = tables[(preset, fam)][i]
                if fl is None or fl >= NA:
                    continue
                for snr, fer, est, pilot in txt_rows(d / f"{preset}_{fam}_M{m}.txt"):
                    if fl + 3.0 <= snr <= fl + 10.0 and pilot >= 0:
                        vals.append(pilot)
            if vals:
                q = statistics.quantiles(vals, n=10) if len(vals) >= 10 else [min(vals)] * 9
                print(f"  {preset:9}{fam:7} median {statistics.median(vals):.3f}  p10 {q[0]:.3f}  p90 {q[-1]:.3f}  (n={len(vals)})")


if __name__ == "__main__":
    main()
