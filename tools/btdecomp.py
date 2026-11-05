#!/usr/bin/env python3
"""usage: btdecomp.py dump <base>...
       btdecomp.py census <Content dir>
       btdecomp.py emit <base>... --out <file.cpp> [--package </Game/...>] [--game <Content dir>]
       btdecomp.py roundtrip <Content dir> [--only <substring>]... [--assetgen <exe>] [--ueapi <UeApi dir>]
                             [--keep <dir>] [--game <Content dir>]

Behaviour trees (UBehaviorTree) and blackboards (UBlackboardData) between their cooked packages and AssetGen source.

  dump       each export of a package by meaning: its class, outer and every tagged value, references as paths
  census     every class, property and struct the BT_* and BB_* packages under a Content folder use, with counts
  emit       one .cpp of UE_ASSET_DEFAULTS blocks, one per package, written as a tree: each composite's children follow
             it in nested braces, each child with its decorators, each key selector by its key's name
  roundtrip  emit every BT_* / BB_* package under a Content folder, compile each with AssetGen, and compare the cooked
             result with the original by meaning (exports, classes, outers, flags, every value). Prints each difference,
             then a total; exits non-zero on any it does not explain. --keep leaves the sources and outputs there.

A package is compiled at its own /Game path, so every path in it compares as is. Another package's asset it points at
is read for its class from the same Content folder, else from --game (the extracted game's FSD/Content).

What the round trip may change and the game reads the same is counted, not reported: see EXPLAINED."""
import os, re, struct, subprocess, sys, tempfile, shutil
from collections import Counter, OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import invariants
import bpdecomp_body as B
from bpdecomp_model import Api

HERE = os.path.dirname(os.path.abspath(__file__))
AG = os.path.normpath(os.path.join(HERE, '..'))
UEAPI = os.path.normpath(os.path.join(AG, '..', 'BpMods', 'UeApi'))

# ---- reading: a package as values

NATIVE = {'Vector': '<3f', 'Vector2D': '<2f', 'Rotator': '<3f', 'Vector4': '<4f', 'Quat': '<4f', 'IntPoint': '<2i',
          'IntVector': '<3i', 'LinearColor': '<4f', 'Color': '<4B', 'Guid': '<4I'}


class Ref(str):
    """An object reference, by path."""
    def __repr__(s): return '&' + str.__str__(s)


class Struct(OrderedDict):
    def __init__(s, name, items=()):
        OrderedDict.__init__(s, items)
        s.struct = name


def fname(pkg, b, o):
    i, n = struct.unpack_from('<ii', b, o)
    return pkg.names[i] + ('_%d' % (n - 1) if n else '')


def fstr(b, o):
    n = struct.unpack_from('<i', b, o)[0]
    if n == 0: return '', o + 4
    if n < 0: return b[o + 4:o + 4 - 2 * n - 2].decode('utf-16-le'), o + 4 - 2 * n
    return b[o + 4:o + 4 + n - 1].decode('latin-1'), o + 4 + n


def tag_list(pkg, i, start):
    """{name: value} of the tag list at blob offset start, and the offset past it."""
    tags = pkg.tags(i, start)
    out = OrderedDict()
    for t in tags:
        key = t['name'] if not t['index'] else '%s[%d]' % (t['name'], t['index'])
        out[key] = value(pkg, i, t)
    return out, tags.end


def value(pkg, i, t):
    ty, b = t['type'], bytes(t['value'])
    if ty == 'BoolProperty': return bool(t['bool'])
    if ty == 'StructProperty':
        if t['struct'] in NATIVE: return Struct(t['struct'], [('', struct.unpack(NATIVE[t['struct']], b))])
        if t['struct'] == 'GameplayTagContainer':        # WithSerializer: a count and the tags' FNames
            n = struct.unpack_from('<i', b)[0]
            return Struct(t['struct'], [('', tuple(fname(pkg, b, 4 + 8 * k) for k in range(n)))])
        return Struct(t['struct'], tag_list(pkg, i, t['at'])[0])
    if ty in ('ArrayProperty', 'SetProperty'):
        o = t['at'] + (4 if ty == 'SetProperty' else 0)
        blob = pkg.blob(i)
        n = struct.unpack_from('<i', blob, o)[0]
        o += 4
        if t['inner'] == 'StructProperty':
            sname = fname(pkg, blob, o + 24)
            o += 49                                      # the inner tag: name, type, size, index, struct, guid, no guid
            items = []
            for _ in range(n):
                if sname in NATIVE:
                    sz = struct.calcsize(NATIVE[sname])
                    items.append(Struct(sname, [('', struct.unpack_from(NATIVE[sname], blob, o))])); o += sz
                else:
                    v, o = tag_list(pkg, i, o)
                    items.append(Struct(sname, v))
            return items
        size = (t['size'] - (o - t['at'])) // n if n else 0
        return [scalar(pkg, t['inner'], blob[o + k * size:o + (k + 1) * size]) for k in range(n)]
    if ty == 'MapProperty':
        raise ValueError('a MapProperty (%s)' % t['name'])
    return scalar(pkg, ty, b, t)


def scalar(pkg, ty, b, t=None):
    if ty == 'IntProperty': return struct.unpack('<i', b)[0]
    if ty == 'UInt32Property': return struct.unpack('<I', b)[0]
    if ty == 'Int64Property': return struct.unpack('<q', b)[0]
    if ty == 'UInt16Property': return struct.unpack('<H', b)[0]
    if ty == 'FloatProperty': return struct.unpack('<f', b)[0]
    if ty == 'BoolProperty': return bool(b[0]) if b else bool(t['bool'])
    if ty == 'StrProperty': return fstr(b, 0)[0]
    if ty == 'NameProperty': return ('name', fname(pkg, b, 0))
    if ty in ('ByteProperty', 'EnumProperty'):
        return b[0] if len(b) == 1 else ('enum', fname(pkg, b, 0))
    if ty in ('ObjectProperty', 'ClassProperty', 'InterfaceProperty', 'WeakObjectProperty', 'LazyObjectProperty'):
        idx = struct.unpack('<i', b[:4])[0]
        return Ref(pkg.path(idx)) if idx else None
    if ty in ('SoftObjectProperty', 'SoftClassProperty'):
        p = fname(pkg, b, 0)
        return ('soft', None if p == 'None' else p + (':' + fstr(b, 8)[0] if fstr(b, 8)[0] else ''))
    if ty == 'TextProperty': return ('text', b.hex())
    raise ValueError('a %s' % ty)


class Export:
    def __init__(s, pkg, i):
        e = pkg.exports[i]
        s.index, s.name, s.flags = i, e['name'], e['flags']
        s.path = pkg.path(i + 1)
        s.cls = pkg.path(e['cls']) if e['cls'] < 0 else Ref(pkg.path(e['cls']))
        s.outer = pkg.path(e['outer']) if e['outer'] else None
        s.values = tag_list(pkg, i, 0)[0]


def read(base):
    pkg = invariants.Package(base)
    return pkg, [Export(pkg, i) for i in range(len(pkg.exports))]


def show(v, ind=''):
    if isinstance(v, Struct):
        if list(v) == ['']: return '%s%r' % (v.struct, v[''])
        return '%s{%s}' % (v.struct, ', '.join('%s=%s' % (k, show(x, ind)) for k, x in v.items()))
    if isinstance(v, list): return '[%s]' % ', '.join(show(x, ind) for x in v)
    if isinstance(v, tuple) and len(v) == 2 and v[0] in ('name', 'enum', 'soft', 'text'): return '%s:%s' % v
    return repr(v)


def dump(args):
    for base in args:
        base = re.sub(r'\.(uasset|uexp)$', '', base)
        pkg, exps = read(base)
        print('== %s' % pkg.package_name())
        for x in exps:
            print('[%d] %s  %s  outer=%s flags=0x%x' % (x.index, x.name, x.cls, x.outer and x.outer.rsplit('.', 1)[-1],
                                                     x.flags))
            for k, v in x.values.items(): print('     %s = %s' % (k, show(v)))


def census(args):
    content = args[0]
    classes, props, structs, fails = Counter(), Counter(), Counter(), []

    def walk(owner, vals):
        for k, v in vals.items():
            props['%s.%s' % (owner, k)] += 1
            items = v if isinstance(v, list) else [v]
            for x in items:
                if isinstance(x, Struct):
                    structs[x.struct] += 1
                    if list(x) != ['']: walk(x.struct, x)

    for base in packages(content):
        try:
            pkg, exps = read(base)
        except Exception as e:
            fails.append((base, e)); continue
        for x in exps:
            c = x.cls.rsplit('.', 1)[-1]
            classes[c] += 1
            walk(c, x.values)
    for title, cnt in (('classes', classes), ('structs', structs), ('properties', props)):
        print('-- %s' % title)
        for k, n in sorted(cnt.items()): print('  %5d  %s' % (n, k))
    for b, e in fails: print('FAILED %s: %s' % (b, e))


def packages(content):
    for dp, dn, fn in os.walk(content):
        dn.sort()
        for f in sorted(fn):
            if f.endswith('.uasset') and (f.startswith('BT_') or f.startswith('BB_')):
                yield os.path.join(dp, f)[:-7].replace('\\', '/')


# ---- writing: a package as AssetGen source

TREE, BOARD = '/Script/AIModule.BehaviorTree', '/Script/AIModule.BlackboardData'


def is_tree_or_board(base):
    pkg = invariants.Package(base)
    return bool(pkg.exports) and pkg.path(pkg.exports[0]['cls']) in (TREE, BOARD)


def ident(name):
    return re.sub(r'\W', '_', name)


class Emitter:
    """One package's UE_ASSET_DEFAULTS block. Every object becomes a local named as its export, declared where it is
    first needed: a tree's composite, then a nested block of its subtree closed by its Children; a blackboard's key
    types before Keys. What it cannot spell is a `// btdecomp:` line, and listed in .missed."""

    def __init__(s, api, base):
        s.api = api
        s.pkg, s.exps = read(base)
        s.by = {x.path: x for x in s.exps}
        s.asset = s.exps[0]
        s.declared = set()
        s.headers, s.assets, s.missed, s.lines = set(), {}, [], []

    # -- types

    def cls_cpp(s, path):
        top, _, name = path.partition('.')
        cpp = s.api.classes.get((top, name))
        if cpp is None: return None
        h = s.api.header_of(cpp)
        if h: s.headers.add(h)
        return cpp

    def member(s, cpp, name):
        """(C++ name, C++ type) of class cpp's member `name` (the engine's), through its bases."""
        c = cpp.split('::')[-1]
        while c:
            real = s.api.members(c).get(name.lower())
            if real: return real, s.api.member_type(c, real)
            c = s.api.bases.get(c)
        return None, None

    @staticmethod
    def inner(ty):
        m = re.match(r'\s*(?:TArray|TSet)<(.*)>\s*$', ty or '')
        return m.group(1).strip() if m else None

    # -- values

    def ref(s, v, ty):
        if v is None: return 'nullptr'
        if v in s.by:
            s.declare(s.by[v])
            return ident(s.by[v].name)
        if ty and ('TSubclassOf' in ty or 'UClass' in ty):
            cpp = s.cls_cpp(v)
            if cpp: return '%s::StaticClass()' % cpp
            s.missed.append('class ' + v)
            return 'nullptr /* btdecomp: class %s */' % v
        top, _, name = v.partition('.')            # another package's asset, or a native object (a UEnum)
        if top.startswith('/Script/'):
            k = next((k for k in range(len(s.pkg.imports)) if s.pkg.path(-k - 1) == v), None)
            c = s.pkg.imports[k]['class_name'] if k is not None else None
            cls = c and s.cls_cpp('/Script/CoreUObject.' + c)
            name = '%s_%s' % (name, c)
        else:
            cls = None if ':' in name else s.asset_class(top, name)
        if not cls:
            s.missed.append('object ' + v)
            return 'nullptr /* btdecomp: %s */' % v
        s.assets[name] = (cls, v)
        return '&' + ident(name)

    def asset_class(s, top, name):
        """The C++ class of another package's asset, read off its own package in the same Content folder or --game."""
        here = s.pkg.base.replace('\\', '/')
        roots = [here[:here.index('/Content/') + len('/Content')]] + invariants.GAME_CONTENT
        other = next((r + top[len('/Game'):] for r in roots if os.path.exists(r + top[len('/Game'):] + '.uasset')), None)
        if not other: return None
        o = invariants.Package(other)
        k = next((i for i, e in enumerate(o.exports) if e['name'] == name), None)
        return s.cls_cpp(o.path(o.exports[k]['cls'])) if k is not None else None

    def value(s, v, ty):
        """The C++ expression of a value of C++ type ty."""
        if isinstance(v, Ref) or v is None: return s.ref(v, ty)
        if isinstance(v, bool): return 'true' if v else 'false'
        if isinstance(v, int): return str(v)
        if isinstance(v, float): return B.cfloat(v)
        if isinstance(v, str): return B.cstring(v)
        if isinstance(v, list):
            it = s.inner(ty)
            return '{%s}' % ', '.join(s.value(x, it) for x in v)
        if isinstance(v, tuple) and v[0] == 'name': return 'FName()' if v[1] == 'None' else B.cstring(v[1])
        if isinstance(v, tuple) and v[0] == 'enum':
            m = re.match(r'\s*(?:TEnum|TEnumAsByte)<(\w+)>\s*$', ty or '') or re.match(r'\s*(E\w+)\s*$', ty or '')
            return '%s::%s' % (m.group(1), v[1].split('::')[-1]) if m else v[1]
        if isinstance(v, tuple) and v[0] == 'soft': return '{}' if v[1] is None else B.cstring(v[1])
        if isinstance(v, Struct): return s.struct(v)
        s.missed.append('value %s' % show(v))
        return '{} /* btdecomp: %s */' % show(v)

    def struct(s, v):
        """A whole struct value: its constructor over every field, or designated braces."""
        name = v.struct
        cpp = s.api.struct_cpp.get(name, 'F' + name)
        if list(v) == ['']:
            raw = v['']
            if name == 'GameplayTagContainer':
                return 'FGameplayTagContainer({%s}, {})' % ', '.join('FGameplayTag(%s)' % B.cstring(t) for t in raw)
            return '%s(%s)' % (cpp, ', '.join(B.cfloat(x) if isinstance(x, float) else str(x) for x in raw))
        fields = s.api.fields.get(name, {})
        order = s.api.ctor_fields.get(name)
        if order:
            return '%s(%s)' % (cpp, ', '.join(s.value(v[f], fields.get(f)) if f in v else '{}' for f in order))
        return '%s{%s}' % (cpp, ', '.join('.%s = %s' % (k, s.value(x, fields.get(k))) for k, x in v.items()))

    def assign(s, target, ty, v):
        """Statements setting target (`X->M` or `M`) to v. A struct written as tags is set member by member, so what its
        tags leave out keeps its default, as the cooked tags do."""
        if isinstance(v, Struct) and list(v) != ['']:
            fields = s.api.fields.get(v.struct, {})
            for k, x in v.items(): s.assign('%s.%s' % (target, k), fields.get(k), x)
            return
        s.lines.append('%s = %s;' % (target, s.value(v, ty)))

    # -- objects

    def members_of(s, x, cpp, skip=(), only=None):
        for k, v in x.values.items():
            if only is not None and k not in only: continue
            if k in skip or k in ('TreeAsset', 'ParentNode'): continue      # AssetGen derives these from the tree
            real, ty = s.member(cpp, re.sub(r'\[\d+\]$', '', k))
            if not real:
                s.missed.append('%s.%s' % (x.name, k))
                s.lines.append('// btdecomp: %s = %s' % (k, show(v)))
                continue
            s.assign(('%s->%s' % (ident(x.name), real)) if x is not s.asset else real, ty, v)

    def declare(s, x, skip=()):
        if x.path in s.declared: return
        s.declared.add(x.path)
        cpp = s.cls_cpp(x.cls)
        if not cpp:
            s.missed.append('class ' + x.cls)
            cpp = 'UObject /* btdecomp: %s */' % x.cls
        if x.outer == s.asset.path: outer = 'this'
        else:
            s.declare(s.by[x.outer])
            outer = ident(s.by[x.outer].name)
        s.lines.append('auto *%s = UE_INSTANCED(%s, %s, %s);' % (ident(x.name), cpp, outer, B.cstring(x.name)))
        s.members_of(x, cpp, skip)

    def composite(s, x):
        s.declare(x, skip=('Children',))
        s.subtree(x)

    def subtree(s, x):
        """A nested block: each child of composite x, with its decorators, then x's Children."""
        children = x.values.get('Children')
        if children is None: return
        s.lines.append('{')
        for c in children:
            for d in c.get('Decorators', []): s.ref(d, None)
            if c.get('ChildComposite'): s.composite(s.by[c['ChildComposite']])
            elif c.get('ChildTask'): s.ref(c['ChildTask'], None)
        real, ty = s.member(s.cls_cpp(x.cls), 'Children')
        s.assign('%s->%s' % (ident(x.name), real), ty, children)
        s.lines.append('}')

    def emit(s):
        cpp = s.cls_cpp(s.asset.cls)
        if s.asset.cls == TREE:
            s.members_of(s.asset, cpp, skip=('RootNode',))
            root = s.asset.values.get('RootNode')
            if root:
                s.declare(s.by[root], skip=('Children',))
                s.members_of(s.asset, cpp, only=('RootNode',))
                s.subtree(s.by[root])
        else:
            s.members_of(s.asset, cpp)
        for x in s.exps[1:]:
            if x.path not in s.declared: s.missed.append('unreached ' + x.name)
        return cpp

    def text(s):
        cpp = s.emit()
        out, depth = [], 1
        for l in s.lines:
            if l == '}': depth -= 1
            out.append('  ' * depth + l)
            if l == '{': depth += 1
        return 'UE_ASSET_DEFAULTS(%s, %s) {\n%s\n}\n' % (cpp, ident(s.asset.name), '\n'.join(out))


def source(api, bases, package=None):
    """(the .cpp of one or more packages, what it could not spell). UE_MOD_PACKAGE is the first one's folder unless
    package is given; another package's asset is a UE_ASSET_AT."""
    ems = [Emitter(api, b) for b in bases]
    blocks = [e.text() for e in ems]
    pkg = package or ems[0].pkg.package_name().rsplit('/', 1)[0]
    headers = set().union(*(e.headers for e in ems))
    own = {e.asset.name for e in ems}
    assets = {}
    for e in ems: assets.update((k, v) for k, v in e.assets.items() if k not in own)
    inc = ['#include "UeApi/Types.h"', ''] + ['#include "UeApi/%s"' % h for h in sorted(headers)]
    at = ['UE_ASSET_AT(::%s, %s, %s);' % (c.lstrip(':'), ident(n), B.cstring(p)) for n, (c, p) in sorted(assets.items())]
    head = '// Generated by AssetGen/tools/btdecomp.py from %s.\n' % ', '.join(e.pkg.package_name() for e in ems)
    return (head + '\n'.join(inc) + '\n\nUE_MOD_PACKAGE(%s);\n\n' % B.cstring(pkg) + ('\n'.join(at) + '\n\n' if at else '')
            + '\n'.join(blocks)), sum((e.missed for e in ems), [])


def take_game(args):
    while '--game' in args:
        i = args.index('--game')
        invariants.GAME_CONTENT.append(args[i + 1].replace(os.sep, '/').rstrip('/'))
        del args[i:i + 2]
    return args


def emit_cmd(args):
    args = take_game(list(args))
    out = args[args.index('--out') + 1]
    package = args[args.index('--package') + 1] if '--package' in args else None
    bases = [re.sub(r'\.(uasset|uexp)$', '', a) for a in args if not a.startswith('--') and a not in (out, package)]
    text, missed = source(Api(UEAPI), bases, package)
    open(out, 'w', encoding='utf-8').write(text)
    for m in missed: print('missed: ' + m)


# ---- the round trip: emit, compile, compare by meaning

# What the round trip may change, each explained, not a difference:
#   transactional  RF_Transactional (0x8) on an export: some trees were saved without it. It is the editor's undo
#                  flag; the game never reads it. AssetGen writes it on every asset and object, as most trees have it.
#   name case      a tag's or an object's name in another case (`range` for `Range`, `TSK_..._WithinHeight_C_0` for
#                  `TSK_..._WIthinHeight_C_0`): both are FNames, which match without case. UeApi spells a Blueprint
#                  variable as the dump's name table first saw it, and a package's name table holds one spelling.
#   empty struct   a struct tag with no members: written when the value differs from the class default only in a
#                  transient member (a key selector's AllowedTypes, filled by the node's constructor). It loads
#                  nothing, so the object keeps its archetype's value, as with no tag.
#   enum prefix    a value of an enum that is no `enum class` (ECollisionChannel) written `ECollisionChannel::ECC_Visibility`
#                  where the cook writes `ECC_Visibility`: UEnum::GetIndexByName loads it by either name.
EXPLAINED = ('transactional', 'name case', 'empty struct', 'enum prefix')


def differences(orig, made):
    """(what the compiled package says otherwise than the original, Counter of the explained differences)."""
    a, b = read(orig)[1], read(made)[1]
    ea, eb = {x.name.lower(): x for x in a}, {x.name.lower(): x for x in b}
    out, ok = ['export %s missing' % n for n in ea if n not in eb] + ['export %s extra' % n for n in eb if n not in ea], Counter()
    for n in ea:
        if n not in eb: continue
        x, y = ea[n], eb[n]
        if x.name != y.name: ok['name case'] += 1
        for what in ('cls', 'outer'):
            if not same(getattr(x, what), getattr(y, what)): out.append('%s: %s %r, made %r' % (n, what, getattr(x, what), getattr(y, what)))
        if x.flags != y.flags:
            if x.flags | 0x8 == y.flags | 0x8: ok['transactional'] += 1
            else: out.append('%s: flags 0x%x, made 0x%x' % (n, x.flags, y.flags))
        vx, vy = values(x, ok), values(y, ok)
        ok['name case'] += len({k for k in x.values if k.lower() in vy} - set(y.values))
        for k in list(vx) + [k for k in vy if k not in vx]:
            ex, ey = vx.get(k), vy.get(k)
            if isinstance(ex, tuple) and isinstance(ey, tuple) and ex[0] == ey[0] == 'enum' and ex != ey                     and '::' not in ex[1] and ey[1].endswith('::' + ex[1]):
                ok['enum prefix'] += 1
                continue
            if not same(vx.get(k, '<unset>'), vy.get(k, '<unset>')):
                out.append('%s.%s: %s, made %s' % (n, k, show(vx.get(k, '<unset>')), show(vy.get(k, '<unset>'))))
    return out, ok


def values(x, ok):
    """An export's values keyed by lowercase name, empty struct tags left out."""
    out = {}
    for k, v in x.values.items():
        if isinstance(v, Struct) and not v: ok['empty struct'] += 1; continue
        out[k.lower()] = v
    return out


def same(x, y):
    if isinstance(x, Struct) and isinstance(y, Struct):
        kx, ky = [k.lower() for k in x], [k.lower() for k in y]
        return x.struct == y.struct and kx == ky and all(same(a, b) for a, b in zip(x.values(), y.values()))
    if isinstance(x, list) and isinstance(y, list): return len(x) == len(y) and all(map(same, x, y))
    if isinstance(x, str) and isinstance(y, str) and type(x) is type(y) and x != y and x.lower() == y.lower():
        return isinstance(x, Ref)       # an object path is FNames, which match without case; a string value does not
    return type(x) is type(y) and x == y


def roundtrip(args):
    args = take_game(list(args))
    content = args[0].replace('\\', '/').rstrip('/')
    only = [args[i + 1] for i, a in enumerate(args) if a == '--only']
    exe = args[args.index('--assetgen') + 1] if '--assetgen' in args else os.path.join(AG, '..', 'x64', 'Release', 'assetgen.exe')
    ueapi = args[args.index('--ueapi') + 1] if '--ueapi' in args else UEAPI
    keep = args[args.index('--keep') + 1] if '--keep' in args else None
    work = keep or tempfile.mkdtemp(prefix='btrt')
    api = Api(ueapi)
    bases = [b for b in packages(content) if (not only or any(o in b for o in only)) and is_tree_or_board(b)]

    def one(k_base):
        k, base = k_base
        name = base.rsplit('/', 1)[-1]
        try:
            text, missed = source(api, [base])
        except Exception as e:
            return base, ['emit failed: %r' % e], [], Counter()
        d = os.path.join(work, str(k))
        os.makedirs(d, exist_ok=True)
        src = os.path.join(d, name + '.cpp')
        open(src, 'w', encoding='utf-8').write(text)
        folder = base[len(content):].rsplit('/', 1)[0]
        out = os.path.join(d, 'FSD', 'Content', *folder.strip('/').split('/'))
        os.makedirs(out, exist_ok=True)
        p = subprocess.run([exe, 'compile', src, ueapi, out], capture_output=True, text=True, encoding='utf-8', errors='replace')
        if p.returncode != 0:
            return base, ['compile failed: ' + (p.stdout + p.stderr).strip().splitlines()[-1]], missed, Counter()
        diffs, ok = differences(base, os.path.join(out, name))
        return base, diffs, missed, ok

    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max(1, (os.cpu_count() or 2) // 2)) as pool:
        results = list(pool.map(one, enumerate(bases)))
    exact, explained = 0, Counter()
    for base, diffs, missed, ok in results:
        explained.update(ok)
        if not diffs and not missed: exact += 1; continue
        print('== %s' % base[len(content):])
        for m in missed: print('   missed: %s' % m)
        for d in diffs: print('   %s' % d)
    print('explained: %s' % ', '.join('%s %d' % (k, explained[k]) for k in EXPLAINED))
    print('%d of %d packages equal (%s)' % (exact, len(results), work))
    if not keep and exact == len(results): shutil.rmtree(work, ignore_errors=True)
    return 0 if exact == len(results) else 1


COMMANDS = {'dump': dump, 'census': census, 'emit': emit_cmd, 'roundtrip': roundtrip}

if __name__ == '__main__':
    if len(sys.argv) < 3 or sys.argv[1] not in COMMANDS: sys.exit(__doc__)
    sys.exit(COMMANDS[sys.argv[1]](sys.argv[2:]))
