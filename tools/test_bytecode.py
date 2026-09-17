#!/usr/bin/env python3
"""Runs the test mods' compiled functions offline (runscript.py) against Python oracles.
Build first: bpbuild.py . BpMods/UeApi x64/Release/assetgen.exe --no-pak"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from runscript import run
import dumpexp

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'BpMods', 'build')


def asset(mod):
    return os.path.join(ROOT, mod, 'FSD', 'Content', '_ElytrasMods', mod, mod)


def sweep():
    """Every function of every built mod decodes to exactly its header's storage and memory sizes."""
    import glob, re, subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    n = 0
    for ua in glob.glob(os.path.join(ROOT, '*', 'FSD', 'Content', '**', '*.uasset'), recursive=True):
        base = ua[:-len('.uasset')]
        exports = dumpexp.load(base)[5]
        for i in range(len(exports)):
            out = subprocess.run([sys.executable, os.path.join(here, 'walkscript.py'), base, str(i)],
                                 capture_output=True, text=True).stdout
            m = re.search(r'walked: disk (\d+) \(header (\d+)\)  mem (\d+) \(header (\d+)\)', out)
            if not m: continue
            assert m.group(1) == m.group(2) and m.group(3) == m.group(4), '%s export %d: %s' % (base, i, m.group(0))
            assert 'loader would STOP' not in out, '%s export %d stops early' % (base, i)
            n += 1
    print('ok  %d functions decode to their header sizes' % n)


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


sweep()
check('FlowTest', 'SumSkipping', sum_skipping, [dict(Count=c, Skip=k) for c in (0, 1, 5, 10, 20) for k in (-1, 0, 3, 9)])
check('FlowTest', 'FirstOver', first_over, [dict(Limit=l) for l in (0, 1, 5, 99, 100)])
check('FlowTest', 'Nested', nested, [dict(Size=s) for s in (0, 1, 2, 4, 7)])


def classify(Code):
    return {1: 10, 2: 25, 3: 25, 4: 5}.get(Code, -1)


def no_default(Code):
    return {0: 0, 9: 99}.get(Code, 7)


def default_first(Code):
    return {5: 5, 6: 6}.get(Code, 105)


def switch_in_loop(Count):
    s = 0
    for i in range(Count):
        m = i % 3
        if m == 0: continue
        s += 1 if m == 1 else 10
        s += 100
        if s > 1000: break
    return s


check('FlowTest', 'Classify', classify, [dict(Code=c) for c in range(-2, 8)])
check('FlowTest', 'NoDefault', no_default, [dict(Code=c) for c in (-1, 0, 1, 9, 10)])
check('FlowTest', 'DefaultFirst', default_first, [dict(Code=c) for c in (0, 4, 5, 6, 7)])
check('FlowTest', 'SwitchInLoop', switch_in_loop, [dict(Count=c) for c in (0, 1, 2, 3, 7, 30)])
check('FlowTest', 'ByteSwitch', lambda Mode: {2: 20, 255: 1}.get(Mode, 0), [dict(Mode=m) for m in (0, 2, 254, 255)])
check('FlowTest', 'DenseHoles', lambda Code: {10: 1, 11: 2, 13: 4, 14: 5}.get(Code, -9), [dict(Code=c) for c in range(8, 17)])
check('FlowTest', 'Negative', lambda Code: {-2: -20, -1: -10, 0: 0}.get(Code, 50), [dict(Code=c) for c in range(-4, 3)])
check('FlowTest', 'DenseByte', lambda Mode: {0: 3, 1: 4, 2: 5}.get(Mode, 0), [dict(Mode=m) for m in (0, 1, 2, 3, 255)])


def cdiv(a, b): q = abs(a) // abs(b); return q if (a < 0) == (b < 0) else -q
def cmod(a, b): return a - cdiv(a, b) * b


def compound(N):
    acc = 1
    for i in range(N):
        acc += i; acc *= 2; acc -= 1; acc = cmod(acc, 1000)
    return acc + N - 2


def while_and(Limit):
    i = hits = 0
    while i < Limit and cdiv(100, Limit - i) > 1:
        hits += 1; i += 1
    return hits * 1000 + i


check('FlowTest', 'Arith', lambda A, B: cdiv(A, B) + cmod(A, B) * 10 + A + ~B,
      [dict(A=a, B=b) for a in (-7, 0, 7, 100) for b in (-3, 1, 3)])
check('FlowTest', 'SafeRatio', lambda X: X != 0 and cdiv(10, X) > 2, [dict(X=x) for x in (-2, 0, 1, 3, 4)])
check('FlowTest', 'EitherZero', lambda X, Y: X == 0 or cdiv(100, X) == Y, [dict(X=x, Y=y) for x in (0, 10, 3) for y in (0, 10, 33)])
check('FlowTest', 'Pick', lambda X: X * 2 if X > 0 else (-1 if X < -5 else 7), [dict(X=x) for x in (-9, -5, 0, 4)])
check('FlowTest', 'Compound', compound, [dict(N=n) for n in (0, 1, 5, 40)])
check('FlowTest', 'FloatStep', lambda F: -(F + 1.5), [dict(F=f) for f in (0.0, 2.25, -1.5)])
check('FlowTest', 'WhileAnd', while_and, [dict(Limit=l) for l in (0, 1, 50, 99, 150)])


def range_self(**kw):
    return dict(Items=list(kw.get('Items', [])), Seen=list(kw.get('Seen', [])), Scores=dict(kw.get('Scores', {})))


def check_self(mod, fn, oracle, cases):
    """Like check, but each case is the object's own fields; the oracle gets a copy."""
    for fields in cases:
        mine, theirs = range_self(**fields), range_self(**fields)
        got = run(asset(mod), fn, self_vars=mine)[0]
        want = oracle(theirs)
        assert got == want, '%s.%s(%s) = %r, want %r' % (mod, fn, fields, got, want)
    print('ok  %s.%s  (%d cases)' % (mod, fn, len(cases)))


def double_in_place(f):
    items = f['Items']
    for i, x in enumerate(items):
        if x < 0: continue
        items[i] = x * 2
        if items[i] > 100: break
    s = 0
    for x in items: s = s * 3 + x
    return s


def bump_scores(stop):
    def o(f):
        count = 0
        for k in list(f['Scores']):
            f['Scores'][k] += 10
            count += 1
            if f['Scores'][k] > stop: break
        return sum(f['Scores'].values()) * 100 + count
    return o


arrays = [[], [1], [3, -1, 7], [60, 2, -5, 9], [1, 2, 3, 4, 5]]
check_self('RangeTest', 'SumArray', lambda f: sum(f['Items']), [dict(Items=a) for a in arrays])
check_self('RangeTest', 'DoubleInPlace', double_in_place, [dict(Items=a) for a in arrays])
check_self('RangeTest', 'CopyDoesNotWrite', lambda f: len(f['Items']), [dict(Items=a) for a in arrays])
check_self('RangeTest', 'NestedPairs', lambda f: sum(1 for a in f['Items'] for b in f['Items'] if a < b), [dict(Items=a) for a in arrays])
check_self('RangeTest', 'SumSet', lambda f: sum(f['Seen']), [dict(Seen=a) for a in ([], [4], [1, 5, 9])])
for stop in (0, 15, 1000):
    fn = bump_scores(stop)
    maps = [{}, {'a': 1}, {'a': 1, 'b': 20, 'c': 3}]
    for m in maps:
        mine = range_self(Scores=m)
        got = run(asset('RangeTest'), 'BumpScores', self_vars=mine, Stop=stop)[0]
        want = fn(range_self(Scores=m))
        assert got == want, 'BumpScores(%s, %s) = %r, want %r' % (m, stop, got, want)
print('ok  RangeTest.BumpScores  (9 cases)')


def clamp(v, lo, hi): return lo if v < lo else hi if v > hi else v
def first_above(limit): return next((i for i in range(100) if i * i > limit), -1)
def nest(v): return v * 4 + clamp(v, 0, 10)


check('InlineTest', 'UseClamp', lambda V: clamp(V, -5, 5) + V * 2 + cdiv(V, 2), [dict(V=v) for v in (-9, -1, 0, 3, 7)])
for v in (0, 2, -4):
    fields = {}
    got = run(asset('InlineTest'), 'UseBump', self_vars=fields, V=v)[0]
    assert got == (v + 3 + v) * 100 + 2, ('UseBump', v, got)
print('ok  InlineTest.UseBump  (3 cases)')
check('InlineTest', 'UseLoop', lambda L: sum(first_above(L + i) for i in range(3)), [dict(L=l) for l in (0, 5, 50, 9990)])
check('InlineTest', 'UseNest', lambda V: nest(V) + nest(V + 1), [dict(V=v) for v in (-2, 0, 4, 12)])
check('InlineTest', 'InCond', lambda V: 1 if V * 2 > 10 and clamp(V, 0, 3) == 3 else 0, [dict(V=v) for v in (0, 5, 6, 9)])


def no_inline_ufunctions():
    exports = [e['name'] for e in dumpexp.load(asset('InlineTest'))[5]]
    for name in ('Clamp', 'Half', 'Twice', 'Bump', 'FirstAbove', 'Nest'):
        assert name not in exports, name + ' became a UFunction'
    print('ok  InlineTest: no inline function is a UFunction')


no_inline_ufunctions()
