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
check('FlowTest', 'NameKind', lambda Kind: {'intproperty': 1, 'floatproperty': 2, 'doubleproperty': 2, 'structproperty': 3}.get(Kind.lower(), 0),
      [dict(Kind=k) for k in ('IntProperty', 'intproperty', 'FloatProperty', 'DoubleProperty', 'StructProperty', 'None', 'Int')])
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
check('InlineTest', 'ConstThenVar', lambda V: 6 + V * 2 + clamp(V, 0, 5) + clamp(2, V, 9) + 4 + V - 1, [dict(V=v) for v in (-3, 0, 4, 12)])


def replication():
    import re, subprocess
    here, base = os.path.dirname(os.path.abspath(__file__)), asset('ReplTest')
    exports = [e['name'] for e in dumpexp.load(base)[5]]
    tool = lambda t, i: subprocess.run([sys.executable, os.path.join(here, t), base, str(i)], capture_output=True, text=True).stdout
    cls = tool('dumpstruct.py', 0)
    assert 'NumReplicatedProperties [0] IntProperty size=4: 4' in cls, cls
    for prop, flags, notify, cond in (('Score', '0x10025', 'None', 0), ('bOpen', '0x100010025', 'OnRep_Open', 0),
                                      ('Aim', '0x10025', 'None', 3), ('Slots', '0x100010025', 'OnRep_Slots', 2),
                                      ('Local', '0x10005', 'None', 0)):
        assert re.search(r'Property %s .*flags=%s rep=0 notify=%s cond=%d' % (prop, flags, notify, cond), cls), prop
    for fn, flags in (('ServerOpen', 0x82208c0), ('ClientPing', 0x9020840), ('MultiBoom', 0x8024840), ('OnRep_Open', 0x8020800)):
        assert 'FunctionFlags %#x' % flags in tool('dumpstruct.py', exports.index(fn)), fn
    ops = re.findall(r'\d  (\w+) +(\S*)', tool('walkscript.py', exports.index('ReceiveBeginPlay')))
    calls = [(op, a) for op, a in ops if op in ('VirtualFunction', 'LocalVirtualFunction')]
    # OnRep functions are ordinary script functions, called locally; an RPC goes through CallFunction's routing.
    assert calls == [('LocalVirtualFunction', 'OnRep_Open'), ('LocalVirtualFunction', 'OnRep_Slots'),
                     ('VirtualFunction', 'ServerOpen'), ('VirtualFunction', 'MultiBoom')], calls
    print('ok  ReplTest: replicated properties, RPC flags, OnRep after a set')


replication()


def latent():
    import re, struct, subprocess
    here, base = os.path.dirname(os.path.abspath(__file__)), asset('LatentTest')
    ua, ue, total, names, imports, exports = dumpexp.load(base)
    idx = {e['name']: i for i, e in enumerate(exports)}
    tool = lambda t, i: subprocess.run([sys.executable, os.path.join(here, t), base, str(i)], capture_output=True, text=True).stdout
    assert 'UberGraphFunction [0] ObjectProperty size=4: index %d' % (idx['ExecuteUbergraph_LatentTest'] + 1) in tool('dumpstruct.py', 0)
    assert re.search(r'StructProperty UberGraphFrame .*flags=0x202000', tool('dumpstruct.py', 0))
    uber = tool('dumpstruct.py', idx['ExecuteUbergraph_LatentTest'])
    for local in ('ReceiveBeginPlay_Local', 'ReceiveBeginPlay_I', 'ViaInline_Tag', 'Wait_Seconds', 'Wait_Tag'):
        assert ' %s ' % local in uber, local
    rows = re.findall(r'\+\s*(\d+) mem\s+(\d+)\s+disk\s+\d+ mem\s+\d+\s+(\S+)[ \t]*(.*)', tool('walkscript.py', idx['ExecuteUbergraph_LatentTest']))
    e = exports[idx['ExecuteUbergraph_LatentTest']]
    blob = ue[e['off'] - total: e['off'] - total + e['size']]
    # Each latent call resumes right after the return that follows it; six calls, one in a loop.
    links = [(struct.unpack_from('<i', blob, int(o) + 1)[0], i) for i, (o, m, op, _) in enumerate(rows) if op == 'SkipOffsetConst']
    assert len(links) == 6, links
    for link, i in links:
        ret = next(r for r in rows[i:] if r[2] == 'Return')
        assert link == int(ret[1]) + 2, (link, ret)
    # Each stub enters where its segment starts, the first one right after the computed jump.
    entries = {fn: int(re.search(r'IntConst\s+(\d+)', tool('walkscript.py', idx[fn])).group(1)) for fn in ('Load', 'ReceiveBeginPlay', 'ViaInline', 'Wait')}
    assert min(entries.values()) == 10, entries
    starts = {int(m) for o, m, op, info in rows}
    assert all(v in starts for v in entries.values()), entries
    assert 'LetValueOnPersistentFrame Wait_Tag' in tool('walkscript.py', idx['Wait'])
    assert 'ExecuteUbergraph' not in tool('walkscript.py', idx['Plain'])
    # A value-returning LoadAsset / LoadAssetClass binds a generated event that stores the payload into the frame.
    for ev, local in (('Load_OnLoaded_0', 'Load___Async0__'), ('Load_OnLoaded_1', 'Load___Async1__')):
        assert 'FunctionFlags 0xc000000' in tool('dumpstruct.py', idx[ev]), ev
        assert 'LetValueOnPersistentFrame ' + local in tool('walkscript.py', idx[ev]), ev
    assert 'InstanceDelegate     Load_OnLoaded_0' in tool('walkscript.py', idx['ExecuteUbergraph_LatentTest'])
    print('ok  LatentTest: ubergraph segments, resume linkage, stubs')


latent()


def awaits():
    import re, subprocess
    here, base = os.path.dirname(os.path.abspath(__file__)), asset('AsyncTest')
    idx = {e['name']: i for i, e in enumerate(dumpexp.load(base)[5])}
    tool = lambda t, i: subprocess.run([sys.executable, os.path.join(here, t), base, str(i)], capture_output=True, text=True).stdout
    uber = tool('walkscript.py', idx['ExecuteUbergraph_AsyncTest'])
    rows = re.findall(r'mem\s+(\d+)\s+disk\s+\d+ mem\s+\d+\s+(\S+)', uber)
    # The download task is activated once, after its first bind; the montage proxy is not an async action.
    assert uber.count("Function'Activate'") == 1, uber.count("Function'Activate'")
    for ev, local in (('Download_OnSuccess_0', 'Download___Await0__'), ('Download_OnFail_0', 'Download___Await1__'),
                      ('PlayThen_OnCompleted_0', 'PlayThen___Await0__')):
        out = tool('walkscript.py', idx[ev])
        assert 'FunctionFlags 0xc000000' in tool('dumpstruct.py', idx[ev]), ev
        assert 'LetValueOnPersistentFrame ' + local in out, ev
        # It re-enters right after the return that ends the awaiting run.
        entry = int(re.search(r'IntConst\s+(\d+)', out).group(1))
        at = [i for i, (m, op) in enumerate(rows) if int(m) == entry]
        assert at and rows[at[0] - 2][1] == 'Return', (ev, entry)
        assert 'InstanceDelegate     ' + ev in uber, ev
    print('ok  AsyncTest: awaits bind, activate once, resume after the run')


awaits()


def nested():
    fields = {}
    got = run(asset('NestedTest'), 'Build', self_vars=fields)[0]
    # Found holds 2; Grid ends [[7], [1]] after Grid[1].Add(5), Grid[0][0] = 7 and Grid[1] = Row.
    assert got == 2 + 2 + 7, got
    assert fields == {'Groups': {'first': ['a', 'b']}, 'Grid': [[7], [1]]}, fields
    out = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dumpstruct.py'),
                          asset('NestedTest'), '0'], capture_output=True, text=True).stdout
    assert "UserDefinedStruct'FNC_TArray_FName'" in out and "UserDefinedStruct'FNC_TArray_int'" in out, out
    check('NestedTest', 'MapIndex', lambda Seed: (Seed + 2) * 10 + 1, [dict(Seed=v) for v in (0, 5, -3)])
    print('ok  NestedTest: containers inside containers through wrapper structs')


import subprocess
nested()


def optimizer():
    check('OptTest', 'Drop', lambda A, B: A * B * 2, [dict(A=a, B=b) for a, b in ((2, 3), (-4, 5), (0, 0))])
    base = asset('OptTest')
    names = [e['name'] for e in dumpexp.load(base)[5]]
    walk = lambda fn: subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'walkscript.py'),
                                      base, str(names.index(fn))], capture_output=True, text=True).stdout
    assert walk('Drop').count('Multiply_IntInt') == 2 and 'Abs_Int' not in walk('Drop'), walk('Drop')
    w = walk('Locals')
    assert 'Multiply_IntInt' not in w and 'Add_IntInt' not in w and 'Kept' in w and 'RandomInteger' in w, w
    print('ok  OptTest: unused pure calls dropped, used ones kept')
    print('ok  OptTest.Locals: unread locals dropped, the property store and the impure call kept')


optimizer()


def member_address():
    base = os.path.join(ROOT, 'CppTest', 'FSD', 'Content', '_ElytrasMods', 'CppTest', 'Test')
    names = [e['name'] for e in dumpexp.load(base)[5]]
    w = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'walkscript.py'),
                        base, str(names.index('MemberAddress'))], capture_output=True, text=True).stdout
    assert "Function'GetPropertyAddress'" in w and 'NameConst        Vtbl' in w, w
    print('ok  CppTest.MemberAddress: &Member through ReadProperty::GetPropertyAddress')


member_address()
