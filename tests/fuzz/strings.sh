#!/bin/bash
# String fuzzing against a Python oracle (tests/fuzz/strgen.py); with valgrind, every 10th program is also
# checked for memory errors and leaks.  usage: tests/fuzz/strings.sh [count] [first-seed] [compiler]
N=${1:-100}; S=${2:-1}; CC=${3:-./aegisc}; bad=0; ok=0
T=$(mktemp -d /tmp/aegis_sfuzz.XXXXXX)
for seed in $(seq $S $((S + N - 1))); do
    python3 tests/fuzz/strgen.py $seed $T/exp.txt > $T/s.ag
    for o in "" -O; do
        $CC $o $T/s.ag -o $T/s > $T/s.err 2>&1 || { echo "seed $seed: compile failed ($o)"; head -3 $T/s.err; bad=$((bad+1)); cp $T/s.ag /tmp/sfz_fail_$seed.ag; continue; }
        timeout 10 $T/s > $T/out.txt 2>&1
        if ! cmp -s $T/out.txt $T/exp.txt; then echo "seed $seed: WRONG OUTPUT ($o)"; bad=$((bad+1)); cp $T/s.ag /tmp/sfz_fail_$seed.ag; continue; fi
        if [ $((seed % 10)) = 0 ] && command -v valgrind > /dev/null; then
            valgrind -q --leak-check=full --errors-for-leak-kinds=definite --error-exitcode=9 $T/s > /dev/null 2>&1 || { echo "seed $seed: MEMORY ERROR ($o)"; bad=$((bad+1)); cp $T/s.ag /tmp/sfz_fail_$seed.ag; continue; }
        fi
        ok=$((ok+1))
    done
done
rm -rf $T
echo "string fuzz: $ok ok, $bad failures"
[ $bad = 0 ]
