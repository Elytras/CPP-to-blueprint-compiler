#!/usr/bin/env python3
"""Runs the test mods' compiled functions offline (runscript.py) against Python oracles.
Build first: bpbuild.py . BpMods/UeApi x64/Release/assetgen.exe --no-pak"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from runscript import run

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'BpMods', 'build')


def asset(mod):
    return os.path.join(ROOT, mod, 'FSD', 'Content', '_ElytrasMods', mod, mod)


def check(mod, fn, oracle, cases):
    for parms in cases:
        got = run(asset(mod), fn, **parms)[0]
        want = oracle(**parms)
        assert got == want, '%s.%s(%s) = %r, want %r' % (mod, fn, parms, got, want)
    print('ok  %s.%s  (%d cases)' % (mod, fn, len(cases)))


def sum_skipping(Count, Skip):
    t = 0
    for i in range(Count):
        if i == Skip: continue
        if i > 7: break
        t += i
    return t


def first_over(Limit):
    n = 0
    while True:
        n += 1
        if n * n > Limit: break
    return n


def nested(Size):
    h = 0
    for y in range(Size):
        for x in range(Size):
            if x == y: continue
            if x > y: break
            h += 1
    return h


check('FlowTest', 'SumSkipping', sum_skipping, [dict(Count=c, Skip=k) for c in (0, 1, 5, 10, 20) for k in (-1, 0, 3, 9)])
check('FlowTest', 'FirstOver', first_over, [dict(Limit=l) for l in (0, 1, 5, 99, 100)])
check('FlowTest', 'Nested', nested, [dict(Size=s) for s in (0, 1, 2, 4, 7)])
