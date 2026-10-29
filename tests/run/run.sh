#!/bin/sh
# Runtime tests: each tests/run/X.svl must print exactly tests/run/X.out, compiled by the given compiler.
# With valgrind installed, every program must also be free of memory errors and leaks.
CC=${1:-./svalinnc}
bad=0
for f in tests/run/*.svl; do
    $CC $f -o /tmp/svalinn_run.bin > /tmp/svalinn_run.err 2>&1 || { echo "COMPILE FAIL $f"; cat /tmp/svalinn_run.err; bad=1; continue; }
    /tmp/svalinn_run.bin > /tmp/svalinn_run.out 2>&1
    cmp -s /tmp/svalinn_run.out ${f%.svl}.out || { echo "OUTPUT MISMATCH $f"; diff ${f%.svl}.out /tmp/svalinn_run.out | head -5; bad=1; }
    if command -v valgrind > /dev/null; then
        valgrind -q --max-stackframe=100000 --leak-check=full --errors-for-leak-kinds=definite --error-exitcode=9 /tmp/svalinn_run.bin > /dev/null 2>&1 || { echo "MEMORY ERROR $f"; bad=1; }
    fi
done
[ $bad = 0 ] && echo "ok runtime tests ($CC)"
