#!/bin/bash
# Runtime test of `protect Process;`: the same victim, with and without protection, attacked by an
# unprivileged process of the same user. Needs root (to drop privileges with setpriv).
cd "$(dirname "$0")/../.."
if [ "$(id -u)" != 0 ] || ! command -v setpriv >/dev/null; then echo "skip security test (needs root + setpriv)"; exit 0; fi
W=$(mktemp -d); chmod 755 "$W"; trap 'rm -rf "$W"' EXIT
sed 's#//PROTECT##' tests/security/victim.svl > "$W/victim_open.svl"
sed 's#//PROTECT#protect Process;#' tests/security/victim.svl > "$W/victim_prot.svl"
for p in victim_open victim_prot; do ./svalinnc "$W/$p.svl" -o "$W/$p" || exit 1; done
./svalinnc tests/security/attacker.svl -o "$W/attacker" || exit 1
chmod 755 "$W"/victim_open "$W"/victim_prot "$W"/attacker
user() { setpriv --reuid=65534 --regid=65534 --clear-groups "$@"; }
fail=0
attack() {   # $1 victim name, $2 expected word (OK|DENIED)
  ( exec 2>/dev/null; user "$W/$1" > "$W/$1.out"; true ) &
  for i in $(seq 30); do grep -q READY "$W/$1.out" 2>/dev/null && break; sleep 0.1; done
  read -r _ pid addr < "$W/$1.out"
  user "$W/attacker" "$pid" "$addr" > "$W/$1.res" 2>&1
  { kill "$pid"; wait; } 2>/dev/null
  echo "== $1 (expect $2)"; sed 's/^/   /' "$W/$1.res"
  for m in ptrace_attach process_vm_readv proc_mem; do
    grep -q "^$m: $2" "$W/$1.res" || { echo "   FAIL: $m was not $2"; fail=1; }
  done
}
attack victim_open OK
attack victim_prot DENIED
[ $fail = 0 ] && echo "ok protect Process works: open victim readable, protected victim sealed"
exit $fail
