#!/bin/bash
# Benchmark harness: builds every implementation, checks that all outputs agree with C, reports the best of N runs
# in CPU seconds (user+sys; less sensitive to machine noise than wall-clock time).
# usage: bench/run.sh [runs] [langs...]   (langs: aegis-O0 aegis c rust go java node python)
cd "$(dirname "$0")"
RUNS=${1:-3}; shift
LANGS=${@:-aegis-O0 aegis c rust go java node python}
BENCHES="fib sieve nbody spectral mandelbrot fannkuch bintrees matmul"
declare -A JNAME=([fib]=Fib [sieve]=Sieve [nbody]=NBody [spectral]=Spectral [mandelbrot]=Mandelbrot [fannkuch]=Fannkuch [bintrees]=BinTrees [matmul]=MatMul)
B=/tmp/aegis_bench; mkdir -p $B $B/jcls
export GOCACHE=$B/gocache
for b in $BENCHES; do
  ../aegisc $b.ag -o $B/ag0_$b || exit 1
  ../aegisc -O $b.ag -o $B/ag_$b || exit 1
  gcc -O2 -march=native -o $B/c_$b c/$b.c -lm
  rustc -C opt-level=3 -C target-cpu=native -o $B/rs_$b rs/$b.rs 2>/dev/null
  go build -o $B/go_$b go/$b.go
  $B/c_$b > $B/ref_$b.txt
done
javac -d $B/jcls java/*.java 2>/dev/null
cmd() { case $1 in
  aegis-O0) echo "$B/ag0_$2";; aegis) echo "$B/ag_$2";; c) echo "$B/c_$2";; rust) echo "$B/rs_$2";; go) echo "$B/go_$2";;
  java) echo "java -cp $B/jcls ${JNAME[$2]}";; node) echo "node js/$2.js";; python) echo "python3 py/$2.py";; esac; }
printf "%-11s" bench; for l in $LANGS; do printf "%10s" $l; done; echo
for b in $BENCHES; do
  printf "%-11s" $b
  for l in $LANGS; do
    r=$RUNS; tmo=120; [ $l = python ] && { r=1; tmo=400; }
    t=$(python3 cputime.py $r $tmo $B/ref_$b.txt $(cmd $l $b))
    printf "%10s" $t
  done
  echo
done
