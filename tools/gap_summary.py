#!/usr/bin/env python3
"""Summarise tools/gap_sweeps.sh output (docs/GAP_CLOSURE_PLAN.md).

    gap_summary.py DIR [BASE_DIR]

Prints, per suite: SNR (3 kHz ref) at 10% FER for each ladder row,
per-point FER for awgn/cfo/drift, and - when BASE_DIR is given - the
difference against it (worse AWGN points flagged when > 4 pp).
"""
import csv
import glob
import os
import sys


def rows(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def load(d):
    out = {}
    for p in sorted(glob.glob(os.path.join(d, "*.csv"))):
        out[os.path.basename(p)] = rows(p)
    return out


def snr_at(points, target=0.10):
    """Linear-interpolated SNR where FER first falls to <= target."""
    pts = sorted((float(r["snr3k"]), float(r["fer"])) for r in points)
    prev = None
    for s, f in pts:
        if f <= target:
            if prev is None:
                return f"<={s:.1f}"
            s0, f0 = prev
            return f"{s0 + (f0 - target) / (f0 - f) * (s - s0):.1f}"
        prev = (s, f)
    return f"none(min {min(f for _, f in pts) * 100:.0f}%)" if pts else "-"


def ladder(data):
    res = {}
    for name, rs in data.items():
        if not name.startswith("lad_"):
            continue
        preset = name.split("_")[1]
        by = {}
        for r in rs:
            by.setdefault((r["family"], r["mode"]), []).append(r)
        for (fam, mode), pts in by.items():
            hdr = sum(int(r["hdrFail"]) for r in pts)
            res[(preset, fam, int(mode))] = (snr_at(pts), hdr)
    return res


def points(data, prefix):
    res = {}
    for name, rs in data.items():
        if not name.startswith(prefix):
            continue
        for r in rs:
            key = (name[:-4], r["family"], int(r["mode"]), r["snr3k"])
            res[key] = float(r["fer"])
    return res


def main():
    d = sys.argv[1]
    base = load(sys.argv[2]) if len(sys.argv) > 2 else None
    cur = load(d)

    lad = ladder(cur)
    if lad:
        blad = ladder(base) if base else {}
        print("== SNR3k at 10% FER (hdrFail total over the ladder)")
        for k in sorted(lad):
            b = blad.get(k)
            bs = f"   base {b[0]:>14} (hdr {b[1]})" if b else ""
            print(f"{k[0]:9} {k[1]:7} M{k[2]:<3} {lad[k][0]:>14} (hdr {lad[k][1]}){bs}")

    for prefix, title in (("awgn_", "AWGN"), ("cfo_", "Static CFO @ floor+6"),
                          ("drift_", "Drift @ floor+6")):
        p = points(cur, prefix)
        if not p:
            continue
        bp = points(base, prefix) if base else {}
        print(f"== {title}: FER %")
        worse = 0
        for k in sorted(p):
            b = bp.get(k)
            flag = ""
            if b is not None:
                if prefix == "awgn_" and p[k] - b > 0.04:
                    flag = "  WORSE"
                    worse += 1
                flag = f"   base {b * 100:5.1f}{flag}"
            print(f"{k[0]:24} {k[1]:7} M{k[2]:<3} {k[3]:>6} {p[k] * 100:5.1f}{flag}")
        if prefix == "awgn_" and bp:
            print(f"AWGN points worse than base by > 4 pp: {worse}")


if __name__ == "__main__":
    main()
