#!/bin/sh
# aegisc --test (test runner) and @deprecated warnings
CC=${1:-./aegisc}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
$CC --test tests/testmode/calc.ag -o "$W/t" > "$W/out" 2>&1 || exit 1
"$W/t" >> "$W/out" 2>&1; echo "exit $?" >> "$W/out"
$CC tests/testmode/deprecated.ag -o "$W/d" >> "$W/out" 2>&1 || exit 1
"$W/d" >> "$W/out" 2>&1
if cmp -s "$W/out" tests/testmode/expected.out; then echo "ok test runner and warnings ($CC)"; else echo "TEST MODE MISMATCH ($CC)"; diff tests/testmode/expected.out "$W/out"; exit 1; fi
