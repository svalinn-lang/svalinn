#!/bin/bash
# Differential fuzzing: random programs must behave identically with and without -O.
# usage: tests/fuzz/run.sh [count] [first-seed] [compiler]
N=${1:-200}; S=${2:-1}; CC=${3:-./aegisc}; bad=0; ok=0; skip=0
T=$(mktemp -d /tmp/aegis_fuzz.XXXXXX)
for seed in $(seq $S $((S + N - 1))); do
    python3 tests/fuzz/gen.py $seed > $T/fz.ag
    $CC $T/fz.ag -o $T/fz0 > $T/fz.err 2>&1 || { skip=$((skip+1)); continue; }   # generator can produce ill-typed programs: skip
    $CC -O $T/fz.ag -o $T/fz1 > $T/fz.err 2>&1 || { echo "seed $seed: -O failed to compile"; cat $T/fz.err; bad=$((bad+1)); continue; }
    { timeout 10 $T/fz0 > $T/fz0.out 2>&1; r0=$?; } 2>/dev/null
    { timeout 10 $T/fz1 > $T/fz1.out 2>&1; r1=$?; } 2>/dev/null
    if [ $r0 != $r1 ] || ! cmp -s $T/fz0.out $T/fz1.out; then echo "seed $seed: MISMATCH (rc $r0/$r1)"; bad=$((bad+1)); cp $T/fz.ag /tmp/fz_fail_$seed.ag; else ok=$((ok+1)); fi
done
rm -rf $T
echo "fuzz: $ok ok, $bad failures, $skip skipped"
