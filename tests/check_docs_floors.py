#!/usr/bin/env python3
"""Assert that WAVEFORM.md's published floor tables match kFloors.

Release gate 7 says the design docs must match the code. For most of the
docs that is a human reading; for the floor tables it can be mechanical,
and it needs to be: three NARROW entries had drifted from
core/constants.hpp without anything noticing, because a stale number in
a markdown table looks exactly like a fresh one.

This checks the AWGN table only (kFloors, M4-M13, all four families).
The CSS deep tier and the fading tables are documented as measurement
records with prose around each entry, so they are not machine-checkable
the same way.

Exit 0 on match, 1 on mismatch, 77 (ctest SKIP) if either file cannot be
parsed - a parse failure means this script needs updating, and it should
not masquerade as a floor regression.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONSTANTS = ROOT / "src" / "core" / "constants.hpp"
WAVEFORM = ROOT / "docs" / "WAVEFORM.md"

# Order of the family rows inside kFloors, and the WAVEFORM.md heading
# that publishes each one.
FAMILIES = [
    ("WIDE", "### WIDE Rates and Floors"),
    ("MID", "### MID Rates and Floors"),
    ("NARROW", "### NARROW Rates and Floors"),
    ("ULTRA", "### ULTRA Rates and Floors (opt-in tier)"),
]
FIRST_OFDM_MODE = 4
LAST_OFDM_MODE = 13


def die_skip(msg):
    print(f"check_docs_floors: SKIP - {msg}")
    sys.exit(77)


def parse_constants():
    """Return {family: {mode: floor}} for the OFDM rungs of kFloors."""
    text = CONSTANTS.read_text()
    m = re.search(
        r"inline constexpr std::array<FloorTable, kNumFamilies> kFloors\s*=\s*\{\{(.*?)\n\}\};",
        text,
        re.S,
    )
    if not m:
        die_skip("could not locate the kFloors initializer in constants.hpp")
    body = re.sub(r"//[^\n]*", "", m.group(1))  # strip the comments
    rows = re.findall(r"\{\{(.*?)\}\}", body, re.S)
    if len(rows) != len(FAMILIES):
        die_skip(f"expected {len(FAMILIES)} kFloors rows, found {len(rows)}")

    out = {}
    for (family, _), row in zip(FAMILIES, rows):
        vals = [v.strip() for v in row.split(",") if v.strip()]
        # 4 CSS rungs (M0-M3), then M4..M14.
        if len(vals) != 15:
            die_skip(f"{family}: expected 15 floor entries, found {len(vals)}")
        out[family] = {
            mode: float(vals[mode])
            for mode in range(FIRST_OFDM_MODE, LAST_OFDM_MODE + 1)
        }
    return out


def parse_waveform():
    """Return {family: {mode: floor}} from the published markdown tables."""
    text = WAVEFORM.read_text()
    out = {}
    for family, heading in FAMILIES:
        start = text.find(heading)
        if start < 0:
            die_skip(f"{family}: heading {heading!r} not found in WAVEFORM.md")
        # The table runs to the next heading of the same or higher level.
        nxt = text.find("\n### ", start + len(heading))
        chunk = text[start : nxt if nxt > 0 else len(text)]
        modes = {}
        for line in chunk.splitlines():
            m = re.match(
                r"\|\s*`M(\d+)`\s*\|[^|]*\|[^|]*\|\s*([^|]+?)\s*\|", line
            )
            if not m:
                continue
            mode = int(m.group(1))
            # Markdown uses U+2212 MINUS SIGN, and a leading + on positives.
            raw = m.group(2).replace("−", "-").replace("dB", "").strip()
            raw = raw.lstrip("+").replace("*", "").strip()
            try:
                modes[mode] = float(raw)
            except ValueError:
                die_skip(f"{family} M{mode}: cannot parse floor {m.group(2)!r}")
        if not modes:
            die_skip(f"{family}: no floor rows parsed under {heading!r}")
        out[family] = modes
    return out


def main():
    code = parse_constants()
    docs = parse_waveform()
    bad = []
    for family, _ in FAMILIES:
        for mode in range(FIRST_OFDM_MODE, LAST_OFDM_MODE + 1):
            want = code[family][mode]
            if mode not in docs[family]:
                bad.append(f"{family} M{mode}: missing from WAVEFORM.md "
                           f"(constants.hpp has {want:+.1f} dB)")
                continue
            got = docs[family][mode]
            if abs(got - want) > 1e-9:
                bad.append(f"{family} M{mode}: WAVEFORM.md says {got:+.1f} dB, "
                           f"constants.hpp has {want:+.1f} dB")

    if bad:
        print("check_docs_floors: FAIL - published floors do not match kFloors")
        for b in bad:
            print(f"  {b}")
        print("\nA floor entry is a measurement. If you changed constants.hpp,")
        print("re-run `bertool --preset awgn` and update WAVEFORM.md to match;")
        print("if you changed WAVEFORM.md, the code is what actually ships.")
        return 1

    n = len(FAMILIES) * (LAST_OFDM_MODE - FIRST_OFDM_MODE + 1)
    print(f"check_docs_floors: PASS - {n} published floors match kFloors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
