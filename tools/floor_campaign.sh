#!/bin/bash
# Floor-table measurement campaign (GAP_CLOSURE_PLAN.md 8.9). One bertool
# job per (channel, family, mode), all offsets relative to the AWGN floor
# so every mode is swept on every channel - including those marked
# kFadingNA. Writes OUT/<preset>_<family>_M<mode>.{csv,txt}; the .txt keeps
# bertool's est/evm/pilot columns for the SNR-calibration and classifier
# work. Derive tables with tools/floor_tables.py.
#
#   tools/floor_campaign.sh BERTOOL OUT
#   env: J (parallel jobs, default 11), TA (AWGN trials, 200),
#        TF (fading trials, 100)
set -u
B=$1; OUT=$2
J=${J:-11}; TA=${TA:-200}; TF=${TF:-100}
mkdir -p "$OUT"
AWGN_OFF="-3,-2.5,-2,-1.5,-1,-0.5,0,0.5,1,1.5,2,2.5,3"
FADE_OFF="0,1.5,3,4.5,6,7.5,9,10.5,12,13.5,15,16.5,18,19.5,21,22.5,24,25.5,27,28.5"
jobs() {
  for fam in wide mid narrow ultra; do
    for m in 4 5 6 7 8 9 10 11 12 13; do
      echo "$B --preset awgn --families $fam --modes $m --offsets $AWGN_OFF --trials $TA --csv $OUT/awgn_${fam}_M$m.csv > $OUT/awgn_${fam}_M$m.txt"
      for p in good moderate poor; do
        echo "$B --preset $p --base awgn --families $fam --modes $m --offsets $FADE_OFF --trials $TF --csv $OUT/${p}_${fam}_M$m.csv > $OUT/${p}_${fam}_M$m.txt"
      done
    done
  done
}
# Longest jobs (robust modes, narrow families) first keeps the pool busy.
jobs | awk '{w = ($0 ~ /--modes [4-7] /) ? 0 : 1; print w "\t" $0}' |
  sort -s -k1,1n | cut -f2- |
  xargs -P "$J" -S 4096 -I{} sh -c '{} 2>/dev/null'
echo "campaign done: $OUT"
