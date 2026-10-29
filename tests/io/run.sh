#!/bin/sh
# main(args: Vec<String>) and std.console: arguments, a prompt, piped standard input, standard error.
CC=${1:-./aegisc}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
$CC tests/io/cat.ag -o "$W/cat" || exit 1
printf 'Ada\n  first \r\nsecond\nno newline' | "$W/cat" one "two words" "" > "$W/out" 2> "$W/err"
printf '' | "$W/cat" >> "$W/out" 2>> "$W/err"
cat "$W/err" >> "$W/out"
if cmp -s "$W/out" tests/io/cat.out; then echo "ok arguments and console input ($CC)"; else echo "IO TEST MISMATCH ($CC)"; diff tests/io/cat.out "$W/out"; exit 1; fi
$CC tests/io/rterr.ag -o "$W/rterr" || exit 1
for k in index div stack string panic; do "$W/rterr" $k > "$W/e_$k" 2>&1; echo "exit $?" >> "$W/e_$k"; cat "$W/e_$k"; done > "$W/rterr.out"
if cmp -s "$W/rterr.out" tests/io/rterr.out; then echo "ok runtime error messages ($CC)"; else echo "RUNTIME ERROR TEST MISMATCH ($CC)"; diff tests/io/rterr.out "$W/rterr.out"; exit 1; fi
