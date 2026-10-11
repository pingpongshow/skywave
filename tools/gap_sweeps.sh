#!/bin/bash
# Phase 8 measurement suites (docs/GAP_CLOSURE_PLAN.md). Runs the
# standard bertool command lines in parallel against one build and writes
# bertool CSVs into OUT; summarise with tools/gap_summary.py.
#
#   tools/gap_sweeps.sh BERTOOL OUT [suite ...]
#     suites: awgn ladder cfo drift   (default: all)
#   env: T (ladder/awgn trials, default 100), TC (cfo/drift trials,
#        default 30), J (parallel jobs, default 10), SKW_* knobs pass
#        through to bertool.
set -u
B=$1; OUT=$2; shift 2
SUITES=${*:-awgn ladder cfo drift}
T=${T:-100}; TC=${TC:-30}; J=${J:-10}
mkdir -p "$OUT"
jobs() {
  for s in $SUITES; do
    case $s in
      awgn)
        for f in wide mid narrow; do
          echo "$B --preset awgn --families $f --offsets 0,1,3 --trials $T --csv $OUT/awgn_$f.csv"
        done ;;
      ladder)
        L="-6,-4,-2,0,2,4,6,9,12,15"
        echo "$B --preset moderate --families wide --modes 4,6,8,11 --offsets $L --trials $T --csv $OUT/lad_moderate_wide.csv"
        echo "$B --preset poor --families wide --modes 4,6,8 --offsets $L --trials $T --csv $OUT/lad_poor_wide.csv"
        echo "$B --preset good --families wide --modes 8,11 --offsets $L --trials $T --csv $OUT/lad_good_wide.csv"
        for f in mid narrow; do
          echo "$B --preset moderate --families $f --modes 4,6,8 --offsets $L --trials $T --csv $OUT/lad_moderate_$f.csv"
          echo "$B --preset poor --families $f --modes 4,6 --offsets $L --trials $T --csv $OUT/lad_poor_$f.csv"
        done ;;
      cfo)
        for c in 0 3.5 -3.5 10 -10 20 -20 30 -30 50 -50; do
          for f in wide mid narrow; do
            echo "$B --preset awgn --families $f --modes 4,7,8,11 --offsets 6 --trials $TC --cfo $c --csv $OUT/cfo_${f}_$c.csv"
          done
        done ;;
      drift)
        for d in 0.1 0.5 1 2; do
          for f in wide mid narrow; do
            echo "$B --preset awgn --families $f --modes 4,7,8,11 --offsets 6 --trials $TC --drift $d --csv $OUT/drift_awgn_${f}_$d.csv"
          done
        done
        for f in wide mid narrow; do
          echo "$B --preset moderate --families $f --modes 4,7,8 --offsets 6 --trials $TC --drift 0.5 --csv $OUT/drift_moderate_${f}_0.5.csv"
        done ;;
    esac
  done
}
jobs | xargs -P "$J" -I{} sh -c '{} > /dev/null 2>&1'
echo "done: $OUT"
