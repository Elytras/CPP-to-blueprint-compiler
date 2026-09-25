#!/usr/bin/env python3
"""usage: test_bytecode.py [--assetgen <exe>] [--ueapi <UeApi dir>]

Compiles every test mod in AssetGen/tests, then checks what a mod can observe: its functions run offline
(runscript.py, runvm.py for latent / delegate / cross-object code) against Python oracles, return values and member
writes both, and what the engine reads off the cooked assets (flags, property types, defaults, references, which
function a call reaches). Never the bytecode's shape: an optimization that keeps the behaviour must pass.

--assetgen defaults to the first build found (ue-mods x64/Release, this repo's x64/Release, a CMake build/);
--ueapi to ue-mods' BpMods/UeApi. Outside ue-mods, pass the UeApi of https://github.com/Elytras/DRG-Blueprint-Cpp-SDK."""
import glob, os, re, shutil, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runscript
from runscript import run, i32
from runvm import VM, Obj, latent_call
import dumpexp

HERE = os.path.dirname(os.path.abspath(__file__))
AG = os.path.normpath(os.path.join(HERE, '..'))
TESTS = os.path.join(AG, 'tests')
ROOT = os.path.join(TESTS, 'build')


def option(flag, candidates):
    if flag in sys.argv:
        return os.path.abspath(sys.argv[sys.argv.index(flag) + 1])   # Windows won't run a relative x64/Release/assetgen.exe
    return next((c for c in candidates if os.path.exists(c)), None)


ASSETGEN = option('--assetgen', [os.path.join(AG, '..', 'x64', 'Release', 'assetgen.exe'),
                                 os.path.join(AG, 'x64', 'Release', 'assetgen.exe'), os.path.join(AG, 'build', 'assetgen')])
UEAPI = option('--ueapi', [os.path.join(AG, '..', 'BpMods', 'UeApi')])
if not ASSETGEN or not UEAPI:
    sys.exit(__doc__)


def build():
    """Each test compiles into build/<Test>/FSD/Content/<its package>, the layout bpbuild stages a mod in."""
    shutil.rmtree(ROOT, ignore_errors=True)
    for src in sorted(glob.glob(os.path.join(TESTS, '*.cpp'))):
        mod = os.path.splitext(os.path.basename(src))[0]
        package = re.search(r'UE_MOD_PACKAGE\s*\(\s*"/Game/([^"]+)"', open(src, encoding='utf-8-sig').read()).group(1)
        out = os.path.join(ROOT, mod, 'FSD', 'Content', *package.split('/'))
        os.makedirs(out)
        proc = subprocess.run([ASSETGEN, 'compile', src, UEAPI, out], capture_output=True, text=True)
        assert proc.returncode == 0, '%s:\n%s%s' % (mod, proc.stdout, proc.stderr)
    print('ok  every test compiles')


build()


def asset(mod):
    return os.path.join(ROOT, mod, 'FSD', 'Content', '_ElytrasMods', mod, mod)


def dump(t, base, i):
    return subprocess.run([sys.executable, os.path.join(HERE, t), base, str(i)], capture_output=True, text=True).stdout


def exports_of(base):
    return [e['name'] for e in dumpexp.load(base)[5]]


def import_paths(base):
    """Each import as its full path, /Game/Pkg.Class_C:Function. dumpexp keeps Class'Name' only, and one package
    can import two objects of one name (SuperTest: SuperBase_C:Bump for the parent call, its own SuperTest_C:Bump)."""
    import struct
    ua, names = open(base + '.uasset', 'rb').read(), dumpexp.load(base)[3]
    r = dumpexp.R(ua, 4)                                        # the summary, as dumpexp.load walks it
    if r.i32() != -4: r.i32()
    r.i32(); r.i32()
    custom = r.i32()
    r.o += 20 * custom
    r.i32(); r.fstr(); flags = r.u32()
    r.o += 8
    if not flags & 0x80000000: r.fstr()
    r.o += 16
    count, off = r.i32(), r.i32()
    stride = 28 if flags & 0x80000000 else 36
    rows = [struct.unpack_from('<7i', ua, off + stride * i) for i in range(count)]

    def path(i):
        outer, obj, num = rows[i][4:]
        name = names[obj] + ('_%d' % (num - 1) if num else '')
        if outer == 0: return name
        return path(-outer - 1) + ('.' if rows[-outer - 1][4] == 0 else ':') + name
    return [path(i) for i in range(count)]


def ref(base, index):
    """An FPackageIndex off a tag or an export header, as an import's full path or this package's export name."""
    return import_paths(base)[-index - 1] if index < 0 else exports_of(base)[index - 1]


def registry_of(mod):
    """A pak's one AssetRegistry.bin sits beside its Content folder, as the game's FSD/AssetRegistry.bin does."""
    return os.path.join(ROOT, mod, 'FSD', 'AssetRegistry.bin')


def registry_rows(path):
    return set(re.findall(r'^\s+(/Game/\S+)\s+(\S+)$', subprocess.run(
        [sys.executable, os.path.join(HERE, 'dumpar.py'), path], capture_output=True, text=True).stdout, re.M))


def registry_layout():
    """Each test's registry is FSD/AssetRegistry.bin, none sits in a package folder, and `assetgen registry`
    folds several into one - what bpbuild does for an embedded dependency - replacing a package it already has."""
    import tempfile
    for mod in os.listdir(ROOT):
        assert os.path.exists(registry_of(mod)), mod
        assert not glob.glob(os.path.join(ROOT, mod, 'FSD', 'Content', '**', 'AssetRegistry.bin'), recursive=True), mod
    a, b = registry_rows(registry_of('AssetTest')), registry_rows(registry_of('IfaceTest'))
    assert a and b and not a & b, (a, b)
    with tempfile.TemporaryDirectory() as tmp:
        merged = os.path.join(tmp, 'AssetRegistry.bin')
        for twice in range(2):
            proc = subprocess.run([ASSETGEN, 'registry', merged, registry_of('AssetTest'), registry_of('IfaceTest')],
                                  capture_output=True, text=True)
            assert proc.returncode == 0 and registry_rows(merged) == a | b, (proc.stdout, registry_rows(merged))
    print('ok  every registry is FSD/AssetRegistry.bin; `assetgen registry` merges them, once per package')


def export_index(base, name):
    return exports_of(base).index(name)


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


def cdiv(a, b): q = abs(a) // abs(b); return q if (a < 0) == (b < 0) else -q
def cmod(a, b): return a - cdiv(a, b) * b
def clamp(v, lo, hi): return lo if v < lo else hi if v > hi else v
def wrap(v): return (v + 2**31) % 2**32 - 2**31          # Kismet int32 arithmetic wraps
EDGE = (-2**31, -2, 0, 9, 2**30, 2**31 - 1)


# ---- FlowTest

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
registry_layout()
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
check('FlowTest', 'NameSet', lambda N: 2 if N.lower() == 'none' else 1, [dict(N=n) for n in ('None', 'none', 'IntProperty', 'x', 'None_1')])
check('FlowTest', 'NameKind', lambda Kind: {'intproperty': 1, 'floatproperty': 2, 'doubleproperty': 2, 'structproperty': 3}.get(Kind.lower(), 0),
      [dict(Kind=k) for k in ('IntProperty', 'intproperty', 'FloatProperty', 'DoubleProperty', 'StructProperty', 'None', 'Int')])
check('FlowTest', 'DefaultFirst', default_first, [dict(Code=c) for c in (0, 4, 5, 6, 7)])
check('FlowTest', 'SwitchInLoop', switch_in_loop, [dict(Count=c) for c in (0, 1, 2, 3, 7, 30)])
check('FlowTest', 'ByteSwitch', lambda Mode: {2: 20, 255: 1}.get(Mode, 0), [dict(Mode=m) for m in (0, 2, 254, 255)])
check('FlowTest', 'DenseHoles', lambda Code: {10: 1, 11: 2, 13: 4, 14: 5}.get(Code, -9), [dict(Code=c) for c in range(8, 17)])
check('FlowTest', 'Negative', lambda Code: {-2: -20, -1: -10, 0: 0}.get(Code, 50), [dict(Code=c) for c in range(-4, 3)])


def clusters():
    # every hole and both edges of each dense run, the outliers' neighbours, and the int32 extremes
    check('FlowTest', 'Clusters', lambda Code: CLUSTERS.get(Code, -1),
          [dict(Code=c) for c in list(range(-2, 13)) + list(range(98, 113)) + [49, 50, 51, 6999, 7000, 7001, -2**31, 2**31 - 1]])


def flow_members():
    """Member side effects: BeginPlay's store, Slots[NextSlot()] += By locating its slot once, the Cursor a template
    reads, and locals named Total leaving the member alone."""
    from runscript import i32
    base = asset('FlowTest')
    exports = dumpexp.load(base)[5]
    imports = dumpexp.load(base)[4]
    bp = next(e for e in exports if e['name'] == 'ReceiveBeginPlay')
    assert imports[-bp['super'] - 1] == "Function'ReceiveBeginPlay'", imports[-bp['super'] - 1]   # overrides the Actor event
    me = dict(Total=-1)
    assert run(base, 'ReceiveBeginPlay', self_vars=me)[0] is None and me == dict(Total=25 + 3 + 6), me
    for cur in (0, 2):
        me = dict(Slots=[10, 20, 30], Cursor=cur)
        run(base, 'BumpSlot', self_vars=me, By=-7)
        want = [10, 20, 30]
        want[cur] -= 7
        assert me == dict(Slots=want, Cursor=cur + 1), me
    me = dict(Cursor=2**31 - 1)
    assert run(base, 'NextSlot', self_vars=me)[0] == 2**31 - 1 and me == dict(Cursor=-2**31), me
    for cur in (0, 5, -4):
        for x in (-2, 7, 10**9):
            me = dict(Cursor=cur)
            got = run(base, 'TemplateMember', self_vars=me, X=x)[0]
            assert got == i32(2 * i32(3 * x + cur)) and me == dict(Cursor=cur), (cur, x, got)
    for fn, want in (('FreshLocals', 1000), ('FreshInLoop', 600)):
        me = dict(Total=777, Cursor=5, Slots=[1])
        assert run(base, fn, self_vars=me, N=3)[0] == want and me == dict(Total=777, Cursor=5, Slots=[1]), (fn, me)
    print('ok  FlowTest: BeginPlay, BumpSlot, NextSlot, TemplateMember and local Totals, on the members')


def call_member():
    """A member of a returned struct: Translation.Y of the transform GetTransform returns."""
    import runscript
    runscript.MATH['GetTransform'] = lambda: {'Translation': {'X': 1.0, 'Y': 2.5, 'Z': -3.0}, 'Scale3D': {'X': 7.0, 'Y': 8.0, 'Z': 6.0}}
    try:
        assert run(asset('FlowTest'), 'CallMember')[0] == 2.5
    finally:
        del runscript.MATH['GetTransform']
    print('ok  FlowTest.CallMember: Translation.Y of GetTransform()')


def float_step():
    import math
    check('FlowTest', 'FloatStep', lambda F: -(F + 1.5), [dict(F=f) for f in (0.0, 2.25, -1.5, -0.5, 100.75)])
    r = run(asset('FlowTest'), 'FloatStep', F=-1.5)[0]
    assert math.copysign(1, r) == -1, 'FloatStep(-1.5) = %r: C++ -F of +0.0 is -0.0' % r
    print('ok  FlowTest.FloatStep: -F flips the sign of zero too')


def flow_exports():
    """Inline and template members are expanded at their calls: none of them is a UFunction."""
    names = {e['name'] for e in dumpexp.load(asset('FlowTest'))[5]}
    for name in ('TwicePlus', 'SumTo', 'TwoOf', 'FirstSquareAbove', 'StopBelow', 'Scaled', 'WidthOf'):
        assert name not in names, name + ' became a UFunction'
    print('ok  FlowTest: no inline or template member is a UFunction')


CLUSTERS = {0: 1, 1: 2, 2: 3, 5: 6, 9: 10, 50: 500, 100: 1000, 103: 1003, 104: 1004, 110: 1010, 7000: 7}
clusters()
check('FlowTest', 'DenseByte', lambda Mode: {0: 3, 1: 4, 2: 5}.get(Mode, 0), [dict(Mode=m) for m in (0, 1, 2, 3, 255)])


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
check('FlowTest', 'PostInc', lambda X: X * 100 + X + 1, [dict(X=x) for x in (-1, 0, 5)])
check('FlowTest', 'PreInc', lambda X: (X + 1) * 101, [dict(X=x) for x in (-1, 0, 5)])
check('FlowTest', 'UpdateChain', lambda X: (1 + X) * 11, [dict(X=x) for x in (-1, 0, 4)])
check('FlowTest', 'OrAssign', lambda N: 11 if N > 0 else 0, [dict(N=n) for n in (-1, 0, 3)])
check('FlowTest', 'TemplateMember', lambda X: X * 6, [dict(X=x) for x in (-2, 0, 7)])
check('FlowTest', 'ConstexprPick', lambda: 84, [dict()])
check('FlowTest', 'CoalesceLoop', lambda N: sum((2 * i + 1) + (2 * (i + 1) + 1) for i in range(2 * N + 1)) * 1000 + 2 * N + 1,
      [dict(N=n) for n in (-1, 0, 1, 3)])
check('FlowTest', 'LoopInline', lambda N: max(N, 0) ** 2 * 100 + max(2 * N + 1, 0) ** 2, [dict(N=n) for n in (-3, 0, 1, 4)])
check('FlowTest', 'FreshLocals', lambda N: 400 + 200 * max(N, 0), [dict(N=n) for n in (0, 1, 3)])
check('FlowTest', 'FreshInLoop', lambda N: 200 * max(N, 0), [dict(N=n) for n in (0, 1, 3)])
check('FlowTest', 'SafeRatio', lambda X: X != 0 and cdiv(10, X) > 2, [dict(X=x) for x in (-2, 0, 1, 3, 4)])
check('FlowTest', 'EitherZero', lambda X, Y: X == 0 or cdiv(100, X) == Y, [dict(X=x, Y=y) for x in (0, 10, 3) for y in (0, 10, 33)])
check('FlowTest', 'Pick', lambda X: X * 2 if X > 0 else (-1 if X < -5 else 7), [dict(X=x) for x in (-9, -5, 0, 4)])
check('FlowTest', 'ConstBreak', lambda X: 205, [dict(X=0)])
vm = VM(asset('FlowTest'))
assert [vm.call('UseDefault', X=x) for x in (-2, 0, 5)] == [(x * 3 + x * 10 + (x + 7) * 1000) for x in (-2, 0, 5)]
print('ok  FlowTest.UseDefault: a defaulted argument is the parameter\'s default, to a method and inlined')
check('FlowTest', 'Compound', compound, [dict(N=n) for n in (0, 1, 5, 40)])
check('FlowTest', 'WhileAnd', while_and, [dict(Limit=l) for l in (0, 1, 50, 99, 150)])


def do_continue(Limit):
    i = s = 0
    while True:
        i += 1
        if i % 2 != 0:
            if i > 9: break
            s += i
        if not i < Limit: break
    return s * 100 + i


def goto_out(Size, Want):
    for y in range(Size):
        for x in range(Size):
            if x * y == Want: return y * 100 + x
    return -2


def if_init(V):
    twice = V * 2
    r = twice if twice > 10 else -twice
    return r + cmod(V, 3) * 1000


def switch_init(V):
    k, m = V + 1, cmod(V, 4)
    if k == 1: return 10
    if k == 2: return 20 + k
    return 300 + m if m == 3 else m


def while_var(Start):
    steps = 0
    while Start - steps != 0:
        left = Start - steps
        steps += 1
        if left < 0: break
    return steps


check('FlowTest', 'DoOnce', lambda Limit: max(Limit, 1), [dict(Limit=l) for l in (-3, 0, 1, 2, 9)])
check('FlowTest', 'DoContinue', do_continue, [dict(Limit=l) for l in (0, 1, 2, 6, 7, 30)])
check('FlowTest', 'GotoLoop', lambda N: sum(range(max(N, 0))), [dict(N=n) for n in (-1, 0, 1, 5, 40)])
check('FlowTest', 'GotoOut', goto_out, [dict(Size=s, Want=w) for s in (0, 1, 4) for w in (0, 6, 7)])
def first_square_above(floor): return next(n for n in range(1, 100) if n * n > floor)
check('FlowTest', 'GotoInlined', lambda A, B: first_square_above(A) * 100 + first_square_above(B), [dict(A=a, B=b) for a, b in ((0, 0), (10, 50), (99, 3))])
check('FlowTest', 'GotoRedeclares', lambda Rounds: 5 * max(Rounds, 1), [dict(Rounds=r) for r in (0, 1, 3)])
check('FlowTest', 'IfInit', if_init, [dict(V=v) for v in (-4, 0, 3, 5, 6, 8)])
check('FlowTest', 'SwitchInit', switch_init, [dict(V=v) for v in (-1, 0, 1, 2, 3, 7)])
check('FlowTest', 'WhileVar', while_var, [dict(Start=s) for s in (-2, 0, 1, 5)])
flow_members()
call_member()
float_step()
flow_exports()


# ---- RangeTest

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
check_self('RangeTest', 'SumScaled', lambda f: 3 * sum(f['Items']), [dict(Items=a) for a in arrays])
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
        theirs = range_self(Scores=m)
        want = fn(theirs)
        assert (got, mine) == (want, theirs), 'BumpScores(%s, %s) = %r %r, want %r %r' % (m, stop, got, mine, want, theirs)
print('ok  RangeTest.BumpScores  (9 cases)')
for m in ({}, {'a': {'X': 1, 'Y': 2}}, {'a': {'X': -1, 'Y': 0}, 'b': {'X': 5, 'Y': 7}}):
    f = dict(Spots={k: dict(v) for k, v in m.items()})
    got = run(asset('RangeTest'), 'ShiftSpots', self_vars=f)[0]
    want = {k: dict(v, X=v['X'] + 1) for k, v in m.items()}
    assert (got, f['Spots']) == (sum(v['X'] * 10 + v['Y'] for v in want.values()), want), (m, got, f)
print('ok  RangeTest.ShiftSpots: a struct value changed through `.` goes back to the map')
for hits in ((), (0,), (4, -1)):
    peers = {'p%d' % i: Obj('RangeTest_C', Hits=h) for i, h in enumerate(hits)}
    vm = VM(asset('RangeTest'), Peers=dict(peers))
    vm.call('PokePeers')
    assert vm.self.vars['Peers'] == peers and [p.vars['Hits'] for p in peers.values()] == [h + 1 for h in hits], hits
print('ok  RangeTest.PokePeers: `Peer->Hits += 1` writes each object, the map keeps its pointers')


def range_members():
    """Range-for side effects on the members: a by-value loop variable writes nothing, a reference one writes
    exactly the elements the loop reached."""
    for a in arrays + [[51, 1], [200, 7]]:
        for fn in ('SumArray', 'SumScaled', 'CopyDoesNotWrite', 'NestedPairs'):
            mine = range_self(Items=a)
            run(asset('RangeTest'), fn, self_vars=mine)
            assert mine == range_self(Items=a), (fn, a, mine)
        mine, theirs = range_self(Items=a), range_self(Items=a)
        got = run(asset('RangeTest'), 'DoubleInPlace', self_vars=mine)[0]
        assert got == double_in_place(theirs) and mine == theirs, (a, got, mine, theirs)
    for s in ([], [4], [1, 5, 9]):
        mine = range_self(Seen=s)
        assert run(asset('RangeTest'), 'SumSet', self_vars=mine)[0] == sum(s) and mine == range_self(Seen=s)
    print('ok  RangeTest: members written only through a reference loop variable')


range_members()


# ---- InlineTest

def first_above(limit): return next((i for i in range(100) if i * i > limit), -1)
def nest(v): return v * 4 + clamp(v, 0, 10)


check('InlineTest', 'UseClamp', lambda V: clamp(V, -5, 5) + V * 2 + cdiv(V, 2), [dict(V=v) for v in (-9, -1, 0, 3, 7)])
check('InlineTest', 'UseLoop', lambda L: sum(first_above(L + i) for i in range(3)), [dict(L=l) for l in (0, 5, 50, 9990)])
check('InlineTest', 'UseNest', lambda V: nest(V) + nest(V + 1), [dict(V=v) for v in (-2, 0, 4, 12)])
check('InlineTest', 'UseStatic', lambda V: i32(V * 2 + V - 1 + 6), [dict(V=v) for v in (-3, 0, 7, 2**30)])
check('InlineTest', 'StaticFromInst', lambda V: i32(V * 2 + V - 1), [dict(V=v) for v in (-3, 0, 7, 2**30)])
check('InlineTest', 'TwiceTwice', lambda V: i32(4 * V + 12), [dict(V=v) for v in (-2, 0, 5, 2**29, -2**31)])
check('InlineTest', 'ClampInArg', lambda V: max(4, clamp(V, 0, 10)), [dict(V=v) for v in (-3, 0, 2, 4, 5, 8, 9, 10, 11, 15)])
check('InlineTest', 'InCond', lambda V: 1 if i32(V * 2) > 10 and clamp(V, 0, 3) == 3 else 0,
      [dict(V=v) for v in (-7, 0, 5, 6, 9, 2**30)])
check('InlineTest', 'InPlace', lambda V: V * 3 + 1 + V + 1, [dict(V=v) for v in (-3, 0, 7)])
check('InlineTest', 'ConstThenVar', lambda V: 6 + V * 2 + clamp(V, 0, 5) + clamp(2, V, 9) + 4 + V - 1, [dict(V=v) for v in (-3, 0, 4, 12)])


def inline_members():
    """Side effects through an inline body land on self, once per expansion, whatever Counter started at."""
    for v in (0, 2, -4, 2**30):
        for c in (0, 5):
            fields = dict(Counter=c)
            got = run(asset('InlineTest'), 'UseBump', self_vars=fields, V=v)[0]
            assert got == i32(i32(v + 3 + v) * 100 + c + 2) and fields == dict(Counter=c + 2), ('UseBump', v, c, got, fields)
            fields = dict(Counter=c)
            got = run(asset('InlineTest'), 'KeptLocal', self_vars=fields, V=v)[0]
            assert got == i32(v * 3) and fields == dict(Counter=c + 1), ('KeptLocal', v, c, got, fields)
    print('ok  InlineTest: UseBump / KeptLocal side effects on Counter')


def inline_statics():
    """A static function stays FUNC_Static (callable with no instance); an instance one does not."""
    base = asset('InlineTest')
    flags = lambda fn: int(re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base, export_index(base, fn))).group(1), 16)
    assert flags('UseStatic') & 0x2000 and not flags('StaticFromInst') & 0x2000, (hex(flags('UseStatic')), hex(flags('StaticFromInst')))
    print('ok  InlineTest: static stays static')


def no_inline_ufunctions():
    exports = [e['name'] for e in dumpexp.load(asset('InlineTest'))[5]]
    for name in ('Clamp', 'Half', 'Twice', 'Bump', 'FirstAbove', 'Nest', 'Dec', 'Plus1', 'Late', 'SDouble', 'SPred'):
        assert name not in exports, name + ' became a UFunction'
    print('ok  InlineTest: no inline function is a UFunction')


def inline_regressions():
    """Miscompiles that once shipped, run: a by-value argument read after the body changed its source, a T& bound to an
    array element (its index a call, run once), and a do/while whose test is an inline call."""
    for c in (0, 5):
        f = dict(Counter=c)
        assert run(asset('InlineTest'), 'LateMember', self_vars=f)[0] == c * 100 + c + 1 and f == dict(Counter=c + 1), (c, f)
    for by in (-3, 0, 4):
        f = dict(Counter=5, Calls=0, Arr=[])
        got = run(asset('InlineTest'), 'BumpElem', self_vars=f, By=by)[0]
        assert got == (10 + by) * 100 + 10 + 6 and f == dict(Counter=6, Calls=1, Arr=[10 + by]), (by, got, f)
    check('InlineTest', 'DoInline', lambda N: next(i for i in range(1, 100) if 2 * i >= N), [dict(N=n) for n in (-3, 0, 1, 2, 4, 5, 12)])
    print('ok  InlineTest: LateMember, BumpElem and DoInline run as C++ does')


inline_members()
inline_statics()
no_inline_ufunctions()
inline_regressions()


# ---- OptTest

check('OptTest', 'Drop', lambda A, B: i32(A * B * 2), [dict(A=a, B=b) for a, b in ((2, 3), (-4, 5), (0, 0), (2**16, 2**15), (-2**31, 1))])
check('OptTest', 'Consts', lambda A: A * 3 + 3 + (10 + A), [dict(A=a) for a in (0, 4, -7)])
check('OptTest', 'Logic', lambda X, Y: (1 if X != 0 and cdiv(10, X) > 2 else 0) + (2 if X != 0 and Y != 0 else 0)
      + (10 if X > 0 and cdiv(Y, 2) > 0 else 0) + (100 if X == 0 or Y == 0 else 0),
      [dict(X=x, Y=y) for x in (0, 3, 4, -4, 20) for y in (0, 1, 2, 5, -3)])
check('OptTest', 'Raw', lambda X, Y: 1 if X != 0 and Y != 0 else 0, [dict(X=x, Y=y) for x in (0, 3) for y in (0, 5)])


def opt_locals():
    """Locals: the self property is written and the impure RandomInteger runs once with A; the result is A."""
    for a in (0, 5, -3):
        fields = {}
        del runscript.CALLS[:]
        got = run(asset('OptTest'), 'Locals', self_vars=fields, A=a)[0]
        assert got == a and fields == dict(Kept=a - 1), (a, got, fields)
        assert [c for c in runscript.CALLS if c[0] == 'RandomInteger'] == [('RandomInteger', (a,))], runscript.CALLS
    print('ok  OptTest.Locals: Kept written, RandomInteger(A) called once')


def opt_raw():
    """UE_NO_OPTIMIZE / #pragma clang optimize off: what the source says runs, unused pure calls and the
    short-circuit included (an execution trace, not the layout)."""
    for x, y in ((0, 5), (3, 5), (3, 0), (-2, 7)):
        del runscript.CALLS[:]
        assert run(asset('OptTest'), 'Raw', X=x, Y=y)[0] == (1 if x != 0 and y != 0 else 0)
        assert ('Multiply_IntInt', (x, y)) in runscript.CALLS and ('Add_IntInt', (x, 3)) in runscript.CALLS, runscript.CALLS
        assert (('NotEqual_IntInt', (y, 0)) in runscript.CALLS) == (x != 0), runscript.CALLS      # Y != 0 runs only when X != 0
    for x in (-4, 0, 9):
        del runscript.CALLS[:]
        assert run(asset('OptTest'), 'RawPragma', X=x)[0] == x and ('Abs_Int', (x,)) in runscript.CALLS, runscript.CALLS
    print('ok  OptTest.Raw / RawPragma: everything the source says runs')


def opt_flags():
    """UE_NO_OPTIMIZE / #pragma clang optimize off change the body only, never what the engine sees of the function."""
    base = asset('OptTest')
    flags = lambda f: re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base, export_index(base, f))).group(1)
    assert flags('RawPragma') == flags('Raw') == flags('Drop'), (flags('RawPragma'), flags('Drop'))
    print('ok  OptTest: an unoptimized function has the flags of an optimized one')


opt_locals()
opt_raw()
opt_flags()


# ---- NestedTest

def nested_containers():
    fields = {}
    got = run(asset('NestedTest'), 'Build', self_vars=fields)[0]
    # Found and Again hold 2; Grid ends [[7], [1]] after Grid[1].Add(5), Grid[0][0] = 7 and Grid[1] = Row.
    assert got == 2 + 2 + 2 + 7, got
    assert fields == {'Groups': {'first': ['a', 'b']}, 'Grid': [[7], [1]]}, fields
    out = dump('dumpstruct.py', asset('NestedTest'), 0)
    assert "UserDefinedStruct'FNC_TArray_FName'" in out and "UserDefinedStruct'FNC_TArray_int'" in out, out
    check('NestedTest', 'MapIndex', lambda Seed: (Seed + 2) * 10 + 1, [dict(Seed=v) for v in (0, 5, -3)])
    print('ok  NestedTest: containers inside containers through wrapper structs')


def map_index_members():
    for seed in (0, 5, -3):
        for start in ({}, {'b': 4, 'a': 99}):
            fields = {'Counts': dict(start)}
            got = run(asset('NestedTest'), 'MapIndex', self_vars=fields, Seed=seed)[0]
            assert got == (seed + 2) * 10 + 1 and fields == {'Counts': dict(start, a=seed + 2)}, (seed, start, got, fields)
    print('ok  NestedTest.MapIndex: Counts[a] ends at Seed + 2, other keys untouched')


def nested_find_types():
    """execMap_Find writes its out-parm in place only when that property's class matches the map's value property
    (ScriptCore / KismetArrayLibrary): so every Map_Find on Groups must write a FNC_TArray_FName-typed local."""
    base = asset('NestedTest')
    props = dict(re.findall(r"^\s+StructProperty (\w+) .*?(UserDefinedStruct'\w+')", dump('dumpstruct.py', base, export_index(base, 'Build')), re.M))
    finds = []
    def walk(n):
        if n.op in (0x1C, 0x46, 0x68) and n.val == 'Map_Find' and n.kids[0].op == 1 and n.kids[0].val == 'Groups':
            finds.append(n.kids[2].val)
        for k in n.kids: walk(k)
    for st in runscript.script_of(base, 'Build'): walk(st)
    assert len(finds) == 2 and all(props.get(o) == "UserDefinedStruct'FNC_TArray_FName'" for o in finds), (finds, props)
    print("ok  NestedTest: Map_Find on Groups writes a value of the map's own value type")


nested_containers()
map_index_members()
nested_find_types()


# ---- CompTest

def comp_test():
    base = asset('CompTest')
    for t in (0, 41):
        fields = dict(Ticks=t)
        assert run(base, 'ReceiveBeginPlay', self_vars=fields)[0] is None and fields == dict(Ticks=t + 1), fields
    # The engine dispatches it: an override of Actor's BlueprintImplementableEvent, not final.
    fn = dump('dumpstruct.py', base, export_index(base, 'ReceiveBeginPlay'))
    flags = int(re.search(r'FunctionFlags (\S+)', fn).group(1), 16)
    assert "SuperStruct imp[" in fn and "Function'ReceiveBeginPlay'" in fn and flags & 0x08000800 == 0x08000800 and not flags & 1, fn
    cls = dump('dumpstruct.py', base, 0)
    for var, klass in (('Root', 'SceneComponent'), ('Mesh', 'StaticMeshComponent'), ('Lamp', 'PointLightComponent')):
        assert re.search(r"ObjectProperty %s .*Class'%s'" % (var, klass), cls), (var, cls)
    assert re.search(r'IntProperty Ticks ', cls), cls
    tags = lambda name: dump('dumptags.py', base, export_index(base, name))
    assert 'bVisible [0] BoolProperty size=0 value=0' in tags('Mesh_GEN_VARIABLE')
    lamp = tags('Lamp_GEN_VARIABLE')
    assert 'Intensity [0] FloatProperty size=4: 1500.0' in lamp and 'LightColor [0] StructProperty size=4 struct=Color: ff8000ff' in lamp, lamp
    assert 'RelativeScale3D [0] StructProperty size=12 struct=Vector: 000000400000004000004040' in tags('Root_GEN_VARIABLE')
    print('ok  CompTest: BeginPlay override, component variables and archetype defaults')


def scs_tree(base):
    """An actor class's SCS as the engine walks it: {node's variable: [its ChildNodes' variables]}, and the root nodes."""
    names = exports_of(base)
    var, tags = {}, {}
    for i, n in enumerate(names):
        if n.startswith('SCS_Node_'):
            tags[n] = dump('dumptags.py', base, i)
            var[n] = re.search(r'InternalVariableName \[0\] NameProperty size=8: (\w+)', tags[n]).group(1)

    def objs(t, prop):
        m = re.search(prop + r' \[0\] ArrayProperty size=\d+ inner=ObjectProperty: (\w+)', t)
        raw = bytes.fromhex(m.group(1)) if m else bytes(4)
        return [var[names[int.from_bytes(raw[4 + 4 * k: 8 + 4 * k], 'little', signed=True) - 1]]
                for k in range(int.from_bytes(raw[:4], 'little'))]
    scs = dump('dumptags.py', base, next(i for i, n in enumerate(names) if n.startswith('SimpleConstructionScript')))
    return {var[n]: objs(t, 'ChildNodes') for n, t in tags.items()}, objs(scs, 'RootNodes'), tags


def comp_attachment():
    """Mesh and Lamp attach to Root in the spawned actor. A cooked SCS's PostLoad runs FixupRootNodeParentReferences
    (not WITH_EDITOR), which clears a root node's ParentComponentOrVariableName unless it is native
    (bIsParentComponentNative) or names an ANCESTOR Blueprint's node via ParentComponentOwnerClassName; so a node
    parented to a sibling in the same SCS must be one of that node's ChildNodes."""
    children, roots, tags = scs_tree(asset('CompTest'))
    assert roots == ['Root'] and children == {'DefaultSceneRoot': [], 'Root': ['Mesh', 'Lamp'], 'Mesh': [], 'Lamp': []}, (roots, children)
    assert not any('ParentComponentOrVariableName' in t for t in tags.values()), tags
    print('ok  CompTest: Mesh and Lamp stay attached to Root after a cooked load')


comp_test()
comp_attachment()


# ---- TypesTest, StringTest, StructTest, PointerTest

def mod_enum():
    import re, struct, subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    base = asset('TypesTest')
    enum = os.path.join(os.path.dirname(base), 'EMood')
    r = dumpexp.load(enum)
    names, raw = r[3], r[1]
    assert [e['name'] for e in r[5]] == ['EMood'] and "Class'UserDefinedEnum'" in r[4], r[4]
    count = struct.unpack_from('<i', raw, 12)[0]
    pairs = [(names[struct.unpack_from('<i', raw, 16 + i * 16)[0]], struct.unpack_from('<q', raw, 24 + i * 16)[0]) for i in range(count)]
    assert pairs == [('EMood::Calm', 0), ('EMood::Angry', 5), ('EMood::Sleepy', 6), ('EMood::EMood_MAX', 7)], pairs
    exports = [e['name'] for e in dumpexp.load(base)[5]]
    cls = subprocess.run([sys.executable, os.path.join(here, 'dumpstruct.py'), base, '0'], capture_output=True, text=True).stdout
    assert re.search(r'ByteProperty Mood .*EMood', cls), cls
    cdo = subprocess.run([sys.executable, os.path.join(here, 'dumptags.py'), base, str(exports.index('Default__TypesTest_C'))], capture_output=True, text=True).stdout
    assert 'EMood::Angry' in cdo, cdo
    print('ok  TypesTest: UE_ENUM cooks EMood with its C++ names and values; the default is its enumerator')
    for enum, pairs_want in (('ESpan', [('ESpan::Tiny', -3), ('ESpan::Wide', 70000), ('ESpan::Huge', 70001), ('ESpan::Vast', 70002), ('ESpan::ESpan_MAX', 70003)]),
                             ('EAge', [('EAge::Epoch', 0), ('EAge::Eon', 5000000000), ('EAge::EAge_MAX', 5000000001)])):
        r = dumpexp.load(os.path.join(os.path.dirname(base), enum))
        names, raw = r[3], r[1]
        count = struct.unpack_from('<i', raw, 12)[0]
        got = [(names[struct.unpack_from('<i', raw, 16 + i * 16)[0]], struct.unpack_from('<q', raw, 24 + i * 16)[0]) for i in range(count)]
        assert got == pairs_want, got
    assert re.search(r'EnumProperty Span .*size=4 .*\n\s+IntProperty UnderlyingType .*size=4', cls), cls
    assert re.search(r'EnumProperty Age .*size=8 .*\n\s+Int64Property UnderlyingType .*size=8', cls), cls
    assert 'ESpan::Wide' in cdo and 'EAge::Eon' in cdo, cdo
    print('ok  TypesTest: int32 / int64 enums cook as EnumProperty over Int / Int64Property')


def fnv(s):
    h = 2166136261
    for c in s.encode(): h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h & 0x7FFFFFFF


def constants():
    import re, subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    base = asset('TypesTest')
    exports = [e['name'] for e in dumpexp.load(base)[5]]
    cdo = subprocess.run([sys.executable, os.path.join(here, 'dumptags.py'), base, str(exports.index('Default__TypesTest_C'))], capture_output=True, text=True).stdout
    for want in ('Seed [0] IntProperty size=4: %d' % fnv('types'), 'Budget [0] IntProperty size=4: 25', 'Reach [0] FloatProperty size=4: 125.0', 'Bits [0] IntProperty size=4: 236'):
        assert want in cdo, (want, cdo)
    print('ok  TypesTest: a member default is what its constant expression comes to')
    import struct
    six, pair = struct.pack('<6f', 1, 2, 3, 4, 5, 6).hex(), struct.pack('<3f', 7, 8, 9).hex()
    points = re.search(r'Points \[0\] ArrayProperty size=77 inner=StructProperty: (\w+)', cdo).group(1)   # count, inner tag (53), 2 x 12 raw
    assert points.startswith('02000000') and points.endswith(six), points
    assert re.search(r'Spots \[0\] MapProperty size=28 .*: 0000000001000000\w{16}' + pair, cdo), cdo
    print('ok  TypesTest: a container default holds struct elements, raw or tagged as the struct serializes')
    names = dumpexp.load(base)[3]
    name_of = lambda hx: names[struct.unpack('<i', bytes.fromhex(hx[:8]))[0]]
    by_mood = re.search(r'MoodNames \[0\] MapProperty size=56 key=ByteProperty value=NameProperty: 0{8}03000000(\w+)', cdo).group(1)
    pairs = [(name_of(by_mood[i:i + 16]), name_of(by_mood[i + 16:i + 32])) for i in range(0, 96, 32)]
    assert pairs == [('EMood::Calm', 'Calm'), ('EMood::Angry', 'Angry'), ('EMood::Sleepy', 'Sleepy')], pairs
    by_name = re.search(r'MoodsByName \[0\] MapProperty size=62 key=StrProperty value=ByteProperty: 0{8}03000000(\w+)', cdo).group(1)
    assert by_name.startswith('05000000' + b'Calm'.hex() + '00') and name_of(by_name[18:34]) == 'EMood::Calm', by_name
    print('ok  TypesTest: UE_ENUM_MAP fills a map from the enum, either way round')
    base = asset('TypesTest')
    exports = [e['name'] for e in dumpexp.load(base)[5]]
    tool = lambda t, i: subprocess.run([sys.executable, os.path.join(here, t), base, str(i)], capture_output=True, text=True).stdout
    assert re.search(r"ObjectProperty Aimed .*Class'Actor'", tool('dumpstruct.py', 0)), 'a `using` alias of a class is still an object reference'
    assert re.search(r"ObjectProperty Spotted .*Class'Pawn'", tool('dumpstruct.py', 0)), 'and so is a class-scope one'
    forget = ' '.join(tool('walkscript.py', exports.index('Forget')).split())
    step = r' \+ *\d+ mem \d+ disk \d+ mem \d+ '
    assert re.search(r'InstanceVariable Health@\S+' + step + 'NoInterface', forget) and re.search(r'InstanceVariable Aimed@\S+' + step + 'NoObject', forget), forget
    print('ok  TypesTest: a class alias stays an object; nullptr is EX_NoInterface for an interface')


def types_behaviour():
    # Cpp20: every enumerator, the values between and past them (no case: `return kSeed`), int32 wrap.
    check('TypesTest', 'Cpp20', lambda M, N: wrap(N + N) if M == 0 else wrap(N + fnv('angry')) if M == 5 else fnv('types'),
          [dict(M=m, N=n) for m in (0, 1, 4, 5, 6, 7, 255) for n in EDGE])
    check('TypesTest', 'ConstSum', lambda N: wrap(N * 3 + 31), [dict(N=n) for n in EDGE])
    check('TypesTest', 'HalfOf', lambda V: V * 0.5, [dict(V=v) for v in (-3.0, 0.0, 8.0, -0.25)])
    check('TypesTest', 'ShrBy', lambda X, M: X >> (M if M in (1, 4) else 31),   # Python >> floors, as C++'s does
          [dict(X=x, M=m) for x in EDGE + (-3, -1, -17) for m in (1, 4, 31)])
    check('TypesTest', 'Shr64', lambda X, M: X >> (1 if M == 1 else 63),
          [dict(X=x, M=m) for x in (-2**63, -3, -1, 0, 5, 2**63 - 1) for m in (1, 63)])
    # An int64 enum compares as int64: a value sharing only Eon's / Epoch's low 32 bits is neither.
    check('TypesTest', 'AgeOf', lambda A: 1 if A == 5000000000 else 2 if A == 0 else 0,
          [dict(A=a) for a in (0, 5000000000, 7, 5000000001, 5000000000 & 0xFFFFFFFF, 1 << 32, -(1 << 32))])
    # Past the switch, `M == Mood ? 10 : 0` / `S == Span ? 10 : 0` read the member.
    for mood in (0, 5, 7):
        for m in (0, 1, 4, 5, 6, 7, 255):
            got = run(asset('TypesTest'), 'MoodScore', self_vars={'Mood': mood}, M=m)[0]
            assert got == {0: 1, 5: 2, 6: 3}.get(m, 10 if m == mood else 0), ('MoodScore', mood, m, got)
    for span in (70000, 5, -3):
        for s in (-4, -3, -2, 5, 69999, 70000, 70001, 70002, 70003):
            got = run(asset('TypesTest'), 'SpanScore', self_vars={'Span': span}, S=s)[0]
            assert got == {-3: 1, 70000: 2, 70001: 3, 70002: 4}.get(s, 10 if s == span else 0), ('SpanScore', span, s, got)
    print('ok  TypesTest.MoodScore / SpanScore: the switch, then the member compare')
    check('TypesTest', 'GetIsTargetable', lambda: True, [dict()])
    for r in (0, 4):
        f = {}
        run(asset('TypesTest'), 'ReceiveEndPlay', self_vars=f, Reason=r)
        assert f == {'LastReason': r}, f
    f = {'Scores': [3]}
    run(asset('TypesTest'), 'HandleScored', self_vars=f, Points=7, By=None)
    assert f == {'Scores': [3, 7]}, f
    f = {'Health': 'H', 'Aimed': 'A'}
    run(asset('TypesTest'), 'Forget', self_vars=f)
    assert f == {'Health': None, 'Aimed': None}, f
    print('ok  TypesTest: ReceiveEndPlay, HandleScored and Forget write their members')
    # The class implements ITargetable, and each interface function it leaves out exists and returns the zero value,
    # so a call through the interface finds the Blueprint function rather than the interface's native one.
    here = os.path.dirname(os.path.abspath(__file__))
    cls = subprocess.run([sys.executable, os.path.join(here, 'dumpstruct.py'), asset('TypesTest'), '0'], capture_output=True, text=True).stdout
    assert re.search(r"""Interfaces \[\("imp\[\d+\]:Class'Targetable'", 0, 1\)\]""", cls), cls
    for fn in ('GetTargetCenterMass', 'GetTargetHealthComponent', 'ShowDamageEffects'):
        assert run(asset('TypesTest'), fn)[0] is None, fn
    print('ok  TypesTest: implements Targetable; the functions it leaves out return zero')


def types_defaults():
    """Every element of the tagged-struct array and both maps the source initialises (replaces the size-only 414)."""
    import struct
    from dumptags import tags
    here, base = os.path.dirname(os.path.abspath(__file__)), asset('TypesTest')
    names, exports = dumpexp.load(base)[3], [e['name'] for e in dumpexp.load(base)[5]]
    cdo = subprocess.run([sys.executable, os.path.join(here, 'dumptags.py'), base, str(exports.index('Default__TypesTest_C'))],
                         capture_output=True, text=True).stdout
    raw = bytes.fromhex(re.search(r'Spans \[0\] ArrayProperty size=\d+ inner=StructProperty: (\w+)', cdo).group(1))
    o, spans = 4 + 49, []                                  # count, then the inner tag (name, type, size, index, struct, guid)
    for _ in range(struct.unpack_from('<i', raw, 0)[0]):
        out = []
        o = tags(raw, o, len(raw), names, 0, out)
        spans.append({l.split()[0]: float(l.split(': ')[1]) for l in out})
    assert spans == [{'Min': 1.0, 'Max': 5.0}, {'Min': -2.0, 'Max': 2.0}], spans
    raw = bytes.fromhex(re.search(r'MoodsByName \[0\] MapProperty .*?: (\w+)', cdo).group(1))
    o, pairs = 8, []
    for _ in range(struct.unpack_from('<i', raw, 4)[0]):
        n = struct.unpack_from('<i', raw, o)[0]
        key = raw[o + 4:o + 3 + n].decode(); o += 4 + n
        pairs.append((key, names[struct.unpack_from('<i', raw, o)[0]])); o += 8
    assert pairs == [('Calm', 'EMood::Calm'), ('Angry', 'EMood::Angry'), ('Sleepy', 'EMood::Sleepy')], pairs
    spots = bytes.fromhex(re.search(r'Spots \[0\] MapProperty .*?: (\w+)', cdo).group(1))
    assert names[struct.unpack_from('<i', spots, 8)[0]].lower() == 'home' and struct.unpack_from('<3f', spots, 16) == (7, 8, 9)
    print('ok  TypesTest: Spans, MoodsByName and Spots defaults hold every element')


def string_behaviour():
    import runscript
    check('StringTest', 'MakeKey', lambda Prefix, Index: Prefix + '_' + str(Index),
          [dict(Prefix=p, Index=i) for p in ('', 'Abc') for i in (-5, 0, 42, 2**31 - 1)])

    class Trace(dict):
        """The object's fields, recording every store in order."""
        def __init__(s): super().__init__(); s.log = []
        def __setitem__(s, k, v): s.log.append((k, v)); super().__setitem__(k, v)

    f = Trace()
    runscript.MESSAGES.clear()
    run(asset('StringTest'), 'ReceiveBeginPlay', self_vars=f)
    k3, tail = 'Kills: 3, ratio 0.5', 'plain nameplain text1234567890123' + '1234567890123_7' + 'false'
    assert f.log == [('Count', 3), ('Ratio', 0.5), ('Label', k3), ('Key', k3 + '_key'), ('Caption', k3 + '_key'),
                     ('Caption', k3 + (k3 + '_key') * 2), ('Label', 'plain nameplain text'), ('Big', 1234567890123),
                     ('Caption', '1234567890123'), ('Label', 'plain nameplain text1234567890123'),
                     ('Count', 0), ('Ratio', 0.0), ('Big', 0), ('Count', 7), ('Key', '1234567890123_7'), ('Label', tail),
                     ('Label', 'Kills: 7'), ('Label', '7 left')], f.log
    assert runscript.MESSAGES == [tail + ' (Self)'], runscript.MESSAGES
    print('ok  StringTest.ReceiveBeginPlay: every conversion and concat; "lit" + N and N + "lit" concatenate')


def base_names(v):
    """A UserDefinedStruct value with the cooked GUID suffix off each member: Kills_624902_<guid> -> Kills."""
    return {re.sub(r'_\d+_[0-9A-F]{32}$', '', k): base_names(x) for k, x in v.items()} if isinstance(v, dict) else v


def struct_behaviour():
    import runscript
    kills = next(n for n in dumpexp.load(os.path.join(os.path.dirname(asset('StructTest')), 'FStats'))[3] if n.startswith('Kills_'))
    check('StructTest', 'KillsOf', lambda S: S.get(kills, 0), [dict(S=s) for s in ({}, {kills: 5}, {kills: -1})])
    check('StructTest', 'MakeLocal', lambda K: wrap(K + wrap(K * 2) * 10 + 1000), [dict(K=k) for k in (0, 3, -2, 2**30)])
    check('StructTest', 'MakeArgument', lambda K: wrap(K + wrap(K + 1)), [dict(K=k) for k in (0, 5, -1, 2**31 - 1)])
    check('StructTest', 'MakeInLoop', lambda Rounds: max(Rounds, 0), [dict(Rounds=r) for r in (-3, 0, 1, 2, 4)])
    f = {}
    runscript.MESSAGES.clear()
    run(asset('StructTest'), 'ReceiveBeginPlay', self_vars=f)
    # `Stats = Local` replaces the whole struct: Alive and Owner are false / null again, so no message is posted.
    assert base_names(f) == {'Stats': {'Kills': 4, 'Time': 1.5}, 'Nested': {'Inner': {'Kills': 8, 'Time': 1.5}},
                             'Moody': {'Mood': 0, 'Level': 2}}, base_names(f)
    assert runscript.MESSAGES == [], runscript.MESSAGES
    print('ok  StructTest.ReceiveBeginPlay: member stores, whole-struct copies, nested members')


def pointer_behaviour():
    import runscript
    check('PointerTest', 'Advance', lambda Base, Count: Base + 4 * Count,
          [dict(Base=b, Count=c) for b in (0, 0x7FF600001000) for c in (-2, 0, 1, 2**31 - 1)])
    for v in (-1, 0, 41, 2**31 - 1):
        assert run(asset('PointerTest'), 'Bump', Value=v)[1]['Value'] == wrap(v + 1), v
    for ok in (True, False):
        f = {'Failures': 2}
        runscript.MESSAGES.clear()
        run(asset('PointerTest'), 'Check', self_vars=f, bOk=ok, What='x')
        assert f == {'Failures': 2 if ok else 3} and runscript.MESSAGES == ([] if ok else ['PointerTest FAILED: x']), (ok, f)
    # The synthesized read scratch is cooked beside the class, and the class imports it.
    d = dumpexp.load(os.path.join(os.path.dirname(asset('PointerTest')), 'FDeref'))
    assert [e['name'] for e in d[5]] == ['FDeref'] and "Class'UserDefinedStruct'" in d[4], d[4]
    assert "UserDefinedStruct'FDeref'" in dumpexp.load(asset('PointerTest'))[4]
    print('ok  PointerTest: Advance, Bump (out-parm), Check; FDeref synthesized')


mod_enum()
constants()
types_behaviour()
types_defaults()
string_behaviour()
struct_behaviour()
check('StructTest', 'MakeNative', lambda D: D + 0.5, [dict(D=d) for d in (0.0, 4.0)])
pointer_behaviour()


# ---- IfaceTest, OverrideTest, SuperTest, NameTest, AssetTest

def interfaces():
    """IfaceTest: what the engine reads of a mod interface and of the classes that implement one."""
    folder = os.path.dirname(asset('IfaceTest'))
    base = lambda a: os.path.join(folder, a)
    IFACE = '/Game/_ElytrasMods/IfaceTest/%s.%s_C'

    def cls(a):
        out, paths = dump('dumpstruct.py', base(a), 0), import_paths(base(a))
        sup = re.search(r'SuperStruct imp\[(\d+)\]', out)
        return dict(flags=int(re.search(r'ClassFlags (\S+)', out).group(1), 16),
                    funcs=set(re.findall(r"\('([^']+)', 'exp\[\d+\]", re.search(r'FuncMap (.*)', out).group(1))),
                    props={n: t for t, n in re.findall(r'^  (\w+Property) (\w+) ', out, re.M)},
                    ifaces=[(paths[int(i)], int(off), int(k2)) for i, off, k2 in
                            re.findall(r"imp\[(\d+)\][^,]*, (-?\d+), (\d+)\)", re.search(r'Interfaces (.*)', out).group(1))],
                    super=paths[int(sup.group(1))] if sup else None, text=out)

    # CLASS_Interface (0x4000) over UInterface, or over the interface it extends; one function per declared method,
    # no state - IMarkable's variables are properties of its implementers.
    t, m = cls('ITargetable'), cls('IMarkable')
    assert t['flags'] & 0x4000 and t['super'] == '/Script/CoreUObject.Interface', t['text']
    assert m['flags'] & 0x4000 and m['super'] == IFACE % ('ITargetable', 'ITargetable'), m['text']
    assert t['funcs'] == {'GetPriority', 'OnTargeted'} and m['funcs'] == {'Mark'}, (t['funcs'], m['funcs'])
    assert not t['props'] and not m['props'], (t['props'], m['props'])
    print('ok  IfaceTest: ITargetable / IMarkable cook as interface classes, IMarkable extending ITargetable')
    # An implementer lists its interface with bImplementedByK2 and has a function of every name in the interface's
    # chain: an interface call finds its target by name (FindFunctionChecked), so a missing one is fatal.
    chain = {'ITargetable': {'GetPriority', 'OnTargeted'}, 'IMarkable': {'GetPriority', 'OnTargeted', 'Mark'},
             'CurveSourceInterface': {'GetBindingName', 'GetCurveValue', 'GetCurves'}}
    for a, iface in (('Turret', 'ITargetable'), ('Describe', 'ITargetable'), ('Beacon', 'IMarkable'),
                     ('Singer', 'CurveSourceInterface'), ('Hummer', 'CurveSourceInterface')):
        c = cls(a)
        path = '/Script/Engine.CurveSourceInterface' if iface == 'CurveSourceInterface' else IFACE % (iface, iface)
        assert c['ifaces'] == [(path, 0, 1)] and chain[iface] <= c['funcs'], (a, c['ifaces'], c['funcs'])
    lb, b = cls('LoudBeacon'), cls('Beacon')      # LoudBeacon inherits Beacon's: no entry, no Marks shadowing Beacon's
    assert lb['ifaces'] == [] and lb['super'] == '/Game/_ElytrasMods/IfaceTest/Beacon.Beacon_C' and 'Marks' not in lb['props'], lb['text']
    assert b['props'].get('Marks') == 'IntProperty' and b['props'].get('MarkedBy') == 'ObjectProperty', b['props']
    for a, want in (('Beacon', 12), ('LoudBeacon', 40)):
        cdo = dump('dumptags.py', base(a), exports_of(base(a)).index('Default__%s_C' % a))
        assert 'Marks [0] IntProperty size=4: %d' % want in cdo, (a, cdo)
    print('ok  IfaceTest: implementers list their interface, define its whole chain, and hold its variables')
    # A native interface function is implemented by a script function (no FUNC_Native 0x400) with the native's
    # inherited BlueprintEvent 0x8000000 | Const 0x40000000.
    for a in ('Singer', 'Hummer'):
        for fn in ('GetBindingName', 'GetCurveValue', 'GetCurves'):
            flags = int(re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base(a), exports_of(base(a)).index(fn))).group(1), 16)
            assert flags & 0x48000000 == 0x48000000 and not flags & 0x400, (a, fn, hex(flags))
    print('ok  IfaceTest: a native interface function is implemented by a script function with its flags')


def interface_bodies():
    """IfaceTest: implementations and stubs, run offline - return values and the fields they write."""
    b = lambda a: os.path.join(os.path.dirname(asset('IfaceTest')), a)
    zero = (None, 0)                     # a bare return: the caller reads the frame's zeroed ReturnValue
    assert run(b('Turret'), 'GetPriority')[0] == 7
    for a, fn, parms, before, after in (('Turret', 'OnTargeted', {'By': 'x'}, {'Hits': 5}, {'Hits': 6}),
                                        ('Beacon', 'Mark', {'Count': 4}, {'Marks': 12}, {'Marks': 16}),
                                        ('Beacon', 'OnTargeted', {'By': 'x'}, {'Marks': 12}, {'Marks': 12, 'MarkedBy': 'x'}),
                                        ('Describe', 'OnTargeted', {'By': 'x'}, {}, {})):
        run(b(a), fn, self_vars=before, **parms)
        assert before == after, (a, fn, before)
    for a in ('Beacon', 'Describe'):     # GetPriority left out: an empty stub
        f = {}
        assert run(b(a), 'GetPriority', self_vars=f)[0] in zero and f == {}, a
    assert run(b('Singer'), 'GetBindingName')[0] == 'Singer' and run(b('Hummer'), 'GetBindingName')[0] == 'Hummer'
    assert run(b('Singer'), 'GetCurveValue', CurveName='x')[0] == 0.5 and run(b('Hummer'), 'GetCurveValue', CurveName='x')[0] in zero
    for a in ('Singer', 'Hummer'):
        ret, env = run(b(a), 'GetCurves', OutValues=['kept'])
        assert ret is None and env['OutValues'] == ['kept'], (a, env)
    print('ok  IfaceTest: implementations and stubs return and write what the C++ says')


def interface_calls():
    """IfaceTest: a call through an interface value goes by name to the object behind it; the cast names the
    interface class; LoudBeacon reads the Marks Beacon holds, on itself and on Other."""
    folder = os.path.dirname(asset('IfaceTest'))
    for a, fn in (('Spotter', 'Rate'), ('Beacon', 'PriorityOf')):
        base = os.path.join(folder, a)
        paths, w = import_paths(base), dump('walkscript.py', base, exports_of(base).index(fn))
        casts = re.findall(r'ObjToInterfaceCast\s+imp\[(\d+)\]', w)
        assert casts and {paths[int(i)] for i in casts} == {'/Game/_ElytrasMods/IfaceTest/ITargetable.ITargetable_C'}, (a, w)
        assert 'InterfaceContext' in w and re.search(r'VirtualFunction\s+GetPriority\b', w), (a, w)
        assert not re.search(r"FinalFunction\s+imp\[\d+\]:Function'GetPriority'", w), (a, w)
    base = os.path.join(folder, 'LoudBeacon')
    w = dump('walkscript.py', base, exports_of(base).index('Total'))
    reads = re.findall(r"InstanceVariable\s+Marks@imp\[(\d+)\]", w)
    assert len(reads) == 2 and {import_paths(base)[int(i)] for i in reads} == {'/Game/_ElytrasMods/IfaceTest/Beacon.Beacon_C'}, w
    assert re.search(r'Context\s+skip \d+ Marks@[^\n]*\n[^\n]*LocalVariable\s+Other@', w) and 'Add_IntInt' in w, w
    print('ok  IfaceTest: interface calls go by name; the cast names ITargetable; Marks is read where Beacon holds it')


def inherited_defaults():
    """OverrideTest: UE_DEFAULTS on an inherited SCS component (a handler record on the parent's node), on a native
    parent's component (its default subobject) and on an inherited plain property (a tag on this CDO)."""
    import dumptags
    folder = os.path.dirname(asset('OverrideTest'))
    base = lambda a: os.path.join(folder, a)

    def objects(a):
        """export name -> (class path, template path, outer name, tags)."""
        exports = dumpexp.load(base(a))[5]
        name = lambda v: ref(base(a), v) if v else None
        return {e['name']: (name(e['cls']), name(e['tmpl']), name(e['outer']), dump('dumptags.py', base(a), i))
                for i, e in enumerate(exports)}

    bp = objects('BaseProp')
    assert 'Intensity [0] FloatProperty size=4: 1000.0' in bp['Lamp_GEN_VARIABLE'][3], bp['Lamp_GEN_VARIABLE']
    nodes = {re.search(r'InternalVariableName \[0\] NameProperty size=8: (\w+)', t).group(1): t
             for c, _, _, t in bp.values() if c == '/Script/Engine.SCS_Node'}
    children, roots, _ = scs_tree(base('BaseProp'))
    assert roots == ['Root'] and children == {'DefaultSceneRoot': [], 'Root': ['Lamp'], 'Lamp': []}, (roots, children)
    lamp_guid = re.search(r'VariableGuid \[0\] StructProperty size=16 struct=Guid: (\w+)', nodes['Lamp']).group(1)
    # DimProp: one record keyed on (BaseProp_C, Lamp's node guid) - FComponentKey::Match compares exactly those -
    # whose template, archetyped on BaseProp's Lamp_GEN_VARIABLE, holds Intensity 250.
    dp = objects('DimProp')
    ua, ue, total, names, imports, exports = dumpexp.load(base('DimProp'))
    h = next(e for e in exports if e['name'].startswith('InheritableComponentHandler'))
    blob = ue[h['off'] - total: h['off'] - total + h['size']]
    records = re.search(r'Records \[0\] ArrayProperty size=\d+ inner=StructProperty: (\w+)', dp[h['name']][3]).group(1)
    assert records.startswith('01000000'), records
    out = []
    dumptags.tags(blob, blob.find(bytes.fromhex(records)) + 4, len(blob), names, 1, out)
    rec = '\n'.join(out)
    idx = lambda f: int(re.search(r'%s \[0\] ObjectProperty size=4: index (-?\d+)' % f, rec).group(1))
    assert ref(base('DimProp'), idx('OwnerClass')) == '/Game/_ElytrasMods/OverrideTest/BaseProp.BaseProp_C', rec
    assert 'SCSVariableName [0] NameProperty size=8: Lamp' in rec and 'struct=Guid: ' + lamp_guid in rec, rec
    c, tmpl, _, tags = dp[ref(base('DimProp'), idx('ComponentTemplate'))]
    assert c == '/Script/Engine.PointLightComponent' and tmpl == '/Game/_ElytrasMods/OverrideTest/BaseProp.BaseProp_C:Lamp_GEN_VARIABLE', (c, tmpl)
    assert 'Intensity [0] FloatProperty size=4: 250.0' in tags, tags
    # InitialLifeSpan: a tag on the CDO, and no property of DimProp_C's own shadowing AActor's.
    assert 'InitialLifeSpan [0] FloatProperty size=4: 3.0' in dp['Default__DimProp_C'][3], dp['Default__DimProp_C']
    assert not re.search(r'^  \w+Property ', dump('dumpstruct.py', base('DimProp'), 0), re.M), 'DimProp_C re-declares a property'
    print('ok  OverrideTest: an inherited SCS component is a handler record on the parent node; a plain property a CDO tag')
    # Walker: ACharacter's components are Default__Character's subobjects CollisionCylinder and CharacterMesh0
    # ([V] GObjects dump; [S] Character.cpp:25-27). The override must be an export OF THAT NAME under this CDO -
    # FLinkerLoad::CreateExport (LinkerLoad.cpp:4690) looks the name up, else constructs a new, unused component.
    wk = objects('Walker')
    for sub, cls_path, want in (('CollisionCylinder', '/Script/Engine.CapsuleComponent', 'CapsuleRadius [0] FloatProperty size=4: 55.0'),
                                ('CharacterMesh0', '/Script/Engine.SkeletalMeshComponent', 'bVisible [0] BoolProperty size=0 value=0')):
        assert sub in wk, (sub, sorted(wk))
        c, _, outer, tags = wk[sub]
        assert c == cls_path and outer == 'Default__Walker_C' and want in tags, wk[sub]
    print('ok  OverrideTest: a native parent\'s component is overridden under its subobject name')


def run_as(chain, fn, fields, **parms):
    """Runs fn on an object of class chain[0] whose mod ancestors are chain[1:] (package bases) as the VM
    dispatches: a call by name runs the most derived definition, EX_FinalFunction exactly the function its import
    names. (runscript alone looks both up in the calling package.)"""
    import runscript
    names = {b: exports_of(b) for b in chain}
    owner = lambda f: next(b for b in chain if f in names[b])
    finals = {}
    for b in chain:
        paths = import_paths(b)
        for i in range(len(names[b])):
            for imp in re.findall(r'FinalFunction\s+imp\[(\d+)\]', dump('walkscript.py', b, i)):
                pkg, _, fname = paths[int(imp)].rpartition(':')
                if pkg.startswith('/Game/'):                        # a mod function, in a package beside this one
                    finals[fname] = os.path.join(os.path.dirname(chain[0]), pkg.split('.')[0].rsplit('/', 1)[1])
    saved = runscript.run, runscript.params_of, dict(runscript.MATH)
    runscript.run = lambda base, f, self_vars=None, **p: saved[0](owner(f), f, self_vars, **p)
    runscript.params_of = lambda base, f: saved[1](owner(f), f)
    for f, target in finals.items():
        runscript.MATH[f] = (lambda t, f: lambda *a: saved[0](t, f, fields, **dict(zip(saved[1](t, f), a)))[0])(target, f)
    try:
        return saved[0](owner(fn), fn, fields, **parms)[0]
    finally:
        runscript.run, runscript.params_of = saved[0], saved[1]
        runscript.MATH.clear(); runscript.MATH.update(saved[2])


def parent_call():
    """SuperTest: `SuperBase::X()` in an override runs the parent's X once (final on SuperBase_C's function - by name
    it would re-enter the override, forever); an unqualified inherited call still dispatches to the most derived."""
    folder = os.path.dirname(asset('SuperTest'))
    test, base = os.path.join(folder, 'SuperTest'), os.path.join(folder, 'SuperBase')

    class Base:                                   # SuperTest.cpp, as Python
        def __init__(s, Count): s.Count = Count
        def ReceiveBeginPlay(s): s.Count = 1
        def Bump(s, By): s.Count += By; return s.Count
        def Twice(s, By): return s.Bump(By) + s.Bump(By)

    class Test(Base):
        def ReceiveBeginPlay(s): Base.ReceiveBeginPlay(s); s.Count += 10
        def Bump(s, By): return Base.Bump(s, By * 2)
        def Thrice(s, By): return s.Twice(By) + s.Bump(By)

    n = 0
    for chain, model in (([base], Base), ([test, base], Test)):
        for fn, args in (('ReceiveBeginPlay', {}), ('Bump', {'By': 3}), ('Twice', {'By': 2}), ('Thrice', {'By': 1})):
            if not hasattr(model, fn): continue
            for count in (0, 5):
                fields, obj = {'Count': count}, model(count)
                got, want = run_as(chain, fn, fields, **args), getattr(obj, fn)(**args)
                assert (got in (None, 0) if want is None else got == want) and fields['Count'] == obj.Count, (chain[0], fn, count, got, want, fields)
                n += 1
    print('ok  SuperTest: parent calls, overrides and inherited calls run as in C++  (%d cases)' % n)
    paths = import_paths(test)
    for fn in ('Bump', 'ReceiveBeginPlay'):
        w = dump('walkscript.py', test, exports_of(test).index(fn))
        assert '/Game/_ElytrasMods/SuperTest/SuperBase.SuperBase_C:' + fn in [paths[int(i)] for i in re.findall(r'FinalFunction\s+imp\[(\d+)\]', w)], w
        assert not re.search(r'VirtualFunction\s+%s\b' % fn, w), w
    w = dump('walkscript.py', test, exports_of(test).index('Thrice'))
    assert re.search(r'VirtualFunction\s+Twice\b', w) and re.search(r'VirtualFunction\s+Bump\b', w), w
    # An override names the parent's UFunction as its super and keeps its flags; a new function has no super.
    for b, fn, parent in ((test, 'Bump', '/Game/_ElytrasMods/SuperTest/SuperBase.SuperBase_C:Bump'),
                          (test, 'ReceiveBeginPlay', '/Game/_ElytrasMods/SuperTest/SuperBase.SuperBase_C:ReceiveBeginPlay'),
                          (base, 'ReceiveBeginPlay', '/Script/Engine.Actor:ReceiveBeginPlay'),
                          (test, 'Thrice', None), (base, 'Bump', None), (base, 'Twice', None)):
        e = dumpexp.load(b)[5][exports_of(b).index(fn)]
        assert (ref(b, e['super']) if e['super'] else None) == parent, (b, fn, e['super'])
    flags = lambda b, fn: re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', b, exports_of(b).index(fn))).group(1)
    assert flags(test, 'Bump') == flags(base, 'Bump') and flags(test, 'ReceiveBeginPlay') == flags(base, 'ReceiveBeginPlay') == '0x8080800'
    print('ok  SuperTest: Base::Method() is a final call on the parent\'s function; overrides bind to it')


def engine_names():
    """NameTest: members Dumper-7 respelled, run and cooked under the engine's names; access, purity, const."""
    base = asset('NameTest')
    loaded = dumpexp.load(base)
    exports = [e['name'] for e in loaded[5]]
    cdo = dump('dumptags.py', base, exports.index('Default__NameTest_C'))
    assert 'Index [0] IntProperty size=4: 7' in cdo and re.search(r"Name \[0\] StrProperty size=\d+: 'Karl'", cdo), cdo
    # SplitName would have made FName(Index, 1) of the C++ spelling: neither it nor the spelling may be in the package.
    assert 'Index_0' not in loaded[3] and 'Name_0' not in loaded[3], [n for n in loaded[3] if n.endswith('_0')]
    f = {'Index': 7}                 # runscript keys a field by the name it is cooked under
    assert run(base, 'Next', self_vars=f)[0] == 8 and f == {'Index': 8}, f
    assert run(base, 'Peek', self_vars={'Index': 41})[0] == 41
    owners = {import_paths(base)[int(i)] for i in re.findall(r'InstanceVariable\s+Index@imp\[(\d+)\]', dump('walkscript.py', base, exports.index('Next')))}
    assert owners == {'/Script/FSD.FSDSaveGame'}, owners
    print('ok  NameTest: a member Dumper-7 respelled is cooked and run by the engine\'s name (tag and bytecode)')
    assert run(base, 'Step')[0] == 1 and run(base, 'Twice')[0] == 2
    # FUNC_Public 0x20000 / Private 0x40000 / Protected 0x80000 as the C++ says; UE_PURE = BlueprintPure 0x10000000.
    fflags = lambda fn: int(re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base, exports.index(fn))).group(1), 16)
    for fn, want in (('Next', 0x20000), ('Peek', 0x20000), ('SameKind', 0x20000), ('Whose', 0x20000), ('Step', 0x80000), ('Twice', 0x40000)):
        assert fflags(fn) & 0xE0000 == want and bool(fflags(fn) & 0x10000000) == (fn == 'Peek'), (fn, hex(fflags(fn)))
    print('ok  NameTest: a function carries its C++ access specifier, and UE_PURE')
    props = dump('dumpstruct.py', base, exports.index('NameTest_C'))
    pflags = lambda name: int(re.search(r'Property %s .*? flags=(\S+)' % name, props).group(1), 16)
    assert pflags('Limit') & 0x10 and not any(pflags(p) & 0x10 for p in ('Seed', 'Charges', 'Plain')), props
    assert 'Limit [0] IntProperty size=4: 3' in cdo, cdo
    print('ok  NameTest: a const member is BlueprintReadOnly')


def object_forwards():
    """NameTest: UObject's C++ helpers. The engine has no GetOuter / GetClass / GetName UFunction on UObject, so none
    is imported or called by name; GetClass / GetName are the Kismet statics on the object, a class's own GetName
    (UFSDSaveGame's) wins, and GetOuter reads OuterPrivate at the object + 0x20."""
    base = asset('NameTest')
    exports, paths = exports_of(base), import_paths(base)
    assert not any(p.endswith(('Object:GetOuter', 'Object:GetClass', 'Object:GetName')) for p in paths), paths
    walk = lambda fn: dump('walkscript.py', base, exports.index(fn))
    calls = lambda fn: [paths[int(i)] for i in re.findall(r'(?:FinalFunction|CallMath)\s+imp\[(\d+)\]', walk(fn))]
    for fn in ('Whose', 'SameKind'):
        assert not re.search(r'VirtualFunction\s+(GetOuter|GetClass|GetName)\b', walk(fn)), walk(fn)
    assert calls('Whose').count('/Script/Engine.KismetSystemLibrary:GetObjectName') == 1 and '/Script/FSD.FSDSaveGame:GetName' in calls('Whose'), calls('Whose')
    assert calls('SameKind').count('/Script/Engine.GameplayStatics:GetObjectClass') == 2, calls('SameKind')
    w = walk('SameKind')
    assert re.search(r"GetObjectClass'\s*\n[^\n]*LocalVariable\s+Other@", w) and re.search(r"GetObjectClass'\s*\n[^\n]*Self", w), w
    ua, ue, total, names, imports, exps = dumpexp.load(base)
    e = exps[exports.index('Whose')]
    assert b'\x35' + (0x20).to_bytes(8, 'little') in ue[e['off'] - total: e['off'] - total + e['size']], 'Whose reads no OuterPrivate (+0x20)'
    print('ok  NameTest: GetOuter / GetClass / GetName reach engine functions or OuterPrivate, never a missing UFunction')


def api_stub():
    """The editor API stub (--api) of NameTest: what a Blueprint author sees of a mod class."""
    import struct, tempfile, dumptags
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, 'cooked'))
        os.makedirs(os.path.join(tmp, 'api'))
        proc = subprocess.run([ASSETGEN, 'compile', os.path.join(TESTS, 'NameTest.cpp'), UEAPI,
                               os.path.join(tmp, 'cooked'), '--api', os.path.join(tmp, 'api')], capture_output=True, text=True)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        base = os.path.join(tmp, 'api', 'NameTest')
        ua, ue, total, names, imports, exports = dumpexp.load(base)
        blob = ue[exports[0]['off'] - total: exports[0]['off'] - total + exports[0]['size']]
        out = []
        dumptags.tags(blob, 0, len(blob), names, 1, out)
        # NewVariables: count, the inner StructProperty tag (49 bytes), then one tagged BPVariableDescription each.
        at = blob.find(bytes.fromhex(re.search(r'NewVariables .*: (\w+)', '\n'.join(out)).group(1)))
        p, variables = at + 4 + 49, {}
        for _ in range(struct.unpack_from('<i', blob, at)[0]):
            v = []
            p = dumptags.tags(blob, p, len(blob), names, 0, v)
            variables[re.search(r'VarName \[0\] NameProperty size=8: (\w+)', '\n'.join(v)).group(1)] = '\n'.join(v)
        entries = {exports[e['outer'] - 1]['name']: dump('dumptags.py', base, i)
                   for i, e in enumerate(exports) if e['name'].startswith('K2Node_FunctionEntry')}
    category = b'Names|Test'.hex()
    # A private field is left out; the const one is offered read-only (CPF_BlueprintReadOnly 0x10).
    assert set(variables) == {'Charges', 'Plain', 'Limit'}, set(variables)
    rflags = lambda v: struct.unpack('<Q', bytes.fromhex(re.search(r'PropertyFlags \[0\] UInt64Property size=8: (\w+)', variables[v]).group(1)))[0]
    assert rflags('Limit') & 0x10 and not rflags('Charges') & 0x10 and not rflags('Plain') & 0x10
    # UE_CATEGORY files Charges and Peek; Plain follows UE_CATEGORY("") and has none.
    assert category in variables['Charges'] and 'Category' not in variables['Plain'], variables
    assert category in entries['Peek'] and not any(category in entries[f] for f in entries if f != 'Peek'), entries
    # Every method is offered, its entry node carrying its access specifier and purity (ExtraFlags).
    assert set(entries) == {'Next', 'Peek', 'SameKind', 'Step', 'Twice', 'Whose'}, set(entries)
    extra = lambda f: int(re.search(r'ExtraFlags \[0\] IntProperty size=4: (-?\d+)', entries[f]).group(1))
    for f in entries:
        assert extra(f) & 0xE0000 == {'Step': 0x80000, 'Twice': 0x40000}.get(f, 0x20000), (f, hex(extra(f)))
        assert bool(extra(f) & 0x10000000) == (f == 'Peek'), (f, hex(extra(f)))
    print('ok  NameTest: the API stub carries UE_CATEGORY, access and purity, and leaves a private field out')


def static_assets():
    """AssetTest: namespace-scope objects cook as assets of their class holding exactly the members their braces
    name; `&Asset` anywhere is a reference to that asset's package path."""
    import struct
    folder = os.path.dirname(asset('AssetTest'))
    base = lambda a: os.path.join(folder, a)
    MOD = '/Game/_ElytrasMods/AssetTest/'

    def tags_of(a, i=0):
        return {m.group(1): m.group(2) for m in re.finditer(r'^  (\w+) \[0\] (\w+ size=\d+[^:]*: ?.*)$', dump('dumptags.py', base(a), i), re.M)}

    def objs(a, hexs):
        """An ObjectProperty array payload (count, then one FPackageIndex each), as paths."""
        raw = bytes.fromhex(hexs)
        return [ref(base(a), v) for v in struct.unpack_from('<%di' % struct.unpack_from('<i', raw)[0], raw, 4)]

    for a, cls in (('MD_Plain', MOD + 'UMoodDef.UMoodDef_C'), ('MD_Calm', MOD + 'UMoodDef.UMoodDef_C'),
                   ('MD_Big', MOD + 'UMoodDef.UMoodDef_C'), ('ED_AssetTest', '/Script/FSD.EnemyDescriptor')):
        e = dumpexp.load(base(a))[5]
        assert [x['name'] for x in e] == [a] and ref(base(a), e[0]['cls']) == cls and e[0]['flags'] & 0x3 == 0x3, (a, e)   # RF_Public | RF_Standalone
    # Only the named members are written, the rest stay the class defaults; an explicit zero is written.
    assert tags_of('MD_Plain') == {}, tags_of('MD_Plain')
    assert tags_of('MD_Calm') == {'Count': 'IntProperty size=4: 0', 'Mood': 'ByteProperty size=8 enum=EMood: EMood::Calm'}, tags_of('MD_Calm')
    big = tags_of('MD_Big')
    assert set(big) == {'Health', 'Title', 'Tag', 'bBig', 'Next', 'Waves'}, big
    assert big['Health'].endswith(': -500.5') and big['Title'].endswith(": 'Big'") and big['Tag'].endswith(': big') and 'value=1' in big['bBig'], big
    assert ref(base('MD_Big'), int(big['Next'].split()[-1])) == MOD + 'MD_Calm.MD_Calm', big['Next']
    assert big['Waves'].endswith(struct.pack('<4i', 3, 3, 5, 8).hex()), big['Waves']
    cdo = dump('dumptags.py', base('UMoodDef'), exports_of(base('UMoodDef')).index('Default__UMoodDef_C'))
    for want in ("Title [0] StrProperty size=9: 'Base'", 'Health [0] FloatProperty size=4: 100.0', 'Count [0] IntProperty size=4: 3',
                 'Mood [0] ByteProperty size=8 enum=EMood: EMood::Angry'):
        assert want in cdo, (want, cdo)
    ed = tags_of('ED_AssetTest')
    assert objs('ED_AssetTest', ed['VeteranClasses'].split()[-1]) == ['/Game/Enemies/Spider/Grunt/ED_Spider_Grunt.ED_Spider_Grunt'], ed
    assert ed['SpawnSpread'].endswith(': 250.0') and ed['IdealSpawnSize'].endswith(': 4') and 'value=1' in ed['CanBeUsedForConstantPressure'], ed
    print('ok  AssetTest: each declared object is an asset of its class with the members its braces name')
    # AssetUser's defaults: a pointer, an array, a set and two maps, object elements by package path.
    user = base('AssetUser')
    names = dumpexp.load(user)[3]
    cdo = {k: bytes.fromhex(v.split()[-1]) if k != 'Picked' else int(v.split()[-1]) for k, v in tags_of('AssetUser', exports_of(user).index('Default__AssetUser_C')).items()}
    assert ref(user, cdo['Picked']) == MOD + 'MD_Big.MD_Big', cdo
    assert objs('AssetUser', cdo['Enemies'].hex()) == ['/Game/Enemies/Spider/Grunt/ED_Spider_Grunt.ED_Spider_Grunt', MOD + 'ED_AssetTest.ED_AssetTest'], cdo
    # One name in two namespaces is two assets; a Package.Object path imports that object from that package.
    assert objs('AssetUser', cdo['Picks'].hex()) == [
        '/Game/Enemies/Spider/Grunt/ED_Spider_Grunt.ED_Spider_Grunt', '/Game/Enemies/Spider/Exploder/ED_Spider_Exploder.ED_Spider_Exploder',
        '/Game/Art/Environments/Holiday_GreatEggHunt/SK_greatEggHunt_bunnyPlush.SK_GreatEggHunt_BunnyPlush'], cdo
    tags = struct.unpack_from('<6i', cdo['Tags'])                                    # removed, count, (FName) x 2
    assert tags[:2] == (0, 2) and [names[tags[2]], names[tags[4]]] == ['big', 'calm'], tags
    m = struct.unpack_from('<8i', cdo['ByName'])                                     # removed, count, (FName, object) x 2
    assert m[:2] == (0, 2) and [(names[m[2]], ref(user, m[4])), (names[m[5]], ref(user, m[7]))] == [('big', MOD + 'MD_Big.MD_Big'), ('calm', MOD + 'MD_Calm.MD_Calm')], m
    assert struct.unpack_from('<3ifif', cdo['Scale']) == (0, 2, 1, 0.5, 2, -2.0), cdo
    print('ok  AssetTest: member defaults reference the assets, in containers too')
    # ReceiveBeginPlay posts "Picked <Picked->Title>, calm count <MD_Calm->Count>" through the game state; Say is
    # inline, so no UFunction of that name.
    exports, paths = exports_of(user), import_paths(user)
    assert 'Say' not in exports, exports
    w = dump('walkscript.py', user, exports.index('ReceiveBeginPlay'))
    used = {paths[int(i)] for i in re.findall(r'imp\[(\d+)\]', w)}
    for p in ('/Script/FSD.GameFunctionLibrary:GetFSDGameState', '/Script/FSD.FSDGameState:PostGameMessage',
              MOD + 'MD_Calm.MD_Calm', '/Script/Engine.KismetStringLibrary:Conv_IntToString'):
        assert p in used, (p, used)
    for want in (r"StringConst\s+'Picked '", r"StringConst\s+', calm count '", r'InstanceVariable\s+Picked@', r'InstanceVariable\s+Title@',
                 r'InstanceVariable\s+Count@'):
        assert re.search(want, w), (want, w)
    print('ok  AssetTest: a function body reaches an asset by reference')
    ar = subprocess.run([sys.executable, os.path.join(HERE, 'dumpar.py'), registry_of('AssetTest')], capture_output=True, text=True).stdout
    assert set(re.findall(r'^\s+(/Game/\S+)\s+(\S+)$', ar, re.M)) == {
        (MOD + 'AssetUser.AssetUser_C', 'BlueprintGeneratedClass'), (MOD + 'UMoodDef.UMoodDef_C', 'BlueprintGeneratedClass'),
        (MOD + 'EMood.EMood', 'UserDefinedEnum'), (MOD + 'MD_Plain.MD_Plain', 'UMoodDef_C'), (MOD + 'MD_Calm.MD_Calm', 'UMoodDef_C'),
        (MOD + 'MD_Big.MD_Big', 'UMoodDef_C'), (MOD + 'ED_AssetTest.ED_AssetTest', 'EnemyDescriptor')}, ar
    print('ok  AssetTest: the asset registry lists every asset with its class')


def ue_assets():
    """genueassets names each asset a registry lists by its content path, one header per class; a mod reaching one
    through that header imports exactly that object. The registry here is AssetTest's, the game's in real use."""
    import tempfile
    with tempfile.TemporaryDirectory(dir=TESTS) as tmp:
        out = os.path.join(tmp, 'UeAssets')
        proc = subprocess.run([sys.executable, os.path.join(HERE, 'genueassets.py'), registry_of('AssetTest'), UEAPI, out],
                              capture_output=True, text=True)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        h = open(os.path.join(out, 'UEnemyDescriptor.h'), encoding='utf-8-sig').read()
        assert 'UE_ASSET_AT(::UEnemyDescriptor, ED_AssetTest, "/Game/_ElytrasMods/AssetTest/ED_AssetTest");' in h, h
        assert not os.path.exists(os.path.join(out, 'UBlueprintGeneratedClass.h')), os.listdir(out)
        with open(os.path.join(tmp, 'UeAssetsUser.cpp'), 'w') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeAssets/UEnemyDescriptor.h"\n'
                    'UE_MOD_PACKAGE("/Game/_ElytrasMods/UeAssetsUser");\n'
                    'class UeAssetsUser : public AActor {\npublic:\n'
                    '  UEnemyDescriptor *Ed = &UeAssets::UEnemyDescriptor::Game::_ElytrasMods::AssetTest::ED_AssetTest;\n'
                    '  int32 Count() { return UeAssets::UEnemyDescriptor::All.Num(); }\n};\n')
        proc = subprocess.run([ASSETGEN, 'compile', os.path.join(tmp, 'UeAssetsUser.cpp'), UEAPI, tmp], capture_output=True, text=True)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        user = os.path.join(tmp, 'UeAssetsUser')
        cdo = dump('dumptags.py', user, exports_of(user).index('Default__UeAssetsUser_C'))
        ed = int(re.search(r'Ed \[0\] ObjectProperty size=4: index (-?\d+)', cdo).group(1))
        assert ref(user, ed) == '/Game/_ElytrasMods/AssetTest/ED_AssetTest.ED_AssetTest', cdo
        assert global_default(tmp, 'UeAssets__UEnemyDescriptor__All', 'All') == ['/Game/_ElytrasMods/AssetTest/ED_AssetTest.ED_AssetTest']
    print('ok  genueassets: a header per class names each asset by its path, and a mod reaches one, or All, through it')


def global_default(folder, cls, member):
    """The default a generated global class holds: a soft path (list) decoded from its FName indices, else the tag."""
    import struct
    base = os.path.join(folder, cls)
    tags = dump('dumptags.py', base, exports_of(base).index('Default__%s_C' % cls))
    m = re.search(r'^  %s \[0\] (\w+) size=\d+( inner=\w+)?: (.*)$' % member, tags, re.M)
    if not m: return None
    if 'SoftObjectProperty' not in m.group(0): return m.group(3)
    names, raw = dumpexp.load(base)[3], bytes.fromhex(m.group(3))
    one = m.group(1) == 'SoftObjectProperty'                                # a lone FSoftObjectPath: FName, sub-path
    at = [0] if one else range(4, 4 + 12 * struct.unpack_from('<i', raw)[0], 12)
    paths = [names[struct.unpack_from('<i', raw, o)[0]] for o in at]
    return paths[0] if one else paths


def globals_():
    """GlobalTest: each namespace-scope variable a function uses is the one member of a class generated for it,
    <Ns>__<Name>, whose default object holds the initializer. Every class of the mod reads and writes that one object.
    UE_ASSET_ALL's All lists, as soft paths, the UE_ASSET_ATs under its namespace whose class is its element class."""
    folder = os.path.dirname(asset('GlobalTest'))
    assert global_default(folder, 'Counter', 'Counter') == '5'
    assert global_default(folder, 'Greeting', 'Greeting') == "'hi'"
    assert global_default(folder, 'Tally__Hits', 'Hits') is None           # zero, as a C++ global with no initializer
    picks = global_default(folder, 'Picks__All', 'All')
    assert sorted(picks) == ['/Game/Enemies/Spider/Exploder/ED_Spider_Exploder.ED_Spider_Exploder',
                             '/Game/Enemies/Spider/Grunt/ED_Spider_Grunt.ED_Spider_Grunt'], picks   # not the texture
    web = '/Game/LevelElements/RoomObjects/Hazards/StickySpiderWeb/T_StickySpiderWeb_Corner'
    assert global_default(folder, 'GlobalTest', 'WebIcon') == web + '.T_StickySpiderWeb_Corner'
    print('ok  GlobalTest: each global is its own class\'s default, its initializer; All is its namespace\'s assets of its class')
    # Run: the default objects as their cooked defaults say, shared by both classes.
    objs = {'Default__Counter_C': Obj('Counter_C', Counter=5), 'Default__Greeting_C': Obj('Greeting_C', Greeting='hi'),
            'Default__Tally__Hits_C': Obj('Tally__Hits_C'), 'Default__Picks__All_C': Obj('Picks__All_C', All=list(picks))}
    vm, peer = VM(asset('GlobalTest'), objects=objs), VM(os.path.join(folder, 'GlobalPeer'), objects=objs)
    assert [vm.call('Bump', 2), vm.call('Bump', 3), peer.call('Read')] == [7, 10, 1002]
    assert [vm.call('Take'), vm.call('Take'), peer.call('Read')] == [10, 11, 1202]     # Counter++ is the value before
    assert vm.call('Greet') == 'hi!' and vm.call('PickCount') == 2 and vm.call('FirstPick') == picks[0]
    print('ok  GlobalTest: both classes read and write the one object: =, op=, ++ and a postfix value')


interfaces()
interface_bodies()
interface_calls()
inherited_defaults()
parent_call()
engine_names()
object_forwards()
api_stub()
static_assets()
ue_assets()
globals_()


# ---- ReplTest, LatentTest, AsyncTest, SpawnTest

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
        assert re.search(r'Property %s .*flags=%s rep=\d+ notify=%s cond=%d' % (prop, flags, notify, cond), cls), prop
    for fn, flags in (('ServerOpen', 0xc2208c0), ('ClientPing', 0xd020840), ('MultiBoom', 0xc024840), ('OnRep_Open', 0xc020800),
                      ('ServerBump', 0xc620840), ('AuthOnly', 0xc020804), ('Pretty', 0xc020808)):
        assert 'FunctionFlags %#x' % flags in tool('dumpstruct.py', exports.index(fn)), fn
    print('ok  ReplTest: replicated properties, their conditions and notifies, RPC flags')

    # A mod child's override of a mod parent's RPC: the parent's flags, and the parent's function as its super.
    kid = os.path.join(os.path.dirname(base), 'ReplKid')
    imports, kid_exports = dumpexp.load(kid)[4], dumpexp.load(kid)[5]
    for fn, flags in (('ServerOpen', 0xc2208c0), ('MultiBoom', 0xc024840), ('OnRep_Open', 0xc020800)):
        e = next(x for x in kid_exports if x['name'] == fn)
        out = subprocess.run([sys.executable, os.path.join(here, 'dumpstruct.py'), kid, str(kid_exports.index(e))], capture_output=True, text=True).stdout
        assert 'FunctionFlags %#x' % flags in out, (fn, out)
        assert e['super'] < 0 and imports[-e['super'] - 1] == "Function'%s'" % fn, (fn, e['super'])
    print("ok  ReplTest: an override of a mod parent's RPC keeps its net flags and names it as super")


def latent_flags():
    """The generated completion events: flags the engine checks before binding a delegate to them."""
    base = asset('LatentTest')
    for ev in ('Load_OnLoaded_0', 'Load_OnLoaded_1'):
        assert 'FunctionFlags 0xc000000' in dump('dumpstruct.py', base, export_index(base, ev)), ev
    names = set(exports_of(os.path.join(os.path.dirname(base), 'LatentJob')))
    assert names == {'LatentJob_C', 'Default__LatentJob_C', 'ExecuteUbergraph_LatentJob', 'Run'}, names
    base = asset('AsyncTest')
    for ev in ('Download_OnSuccess_0', 'Download_OnFail_0', 'PlayThen_OnCompleted_0'):
        assert 'FunctionFlags 0xc000000' in dump('dumpstruct.py', base, export_index(base, ev)), ev
    print('ok  LatentTest / AsyncTest: the generated events are what a delegate binds to')


def latent_runs():
    """LatentTest through its latent actions: each Delay / LoadAsset parks the function; firing the action
    resumes it after the call with its locals, on the actor, in the class's ubergraph."""
    lat = {n: latent_call for n in ('Delay', 'LoadAsset', 'LoadAssetClass')}
    vm = VM(asset('LatentTest'), lat, Stage=4, Log=[])
    vm.call('ReceiveBeginPlay')
    assert vm.self.vars == dict(Stage=1, Log=[]), vm.self.vars                      # parked in Delay(this, 0.5)
    [(fn, ctx, info, _)] = vm.latent
    assert (fn, ctx, info[2], info[3], vm.log[-1][2][1]) == ('Delay', vm.self, 'ExecuteUbergraph_LatentTest', vm.self, 0.5)
    vm.call('ViaInline')                                  # waits alongside, with an action of its own: Tag = 1 + 3
    assert len(vm.latent) == 2 and vm.latent[0][2][1] != vm.latent[1][2][1], vm.latent
    vm.fire(0)
    assert vm.self.vars == dict(Stage=12, Log=[]), vm.self.vars                     # Local (4 + 7) survived: + 1
    for i in range(3):                                    # one Delay(0.25) per round, I kept across each
        vm.fire(len(vm.latent) - 1)
        assert vm.self.vars['Log'][:i + 1] == list(range(i + 1)), vm.self.vars
    assert vm.self.vars['Log'] == [0, 1, 2, 100] and vm.log[-1][2][1] == 1.0, vm.self.vars   # Wait(1.0, 100) parked
    vm.fire(len(vm.latent) - 1)
    assert vm.self.vars['Log'] == [0, 1, 2, 100, 101], vm.self.vars                 # its Tag survived the Delay
    vm.fire(0)
    assert vm.self.vars['Stage'] == 4 and not vm.latent, (vm.self.vars, vm.latent)   # ViaInline's Tag, not Wait's
    print('ok  LatentTest: every latent call parks its function and resumes after it with the locals it had')
    for loaded in (Obj('Texture2D'), None):
        vm = VM(asset('LatentTest'), lat, Stage=9, Wanted='soft:Cls', Icon='soft:Icon')
        vm.call('Load')
        [(fn, ctx, info, dlg)] = vm.latent
        assert (fn, vm.log[-1][2][1], info[3]) == ('LoadAssetClass', 'soft:Cls', vm.self) and dlg, vm.latent
        vm.fire(0, result='Cls')
        assert vm.self.vars['Got'] == 'Cls' and [(a[0], vm.log[-1][2][1]) for a in vm.latent] == [('LoadAsset', 'soft:Icon')]
        vm.fire(0, result=loaded)
        assert vm.self.vars['Stage'] == (1 if loaded else 0) and not vm.latent, vm.self.vars
    print('ok  LatentTest.Load: LoadAssetClass / LoadAsset resume with the loaded value as the call\'s value')
    vm = VM(os.path.join(os.path.dirname(asset('LatentTest')), 'LatentJob'), {'Delay': latent_call})
    vm.call('Run', 0.75)
    [(fn, ctx, info, _)] = vm.latent
    assert ctx is vm.self and info[3] is vm.self and vm.log[-1][2][1] == 0.75 and 'Done' not in vm.self.vars
    vm.fire()
    assert vm.self.vars['Done'] == 1
    assert run(asset('LatentTest'), 'Plain', self_vars=vm.self.vars)[0] is None and vm.self.vars['Stage'] == 5
    print('ok  LatentTest: a UObject (LatentJob) waits and resumes too; a function with no latent call just runs')


def latent_links():
    """Every FLatentActionInfo names its class's ubergraph, targets Self, has a UUID no other latent call of the class
    shares, and a Linkage that starts a statement of that ubergraph."""
    for base in (asset('LatentTest'), os.path.join(os.path.dirname(asset('LatentTest')), 'LatentJob')):
        vm = VM(base)
        uber = next(e for e in vm.exports if e.startswith('ExecuteUbergraph_'))
        infos = []

        def walk(n):
            if n.op == 0x2F and n.val == 'LatentActionInfo': infos.append(n)
            for k in n.kids: walk(k)
        for fn in vm.exports:
            try: [walk(n) for n in vm.script(fn)[0]]
            except SystemExit: pass                      # the class and CDO exports have no script
        assert infos, base
        for link, uuid, execfn, target in (n.kids for n in infos):
            assert execfn.val == uber and target.op == 0x17 and link.val in vm.script(uber)[1], (base, link.val)
        assert len({n.kids[1].val for n in infos}) == len(infos), [n.kids[1].val for n in infos]
    print('ok  LatentTest: each latent call resumes a statement of its own ubergraph, under a UUID of its own')


def await_runs():
    """AsyncTest with its proxies faked: what is bound to which dispatcher, when Activate runs, what a broadcast does."""
    made, activated = [], []
    def factory(cls):
        return lambda vm, ctx, *a: made.append(Obj(cls, args=a)) or made[-1]
    natives = {'CreateProxyObjectForPlayMontage': factory('PlayMontageCallbackProxy'),
               'DownloadImage': factory('AsyncTaskDownloadImage'),
               'Activate': lambda vm, ctx: activated.append((ctx, [p for o, p, f, _ in vm.binds if o is ctx]))}
    vm = VM(asset('AsyncTest'), natives, Mesh='mesh', Montage='montage', Last='None')
    bound = lambda obj: sorted((p, f) for o, p, f, b in vm.binds if o is obj and b is vm.self)
    vm.call('Play')                                       # callback style: both dispatchers call Done
    proxy = made[-1]
    assert proxy.vars['args'] == ('mesh', 'montage', 1.0, 0.0, 'None'), proxy.vars
    assert bound(proxy) == [('OnCompleted', 'Done'), ('OnInterrupted', 'Done')], vm.binds
    vm.broadcast(proxy, 'OnInterrupted', 'Cut')
    assert vm.self.vars['Last'] == 'Cut'
    vm.call('PlayThen')                                   # await style: nothing after the await runs yet
    proxy = made[-1]
    assert vm.self.vars['Last'] == 'Cut' and [p for p, f in bound(proxy)] == ['OnCompleted'], vm.binds
    vm.broadcast(proxy, 'OnCompleted', 'End')
    assert vm.self.vars['Last'] == 'End' and not activated     # a montage proxy is not an async action
    vm.call('Download', 'http://x')                       # an async action: activated once, OnSuccess already bound
    task = made[-1]
    assert task.vars['args'] == ('http://x',) and activated == [(task, ['OnSuccess'])] and 'Image' not in vm.self.vars
    vm.broadcast(task, 'OnSuccess', 'tex')
    assert vm.self.vars['Image'] == 'tex' and [p for p, f in bound(task)] == ['OnFail', 'OnSuccess'], vm.binds
    vm.broadcast(task, 'OnFail', None)
    assert vm.self.vars['Image'] is None and len(activated) == 1
    print('ok  AsyncTest: binds, one Activate after the first bind, each await resuming with its value')


def delegate_targets():
    """Every function a delegate is bound to by name exists in the class and takes the one parameter its dispatcher passes."""
    for mod in ('AsyncTest', 'LatentTest'):
        vm, names = VM(asset(mod)), set()

        def walk(n):
            if n.op == 0x4B: names.add(n.val)
            for k in n.kids: walk(k)
        for fn in vm.exports:
            try: [walk(n) for n in vm.script(fn)[0]]
            except SystemExit: pass
        assert names and all(f in vm.exports and len(vm.script(f)[2]) == 1 for f in names), (mod, names)
    print('ok  AsyncTest / LatentTest: every delegate bound by name is a one-parameter function of the class')


def repl_runs():
    """ReplTest as the server runs it: a replicated write wakes the object first, a RepNotify variable's OnRep runs right
    after the write (on the object written), an RPC called on the authority runs its body."""
    vm = VM(asset('ReplTest'), Score=5, Slots=[])
    vm.call('ReceiveBeginPlay')
    assert vm.self.vars == dict(Score=6, Slots=[4], bOpen=False, Local=1, Notified=12), vm.self.vars
    trace = [(l[0], l[2]) if l[0] == 'set' else (l[0],) for l in vm.log if l[1] is vm.self]
    assert trace == [('FlushNetDormancy',), ('set', 'bOpen'), ('set', 'Notified'),        # bOpen = true; OnRep_Open
                     ('FlushNetDormancy',), ('set', 'Slots'), ('set', 'Notified'),        # Slots[0] = 4; OnRep_Slots
                     ('set', 'Local'),
                     ('FlushNetDormancy',), ('set', 'bOpen'), ('set', 'Notified'),        # ServerOpen(false)
                     ('FlushNetDormancy',), ('set', 'Score')], trace                      # MultiBoom
    vm = VM(asset('ReplTest'))
    other = vm.self.vars['Other'] = vm.new(bOpen=True, Notified=0)
    vm.call('SetOther')
    assert other.vars == dict(bOpen=False, Notified=1, Local=2) and vm.self.vars['bReplicateMovement'] is True
    trace = [(l[0], l[1] is other, l[2] if l[0] == 'set' else None) for l in vm.log]
    assert trace == [('FlushNetDormancy', True, None), ('set', True, 'bOpen'), ('set', True, 'Notified'),
                     ('set', True, 'Local'), ('FlushNetDormancy', False, None),
                     ('set', False, 'bReplicateMovement')], trace       # a native RepNotify is for clients: no call
    for fn, parms, want in (('ClientPing', dict(Seq=42), dict(Local=42)), ('AuthOnly', {}, dict(Local=3)),
                            ('Pretty', {}, dict(Local=4)), ('OnRep_Open', {}, dict(Notified=1)),
                            ('OnRep_Slots', {}, dict(Notified=10))):
        vm = VM(asset('ReplTest'))
        vm.call(fn, **parms)
        assert vm.self.vars == want, (fn, vm.self.vars)
    assert run(asset('ReplTest'), 'ServerBump', Count=41)[1]['Count'] == 42
    kid = VM(os.path.join(os.path.dirname(asset('ReplTest')), 'ReplKid'))
    for fn, parms, seen in (('ServerOpen', dict(bValue=True), 7), ('ServerOpen', dict(bValue=False), 8),
                            ('MultiBoom', {}, 9), ('OnRep_Open', {}, 10)):
        kid.call(fn, **parms)
        assert kid.self.vars['Seen'] == seen, (fn, kid.self.vars)
    print('ok  ReplTest: wake before a replicated write, OnRep after it, RPC bodies; ReplKid overrides')


def rpc_routing():
    """A call to a net / authority-only / cosmetic function goes through CallFunction's callspace routing
    (EX_VirtualFunction / EX_FinalFunction): a Local* call (ProcessLocalFunction) would just run it here."""
    base = asset('ReplTest')
    vm, flags = VM(base), {}
    for i, e in enumerate(dumpexp.load(base)[5]):
        m = re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base, i))
        if m: flags[e['name']] = int(m.group(1), 16)
    routed = {f for f, v in flags.items() if v & (0x40 | 0x4 | 0x8)}      # FUNC_Net | AuthorityOnly | Cosmetic
    calls = []

    def walk(n):
        if n.op in (0x1B, 0x1C, 0x45, 0x46) and n.val in routed: calls.append((n.val, n.op))
        for k in n.kids: walk(k)
    for fn in flags:
        [walk(n) for n in vm.script(fn)[0]]
    assert sorted(f for f, _ in calls) == ['MultiBoom', 'ServerOpen'] and all(op in (0x1B, 0x1C) for _, op in calls), calls
    print('ok  ReplTest: RPC calls go through the net routing')


def spawn_runs():
    """SpawnTest with the engine faked: which classes are spawned / constructed / added, with what, in which order."""
    made = lambda k: (lambda vm, ctx, *a: Obj(a[k], args=a))
    natives = {'Conv_VectorToTransform': lambda vm, ctx, v: ('xf', tuple(v)),
               'BeginDeferredActorSpawnFromClass': made(1), 'SpawnObject': made(0), 'Create': made(1),
               'AddComponentByClass': made(0), 'FinishSpawningActor': lambda vm, ctx, a, xf: a,
               'K2_GetRootComponent': lambda vm, ctx: 'root'}
    vm = VM(asset('SpawnTest'), natives)
    vm.call('ReceiveBeginPlay')
    me, spawned = vm.self, vm.self.vars['Spawned']
    where, zero = ('xf', (0.0, 0.0, 100.0)), ('xf', (0.0, 0.0, 0.0))
    twin = next(l[2][0] for l in vm.log if l[0] == 'FinishSpawningActor' and l[2][0] is not spawned)
    late = next(l[2][0] for l in vm.log if l[0] == 'FinishAddComponent')
    assert [l for l in vm.log if l[0] not in ('Conv_VectorToTransform', 'K2_GetRootComponent')] == [
        ('BeginDeferredActorSpawnFromClass', me, [me, 'Actor', where, 0, me]), ('FinishSpawningActor', me, [spawned, where]),
        ('set', me, 'Spawned'),
        ('BeginDeferredActorSpawnFromClass', me, [me, 'SpawnTest_C', where, 0, None]),   # the mod class, deferred,
        ('set', twin, 'Tag'), ('FinishSpawningActor', me, [twin, where]),              # Tag set before it finishes
        ('SpawnObject', me, ['USpawnProbe_C', me]), ('set', me, 'Made'),
        ('Create', me, [me, 'UserWidget', None]), ('set', me, 'Widget'),
        ('AddComponentByClass', me, ['SceneComponent', False, zero, False]), ('set', me, 'Part'),
        ('K2_AttachToComponent', vm.self.vars['Part'], ['root', 'None', 2, 2, 2, True]),  # SnapToTarget, weld
        ('AddComponentByClass', me, ['SceneComponent', False, zero, True]),               # held back ...
        ('set', late, 'bHiddenInGame'), ('FinishAddComponent', me, [late, False, zero]),  # ... until set, then finished
        ('K2_AttachToActor', spawned, [me, 'None', 1, 1, 1, True])], vm.log              # KeepWorld
    assert twin.vars['Tag'] == 7 and late.vars['bHiddenInGame'] is True
    # The twin's own BeginPlay (FinishSpawning runs it) sees Tag = 7 and spawns nothing.
    twin.cls, log = me.cls, len(vm.log)
    vm.call('ReceiveBeginPlay', on=twin)
    assert len(vm.log) == log, vm.log[log:]
    assert vm.call('OwnClass') == 'SpawnTest_C'
    probe = vm.call('MakeProbe')
    assert (probe.cls, probe.vars['args']) == ('USpawnProbe_C', ('USpawnProbe_C', me)), probe.vars
    print('ok  SpawnTest: spawn / construct / add-component calls, their classes and the deferred-set order')


def spawn_relative():
    """Compiled from its own folder as a bare "SpawnTest.cpp": `SpawnTest::StaticClass()` still names the mod class
    (the qualifier is read back from the mod's sources, which a bare path's empty parent once hid)."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        proc = subprocess.run([ASSETGEN, 'compile', 'SpawnTest.cpp', UEAPI, tmp], capture_output=True, text=True, cwd=TESTS)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        assert VM(os.path.join(tmp, 'SpawnTest'), {}).call('OwnClass') == 'SpawnTest_C'
    print('ok  SpawnTest: a bare relative source path finds X::StaticClass() qualifiers')


def outer_runs():
    """GetTypedOuter / GetOutermostTypedOuter over a faked outer chain: nearest / farthest outer of the kind, the
    object itself never a candidate, null when there is none; and no function of their own."""
    kinds = {'Pawn': {'Pawn', 'Actor'}}
    isa = lambda o, c: isinstance(o, Obj) and c in kinds.get(o.cls, {o.cls})
    pkg = Obj('Package'); outer_lvl = Obj('Level', Obj('World', pkg)); lvl = Obj('Level', outer_lvl)
    far = Obj('Actor', lvl); near = Obj('Pawn', far); comp = Obj('SceneComponent', near)
    vm = VM(asset('SpawnTest'), {'GetOuterObject': lambda vm, ctx, o: o.outer}, isa=isa)
    for fn, obj, want in (('OwningActor', comp, near), ('OwningActor', near, far), ('OwningActor', far, None),
                          ('OwningActor', None, None), ('LevelOf', comp, outer_lvl), ('LevelOf', outer_lvl, None)):
        assert vm.call(fn, obj) is want, (fn, obj, want)
    assert not {'GetTypedOuter', 'GetOutermostTypedOuter'} & vm.exports, vm.exports
    print('ok  SpawnTest: GetTypedOuter / GetOutermostTypedOuter walk the outers, inlined')


def uber_frames():
    """Every class with an ubergraph: the class names it (UberGraphFunction), carries the transient UberGraphFrame
    pointer the engine allocates the persistent frame through, and the function is FUNC_UbergraphFunction (0x8000) -
    the flag ProcessEvent / ProcessScriptFunction use to run it on that frame instead of a fresh one."""
    folder = os.path.dirname
    for base in (asset('LatentTest'), os.path.join(folder(asset('LatentTest')), 'LatentJob'), asset('AsyncTest')):
        names = [e['name'] for e in dumpexp.load(base)[5]]
        uber = next(i for i, n in enumerate(names) if n.startswith('ExecuteUbergraph_'))
        cls = dump('dumpstruct.py', base, 0)
        assert 'UberGraphFunction [0] ObjectProperty size=4: index %d' % (uber + 1) in cls, (base, cls)
        assert re.search(r"StructProperty UberGraphFrame .*flags=0x202000 .*PointerToUberGraphFrame", cls), cls
        assert int(re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base, uber)).group(1), 16) & 0x8000, base
    print('ok  LatentTest / AsyncTest: each ubergraph is the class\'s UberGraphFunction and runs on the persistent frame')


def repl_defaults():
    """The actor replicates (CDO bReplicates, or no replicated variable ever leaves the server) and keeps Score's
    initializer; ReplKid inherits both rather than overriding them."""
    folder = os.path.dirname(asset('ReplTest'))
    for name, own in (('ReplTest', True), ('ReplKid', False)):
        base = os.path.join(folder, name)
        names = [e['name'] for e in dumpexp.load(base)[5]]
        cdo = dump('dumptags.py', base, names.index('Default__%s_C' % name))
        if own: assert re.search(r'bReplicates \[0\] BoolProperty size=0 value=1', cdo) and 'Score [0] IntProperty size=4: 5' in cdo, cdo
        else: assert 'bReplicates' not in cdo and 'Score' not in cdo, cdo
    print('ok  ReplTest: the CDO replicates and holds Score = 5; ReplKid inherits them')


replication()
repl_defaults()
repl_runs()
rpc_routing()
uber_frames()
latent_flags()
latent_runs()
latent_links()
await_runs()
delegate_targets()
spawn_runs()
spawn_relative()
outer_runs()
