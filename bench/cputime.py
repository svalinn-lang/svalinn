#!/usr/bin/env python3
"""Run a command RUNS times; print the best CPU time (user+sys seconds, children included) and check that its
stdout matches the reference file.
usage: cputime.py RUNS TIMEOUT REFFILE cmd...   -> prints seconds, WRONG or timeout"""
import resource, subprocess, sys

runs, tmo, ref = int(sys.argv[1]), float(sys.argv[2]), open(sys.argv[3], 'rb').read()
best = None
for _ in range(runs):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    p = subprocess.Popen(sys.argv[4:], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        out, _ = p.communicate(timeout=tmo)
    except subprocess.TimeoutExpired:
        p.kill(); p.wait(); print('timeout'); sys.exit()
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if out != ref:
        print('WRONG'); sys.exit()
    t = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
    best = t if best is None else min(best, t)
print(f'{best:.3f}')
