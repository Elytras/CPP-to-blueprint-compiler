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
CLUSTERS = {0: 1, 1: 2, 2: 3, 5: 6, 9: 10, 50: 500, 100: 1000, 103: 1003, 104: 1004, 110: 1010, 7000: 7}
check('FlowTest', 'Clusters', lambda Code: CLUSTERS.get(Code, -1), [dict(Code=c) for c in list(CLUSTERS) + [-1, 3, 10, 11, 49, 99, 101, 111, 6999, 7001]])


def clusters_shape():
    import re, subprocess
    base = asset('FlowTest')
    names = [e['name'] for e in dumpexp.load(base)[5]]
    w = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'walkscript.py'), base,
                        str(names.index('Clusters'))], capture_output=True, text=True).stdout
    assert w.count('ComputedJump') == 2 and w.count('InRange_IntInt') == 2 and w.count('NotEqual_IntInt') == 2, w
    print('ok  FlowTest.Clusters: two tables behind range checks, the outliers compared')


clusters_shape()
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
def mod_enum():
    import re, struct, subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    base = asset('TypesTest')
    enum = os.path.join(os.path.dirname(base), 'EMood')
    r = dumpexp.load(enum)
    names, raw = r[3], r[1]
    assert [e['name'] for e in r[5]] == ['EMood'] and "Class'UserDefinedEnum'" in r[4], r[4]
    for n in ('EMood::Calm', 'EMood::Angry', 'EMood::Sleepy', 'EMood::EMood_MAX'):
        assert n in names, names
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
    assert re.search(r'Spans \[0\] ArrayProperty size=185 inner=StructProperty', cdo), cdo                 # 2 x (Min tag, Max tag, None) = 132
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
    base = asset('StringTest')
    exports = [e['name'] for e in dumpexp.load(base)[5]]
    walk = subprocess.run([sys.executable, os.path.join(here, 'walkscript.py'), base, str(exports.index('ReceiveBeginPlay'))], capture_output=True, text=True).stdout
    flat = ' '.join(walk.split())
    assert re.search(r"Concat_StrStr' .{0,60}?StringConst 'Kills: ' .{0,60}?Conv_IntToString'", flat), flat[-900:]
    assert re.search(r"Concat_StrStr' .{0,60}?Conv_IntToString' .{0,160}?StringConst ' left'", flat), flat[-900:]
    print('ok  StringTest: "lit" + N and N + "lit" are Concat_StrStr, not pointer arithmetic')


def engine_names():
    import re, subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    base = asset('NameTest')
    loaded = dumpexp.load(base)
    exports = [e['name'] for e in loaded[5]]
    tool = lambda t, i: subprocess.run([sys.executable, os.path.join(here, t), base, str(i)], capture_output=True, text=True).stdout
    cdo = tool('dumptags.py', exports.index('Default__NameTest_C'))
    assert 'Index [0] IntProperty size=4: 7' in cdo and re.search(r"Name \[0\] StrProperty size=\d+: 'Karl'", cdo), cdo
    # SplitName would have made FName(Index, 1) of the C++ spelling: neither it nor the spelling may be in the package.
    assert 'Index_0' not in loaded[3] and 'Name_0' not in loaded[3], [n for n in loaded[3] if n.endswith('_0')]
    walk = tool('walkscript.py', exports.index('Next'))
    assert re.search(r"InstanceVariable\s+Index@imp\[\d+\]:Class'FSDSaveGame'", walk) and 'Index_0' not in walk, walk
    print('ok  NameTest: a member Dumper-7 respelled is cooked by the engine\'s name (tag and bytecode)')
    # FUNC_Public 0x20000, FUNC_Private 0x40000, FUNC_Protected 0x80000: exactly the one the C++ specifier says.
    for fn, want in (('Next', 0x20000), ('Step', 0x80000), ('Twice', 0x40000)):
        flags = int(re.search(r'FunctionFlags (\S+)', tool('dumpstruct.py', exports.index(fn))).group(1), 16)
        assert flags & 0xE0000 == want, (fn, hex(flags))
    print('ok  NameTest: a function carries its C++ access specifier')
    # CPF_BlueprintReadOnly 0x10 on the const member alone, and its initializer is the default.
    props = tool('dumpstruct.py', exports.index('NameTest_C'))
    flags = lambda name: int(re.search(r'Property %s .*? flags=(\S+)' % name, props).group(1), 16)
    assert flags('Limit') & 0x10 and not flags('Seed') & 0x10, props
    assert 'Limit [0] IntProperty size=4: 3' in cdo, cdo
    print('ok  NameTest: a const member is BlueprintReadOnly')


mod_enum()
constants()
engine_names()
check('TypesTest', 'Cpp20', lambda M, N: {0: N + N, 5: N + fnv('angry')}.get(M, fnv('types')), [dict(M=m, N=n) for m in (0, 5, 6) for n in (-2, 9)])
check('TypesTest', 'ConstSum', lambda N: N * 3 + 12 + 19, [dict(N=n) for n in (-4, 0, 9)])
check('TypesTest', 'HalfOf', lambda V: V * 0.5, [dict(V=v) for v in (-3.0, 0.0, 8.0)])
check('TypesTest', 'SpanScore', lambda S: {-3: 1, 70000: 2, 70001: 3, 70002: 4}.get(S, 0), [dict(S=s) for s in (-3, 70000, 70001, 70002, 5)])
check('TypesTest', 'AgeOf', lambda A: 1 if A == 5000000000 else 2 if A == 0 else 0, [dict(A=a) for a in (0, 5000000000, 7)])
check('TypesTest', 'MoodScore', lambda M: {0: 1, 5: 2, 6: 3}.get(M, 0), [dict(M=m) for m in (0, 5, 6, 7)])
check('InlineTest', 'InPlace', lambda V: V * 3 + 1 + V + 1, [dict(V=v) for v in (-3, 0, 7)])


def inline_in_place():
    import re, subprocess
    base = asset('InlineTest')
    names = [e['name'] for e in dumpexp.load(base)[5]]
    walk = lambda fn: subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'walkscript.py'),
                                      base, str(names.index(fn))], capture_output=True, text=True).stdout
    assert not re.search(r'__Inl\d+_A\b', walk('InPlace')), walk('InPlace')
    assert re.search(r'__Inl\d+_A\b', walk('KeptLocal')), walk('KeptLocal')
    print('ok  InlineTest: an argument read once goes in place, a late read keeps its local')


inline_in_place()
check('InlineTest', 'ConstThenVar', lambda V: 6 + V * 2 + clamp(V, 0, 5) + clamp(2, V, 9) + 4 + V - 1, [dict(V=v) for v in (-3, 0, 4, 12)])


check('StructTest', 'MakeLocal', lambda K: K + K * 20 + 1000, [dict(K=k) for k in (0, 3, -2)])
check('StructTest', 'MakeArgument', lambda K: K + 0 + K + 1, [dict(K=k) for k in (0, 5)])
check('StructTest', 'MakeInLoop', lambda Rounds: max(Rounds, 0), [dict(Rounds=r) for r in (0, 1, 4)])
check('StructTest', 'MakeNative', lambda D: D + 0.5, [dict(D=d) for d in (0.0, 4.0)])


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
    for fn, flags in (('ServerOpen', 0x82208c0), ('ClientPing', 0x9020840), ('MultiBoom', 0x8024840), ('OnRep_Open', 0x8020800),
                      ('ServerBump', 0x8620840), ('AuthOnly', 0x8020804), ('Pretty', 0x8020808)):
        assert 'FunctionFlags %#x' % flags in tool('dumpstruct.py', exports.index(fn)), fn
    ops = re.findall(r'\d  (\w+) +(\S*)', tool('walkscript.py', exports.index('ReceiveBeginPlay')))
    calls = [(op, a) for op, a in ops if op in ('VirtualFunction', 'LocalVirtualFunction')]
    # OnRep functions are ordinary script functions, called locally; an RPC goes through CallFunction's routing.
    assert calls == [('LocalVirtualFunction', 'OnRep_Open'), ('LocalVirtualFunction', 'OnRep_Slots'),
                     ('VirtualFunction', 'ServerOpen'), ('VirtualFunction', 'MultiBoom')], calls
    # A set of a replicated variable wakes the actor first, as the editor's Set node does; Local is not replicated.
    begin = [(op, a) for op, a in ops if op in ('FinalFunction', 'LetBool', 'Let')]
    assert [x[0] for x in begin] == ['FinalFunction', 'LetBool', 'FinalFunction', 'Let', 'Let'], begin
    assert sum('FlushNetDormancy' in x[1] for x in begin) == 2, begin
    # On another object: its FlushNetDormancy and OnRep; a native replicated property (Actor.bReplicateMovement) only wakes.
    rows = [re.split(r'\s+', l.split(' mem ')[-1].strip(), maxsplit=2) for l in tool('walkscript.py', exports.index('SetOther')).splitlines() if ' mem ' in l]
    seq = [(r[2] if r[1] == 'FinalFunction' else r[1]) for r in rows if len(r) > 1 and r[1] in ('FinalFunction', 'LetBool', 'Let', 'VirtualFunction')]
    assert seq == ["imp[6]:Function'FlushNetDormancy'", 'LetBool', 'VirtualFunction', 'Let', "imp[6]:Function'FlushNetDormancy'", 'LetBool'], seq
    print('ok  ReplTest: replicated properties, RPC flags, OnRep after a set')

    # A mod child's override of a mod parent's RPC: the parent's flags, and the parent's function as its super.
    kid = os.path.join(os.path.dirname(base), 'ReplKid')
    imports, kid_exports = dumpexp.load(kid)[4], dumpexp.load(kid)[5]
    for fn, flags in (('ServerOpen', 0x82208c0), ('MultiBoom', 0x8024840), ('OnRep_Open', 0x8020800)):
        e = next(x for x in kid_exports if x['name'] == fn)
        out = subprocess.run([sys.executable, os.path.join(here, 'dumpstruct.py'), kid, str(kid_exports.index(e))], capture_output=True, text=True).stdout
        assert 'FunctionFlags %#x' % flags in out, (fn, out)
        assert e['super'] < 0 and imports[-e['super'] - 1] == "Function'%s'" % fn, (fn, e['super'])
    print("ok  ReplTest: an override of a mod parent's RPC keeps its net flags and names it as super")


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
    # A plain UObject waits the same way: its own ubergraph, the Delay on Self, a stub that jumps in.
    base = os.path.join(os.path.dirname(base), 'LatentJob')
    names = [e['name'] for e in dumpexp.load(base)[5]]
    assert names == ['LatentJob_C', 'Default__LatentJob_C', 'ExecuteUbergraph_LatentJob', 'Run'], names
    tool = lambda t, i: subprocess.run([sys.executable, os.path.join(here, t), base, str(i)], capture_output=True, text=True).stdout
    assert re.search(r"Function'Delay'\s+.*Self", tool('walkscript.py', 2)) and 'LetValueOnPersistentFrame Run_Seconds' in tool('walkscript.py', 3)
    print('ok  LatentTest: a UObject class makes a latent call too')


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
    # Found and Again hold 2; Grid ends [[7], [1]] after Grid[1].Add(5), Grid[0][0] = 7 and Grid[1] = Row.
    assert got == 2 + 2 + 2 + 7, got
    assert fields == {'Groups': {'first': ['a', 'b']}, 'Grid': [[7], [1]]}, fields
    out = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dumpstruct.py'),
                          asset('NestedTest'), '0'], capture_output=True, text=True).stdout
    assert "UserDefinedStruct'FNC_TArray_FName'" in out and "UserDefinedStruct'FNC_TArray_int'" in out, out
    # execMap_Find writes in place only into a property of the value property's class: a nested value goes through a
    # wrapper temp and is copied out of its Value member.
    names = [e['name'] for e in dumpexp.load(asset('NestedTest'))[5]]
    w = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'walkscript.py'),
                        asset('NestedTest'), str(names.index('Build'))], capture_output=True, text=True).stdout
    import re
    assert len(set(re.findall(r'LocalVariable\s+(__Wrap\d+__)', w))) == 2 and len(re.findall(r"StructMemberContext\s+Value@imp\[\d+\]:UserDefinedStruct'FNC_TArray_FName'", w)) == 2, w
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
    check('OptTest', 'Consts', lambda A: A * 3 + 3 + (10 + A), [dict(A=a) for a in (0, 4, -7)])
    w = walk('Consts')
    assert 'Three' not in w and 'Grows' in w, w
    print('ok  OptTest.Consts: a read-only const local folds into its uses, a written one keeps its local')
    cdiv = lambda a, b: abs(a) // abs(b) * (1 if (a < 0) == (b < 0) else -1)
    check('OptTest', 'Logic', lambda X, Y: (1 if X != 0 and cdiv(10, X) > 2 else 0) + (2 if X != 0 and Y != 0 else 0)
          + (10 if X > 0 and cdiv(Y, 2) > 0 else 0) + (100 if X == 0 or Y == 0 else 0),
          [dict(X=x, Y=y) for x in (0, 3, -4, 20) for y in (0, 1, 5)])
    w = walk('Logic')
    assert w.count('BooleanAND') == 1 and w.count('BooleanOR') == 1, w
    assert '__Branch' not in w, w
    print('ok  OptTest.Logic: harmless && / || fold into one call, a guard if nests')
    check('OptTest', 'Raw', lambda X, Y: 1 if X != 0 and Y != 0 else 0, [dict(X=x, Y=y) for x in (0, 3) for y in (0, 5)])
    w = walk('Raw')
    assert 'Multiply_IntInt' in w and 'Add_IntInt' in w and 'BooleanAND' not in w, w
    assert 'Abs_Int' in walk('RawPragma'), walk('RawPragma')
    import re
    fnflags = lambda fn: re.search(r'FunctionFlags (\S+)', subprocess.run(
        [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dumpstruct.py'), base, str(names.index(fn))],
        capture_output=True, text=True).stdout).group(1)
    assert fnflags('RawPragma') == fnflags('Raw') == fnflags('Drop'), (fnflags('RawPragma'), fnflags('Drop'))
    print('ok  OptTest.Raw: UE_NO_OPTIMIZE / #pragma clang optimize off keep what the optimizer drops')


optimizer()


def member_address():
    base = os.path.join(ROOT, 'CppTest', 'FSD', 'Content', '_ElytrasMods', 'CppTest', 'Test')
    names = [e['name'] for e in dumpexp.load(base)[5]]
    w = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'walkscript.py'),
                        base, str(names.index('MemberAddress'))], capture_output=True, text=True).stdout
    assert "Function'GetPropertyAddress'" in w and 'NameConst        Vtbl' in w, w
    print('ok  CppTest.MemberAddress: &Member through ReadProperty::GetPropertyAddress')


member_address()
