import sys
from invariants import *
"""Operand rules: what the VM does with the operands a script names - a code offset it jumps to, a property it
offsets an object by, a function it calls by pointer or finds by name, the arguments it steps into the callee's
parameters - checked against the object each one runs on. Each rule cites the UE 4.27 source that makes it one.

The engine's own classes are not on disk: their supers, properties, functions, function flags and parameter flags are
read once off the Dumper-7 dump of the same game build (NATIVE_DUMP), cached beside this file."""
import glob, os, pickle, re
import invariants


# ---- the engine's own classes, read off the Dumper-7 dump

NATIVE_DUMP = SDK_DUMP          # the dump whose SDK/SDK and GObjects-Dump-WithProperties.txt describe /Script
FUNC_BITS = {"Final": 0x1, "RequiredAPI": 0x2, "BlueprintAuthorityOnly": 0x4, "BlueprintCosmetic": 0x8, "Net": 0x40,
             "NetReliable": 0x80, "NetRequest": 0x100, "Exec": 0x200, "Native": 0x400, "Event": 0x800,
             "NetResponse": 0x1000, "Static": 0x2000, "NetMulticast": 0x4000, "UbergraphFunction": 0x8000,
             "MulticastDelegate": 0x10000, "Public": 0x20000, "Private": 0x40000, "Protected": 0x80000,
             "Delegate": 0x100000, "NetServer": 0x200000, "HasOutParams": 0x400000, "HasDefaults": 0x800000,
             "NetClient": 0x1000000, "DLLImport": 0x2000000, "BlueprintCallable": 0x4000000,
             "BlueprintEvent": 0x8000000, "BlueprintPure": 0x10000000, "EditorOnly": 0x20000000,
             "Const": 0x40000000, "NetValidate": 0x80000000}
CPF_BITS = {'Parm': 0x80, 'OutParm': 0x100, 'ReturnParm': 0x400, 'ReferenceParm': 0x8000000, 'ConstParm': 0x2}


# The tables are plain SimpleNamespaces, never classes of this module: the cache pickles them, and a class defined here
# would pickle only when this file is imported under a name sys.modules knows (a sibling loading it with
# spec_from_file_location is not), leaving a truncated cache and every rule raising.
from types import SimpleNamespace as _NS


def NFunc():
    """A native UFunction: flags (None when the SDK has no flag line for it) and its properties in order, each
    [engine name, FProperty class, CPF flags or None, the SDK's C++ type or None]."""
    return _NS(flags=None, props=[], kind=None)


def NStruct():
    """A native UClass or UScriptStruct: its super's path, own properties (engine name -> FProperty class), own
    functions (name -> path) and, for a class, whether it is an interface."""
    return _NS(super=None, props={}, funcs={}, interface=False, cpp=None, kind=None, ctypes={})


def Native():
    return _NS(structs={}, funcs={}, cpp={})


def _build_native(dump):
    nat = Native()
    if not dump: return nat                     # no dump on this machine: every /Script object reads as unknown
    sdk = os.path.join(dump, 'SDK', 'SDK')
    # Supers: `// Class /Script/Engine.Actor`, the size line, then `class AActor : public UObject`. Dumper-7 drops an
    # interface's UInterface base, so an I-class without one is an interface over /Script/CoreUObject.Interface.
    head = re.compile(r'^// (Class|ScriptStruct) (/Script/\S+)\n//[^\n]*\n(?:#pragma[^\n]*\n)*(?:class|struct) '
                      r'(?:(?:alignas|SDK_ALIGN)\(\w+\) )?((?:\w+::)*\w+)(?: final)?(?: : public ((?:\w+::)*\w+))?', re.M)
    bases = {}
    for name in os.listdir(sdk):
        if not name.endswith(('_classes.hpp', '_structs.hpp')): continue
        for m in head.finditer(open(os.path.join(sdk, name), encoding='utf-8', errors='replace').read()):
            st = nat.structs.setdefault(m.group(2), NStruct())
            st.cpp, st.kind = m.group(3).split('::')[-1], m.group(1)
            nat.cpp[st.cpp] = m.group(2)
            bases[m.group(2)] = m.group(4)
            if m.group(1) == 'Class' and st.cpp.startswith('I') and not m.group(4) and m.group(2) != '/Script/CoreUObject.Interface':
                st.interface = True
    # Member C++ types, for the class an object member points at: `<tab>class USceneComponent* RootComponent; // 0x..`.
    member = re.compile(r'^\t(.+?)\s+(\w+)(?:\[\w+\])?;\s+// 0x')
    for name in os.listdir(sdk):
        if not name.endswith(('_classes.hpp', '_structs.hpp')): continue
        cur = None
        for text in open(os.path.join(sdk, name), encoding='utf-8', errors='replace'):
            m = re.match(r'^// (?:Class|ScriptStruct) (/Script/\S+)$', text.rstrip())
            if m: cur = nat.structs.get(m.group(1)); continue
            m = member.match(text.rstrip('\n')) if cur else None
            if m: cur.ctypes[m.group(2)] = m.group(1)
    for path, base in bases.items():
        if base: nat.structs[path].super = nat.cpp.get(base.split('::')[-1], '?' + base)     # '?': unreadable
        elif nat.structs[path].interface: nat.structs[path].super = '/Script/CoreUObject.Interface'
    # Properties and parameters by their engine names, in ChildProperties order: the object dump.
    line = re.compile(r'^\[\w+\] \{0x\w+\} (\w+) (/Script/[^\s]+)$|^\[\w+\] \{0x\w+\}     (\w+) (.+)$')
    cur = None
    for text in open(os.path.join(dump, 'GObjects-Dump-WithProperties.txt'), encoding='utf-8', errors='replace'):
        m = line.match(text.rstrip('\n'))
        if not m: cur = None; continue
        if m.group(1):
            kind, full = m.group(1), m.group(2)
            cur = None
            if kind in ('Class', 'ScriptStruct'):
                cur = nat.structs.setdefault(full, NStruct()).props
            elif kind in ('Function', 'DelegateFunction', 'SparseDelegateFunction'):
                pkg, _, rest = full.partition('.')
                cls, dot, fn = rest.partition('.')
                path = (pkg + '.' + cls + ':' + fn) if dot else full
                f = nat.funcs.setdefault(path, NFunc())
                f.kind = kind
                if dot: nat.structs.setdefault(pkg + '.' + cls, NStruct()).funcs[fn] = path
                cur = f.props
        elif cur is not None:
            if isinstance(cur, dict): cur[m.group(4)] = m.group(3)
            else: cur.append([m.group(4), m.group(3), None, None])
    # Function flags and parameter flags: the SDK's `// Function Pkg.Class.Name`, `// (Flags)`, `// Parameters:`.
    fhead = re.compile(r'^// Function ([^.\n]+)\.([^.\n]+)\.([^\n]+)\n// \(([^)]*)\)\n((?:// [^\n]*\n)*)', re.M)
    pline = re.compile(r'^// (.+?)\s+(\S+)\s+\(([^)]*)\)$')
    for name in os.listdir(sdk):
        if not name.endswith('_functions.cpp'): continue
        for m in fhead.finditer(open(os.path.join(sdk, name), encoding='utf-8', errors='replace').read()):
            path = '/Script/%s.%s:%s' % (m.group(1), m.group(2), m.group(3).rstrip())
            f = nat.funcs.get(path)
            if f is None: continue                        # a /Game class's function: its package has it
            f.flags = sum(FUNC_BITS.get(n, 0) for n in m.group(4).split(', ') if n)
            parms = [pline.match(l) for l in m.group(5).splitlines()[1:]]
            parms = [p for p in parms if p]
            if len(parms) == len(f.props):
                for p, q in zip(f.props, parms):
                    p[2] = sum(CPF_BITS.get(n, 0) for n in q.group(3).split(', '))
                    p[3] = q.group(1)
    return nat


_OPERAND_NATIVE = []


def native_tables():
    """The Native tables of NATIVE_DUMP, built once and cached in a pickle in the temp folder (never in the repo). The
    cache is written to a side file and renamed over, so a process reading it meanwhile never sees half of one, and a
    cache that cannot be written only costs the next run a rebuild."""
    if _OPERAND_NATIVE: return _OPERAND_NATIVE[0]
    import tempfile
    cache = os.path.join(tempfile.gettempdir(), 'invariants_native_v2_%s.pickle' % re.sub(r'\W', '_', os.path.basename(NATIVE_DUMP or 'none')))
    try:
        with open(cache, 'rb') as f: nat = pickle.load(f)
        if not isinstance(nat, _NS) or not hasattr(nat, 'structs'): raise ValueError('not a table')
    except Exception:
        nat = _build_native(NATIVE_DUMP)
        try:
            side = '%s.%d.tmp' % (cache, os.getpid())
            with open(side, 'wb') as f: pickle.dump(nat, f)
            os.replace(side, cache)
        except Exception:
            pass
    _OPERAND_NATIVE.append(nat)
    return nat


# ---- one handle on a class, struct or function, wherever it lives

class R:
    """A class, struct or function: export i of a package on disk, or a /Script object by path. UNKNOWN stands in
    for one that cannot be read (a /Game package not found, a /Script object the dump does not have)."""
    __slots__ = ('pkg', 'i', 'path')
    def __init__(s, pkg=None, i=None, path=None): s.pkg, s.i, s.path = pkg, i, path
    def key(s): return s.path if s.path else (os.path.normcase(os.path.abspath(s.pkg.base)), s.i)
    def __eq__(s, o): return isinstance(o, R) and s.key() == o.key()
    def __hash__(s): return hash(s.key())
    def __repr__(s): return s.path or '%s.%s' % (s.pkg.package_name(), s.pkg.exports[s.i]['name'])
    @property
    def name(s): return re.split(r'[.:]', s.path)[-1] if s.path else s.pkg.exports[s.i]['name']


UNKNOWN = R(path='?unknown')
CLASS_KINDS = ('Class', 'BlueprintGeneratedClass', 'WidgetBlueprintGeneratedClass', 'AnimBlueprintGeneratedClass')
STRUCT_KINDS = CLASS_KINDS + ('ScriptStruct', 'UserDefinedStruct') + Package.FUNCTION_CLASSES
OPERAND_STATS = {}                  # rule -> {what: count}: how much each rule checked, and how much it could not see


def stat(rule_name, what, n=1):
    d = OPERAND_STATS.setdefault(rule_name, {})
    d[what] = d.get(what, 0) + n


def _resolve_beside(pkg, idx, name):
    """A /Game import of a mod built beside this one: test_bytecode.py compiles each mod into <root>/<Mod>/FSD/Content,
    so LibraryUser's MathLib sits in a sibling Content folder, where Package.resolve does not look."""
    here = pkg.base.replace(os.sep, '/')
    cut = here.rfind('/FSD/Content/')
    if cut < 0: return None
    want = pkg.path(idx).lower()
    for root in glob.glob(os.path.dirname(here[:cut]) + '/*/FSD/Content'):
        if not os.path.exists(root + name[len('/Game'):] + '.uasset'): continue
        other = invariants.load(root + name[len('/Game'):])
        k = next((k for k in range(len(other.exports)) if other.path(k + 1).lower() == want), None)
        if k is not None: return other, k
    return None


def ref(pkg, idx):
    """The R an FPackageIndex of pkg names: an export, a /Script import by path, a /Game import found on disk
    (Package.resolve), UNKNOWN for one not found; None for index 0."""
    if not idx: return None
    memo = pkg.__dict__.setdefault('_operand_refs', {})
    if idx not in memo:
        if idx > 0: r = R(pkg, idx - 1)
        else:
            top = idx
            while pkg.obj(top)['outer']: top = pkg.obj(top)['outer']
            if pkg.obj(top)['name'].startswith('/Game/'):
                try: got = pkg.resolve(idx) or _resolve_beside(pkg, idx, pkg.obj(top)['name'])
                except Exception: got = None
                r = R(*got) if got else UNKNOWN
            else: r = R(path=pkg.path(idx))
        memo[idx] = r
    return memo[idx]


def kind_of(r):
    """The class of the object r is: Function, BlueprintGeneratedClass, Class, ScriptStruct, ...; None if unknown."""
    if r is None or r is UNKNOWN: return None
    if r.path:
        nat = native_tables()
        if r.path in nat.funcs: return nat.funcs[r.path].kind
        st = nat.structs.get(r.path)
        return st.kind if st else None
    return r.pkg.class_of(r.i + 1)


def body(r):
    return r.pkg.struct(r.i) if r is not None and r is not UNKNOWN and not r.path else None


def is_interface(r):
    if r is None or r is UNKNOWN: return False
    if r.path:
        st = native_tables().structs.get(r.path)
        return bool(st and st.interface)
    b = body(r)
    return bool(b and getattr(b, 'class_flags', 0) & 0x4000)


def super_chain(r):
    """r and its supers, nearest first; ends in UNKNOWN when a super cannot be read."""
    out, seen = [], set()
    while r is not None and r not in seen:
        out.append(r); seen.add(r)
        if r is UNKNOWN: break
        if r.path:
            st = native_tables().structs.get(r.path)
            if st is None: out.append(UNKNOWN); break
            r = R(path=st.super) if st.super else None
        else:
            b = body(r)
            r = (ref(r.pkg, b.super) or None) if b is not None and b.super else None
    return out


class PI:
    """One property as a rule needs it: name, FProperty class, CPF flags (None if unknown), and how to find the class
    it refers to - the package and Prop of a cooked one, the SDK's C++ type of a native one."""
    def __init__(s, name, type, flags, pkg=None, prop=None, ctype=None):
        s.name, s.type, s.flags, s.pkg, s.prop, s.ctype = name, type, flags, pkg, prop, ctype
    def __repr__(s): return '%s %s' % (s.type, s.name)


def own_props(r):
    """lower-cased name -> PI of r's own properties (a class's, a struct's or a function's), or None if r cannot be
    read. An FName compares case-insensitively, and the game's packages spell a native's names their own way."""
    if r is None or r is UNKNOWN: return None
    if r.path:
        nat = native_tables()
        if r.path in nat.funcs:
            return {n.lower(): PI(n, t, f, ctype=c) for n, t, f, c in nat.funcs[r.path].props}
        st = nat.structs.get(r.path)
        return {n.lower(): PI(n, t, None, ctype=st.ctypes.get(n)) for n, t in st.props.items()} if st else None
    b = body(r)
    return {p.name.lower(): PI(p.name, p.type, p.flags, r.pkg, p) for p in b.props} if b is not None else None


def find_prop(r, name):
    """(owner R, PI) of the property `name` on r or a super - FindFProperty walks the supers too (Class.cpp) -,
    None if there is none, UNKNOWN if a super on the way cannot be read."""
    for c in super_chain(r):
        props = own_props(c)
        if props is None: return UNKNOWN
        if name.lower() in props: return c, props[name.lower()]
    return None


def _native_iface_funcs():
    nat = native_tables()
    if not hasattr(nat, '_iface_funcs'):
        nat._iface_funcs = {f.lower() for st in nat.structs.values() if st.interface for f in st.funcs}
    return nat._iface_funcs


def own_funcs(r):
    """lower-cased name -> R of r's own functions: a cooked class's FuncMap (serialized as is, Class.cpp 4413), a native's."""
    if r.path:
        st = native_tables().structs.get(r.path)
        return {n.lower(): R(path=p) for n, p in st.funcs.items()} if st else None
    b = body(r)
    return {n.lower(): ref(r.pkg, v) for n, v in b.func_map} if b is not None and hasattr(b, 'func_map') else None


def interfaces_of(r):
    b = body(r)
    return [ref(r.pkg, c) for c, _, _ in b.interfaces] if b is not None and hasattr(b, 'interfaces') else []


def find_func(r, name, _depth=0):
    """What UClass::FindFunctionByName finds for `name` on class r (Class.cpp 5281-5323): its own FuncMap, then each
    interface it lists, then its super. None if nothing; UNKNOWN if a class on the way cannot be read, or the walk
    reaches a native class that might list a native interface (the dump does not say which) that has the name."""
    native_seen = False
    for c in super_chain(r):
        if c is UNKNOWN: return UNKNOWN
        funcs = own_funcs(c)
        if funcs is None: return UNKNOWN
        if name.lower() in funcs: return funcs[name.lower()]
        for iface in interfaces_of(c):
            if iface is None or iface is UNKNOWN: return UNKNOWN
            got = find_func(iface, name, _depth + 1) if _depth < 8 else UNKNOWN
            if got is not None: return got
        native_seen = native_seen or bool(c.path)
    if native_seen and name.lower() in _native_iface_funcs(): return UNKNOWN
    return None


class FI:
    """A function as a call needs it: flags (None if unknown), parameters in order (PI, the return value included),
    its outer class (None for a package's delegate signature), and whether it runs script."""
    def __init__(s, r, flags, params, outer, kind):
        s.r, s.flags, s.params, s.outer, s.kind = r, flags, params, outer, kind
    def is_return(s, p): return p.flags & CPF_ReturnParm if p.flags is not None else p.name == 'ReturnValue'
    @property
    def args(s): return [p for p in s.params if not s.is_return(p)]
    @property
    def ret(s): return next((p for p in s.params if s.is_return(p)), None)
    @property
    def native(s): return s.r.path is not None or (s.flags is not None and s.flags & 0x400)


def finfo(r):
    """FI of function r, or None when r is not a function this can read."""
    if r is None or r is UNKNOWN: return None
    if r.path:
        f = native_tables().funcs.get(r.path)
        if f is None: return None
        outer = R(path=r.path.rsplit(':', 1)[0]) if ':' in r.path else None
        return FI(r, f.flags, [PI(n, t, fl, ctype=c) for n, t, fl, c in f.props], outer, f.kind)
    b = body(r)
    if b is None or not hasattr(b, 'function_flags'): return None
    outer = ref(r.pkg, r.pkg.exports[r.i]['outer'])
    return FI(r, b.function_flags, [PI(p.name, p.type, p.flags, r.pkg, p) for p in b.props if p.flags & CPF_Parm],
              outer, r.pkg.class_of(r.i + 1))


# ---- static types: what an expression holds, as far as the package and the dump say

def _ctype(t):
    """A static type off an SDK C++ spelling: ('obj', R) / ('class', R) / ('iface', R) / ('array', T) / ('struct', R)."""
    if not t: return None
    t = re.sub(r'^const\s+', '', t.strip()).rstrip('&').strip()
    cpp = native_tables().cpp
    m = re.match(r'^TArray<(.*)>\*?$', t)
    if m:
        inner = _ctype(m.group(1))
        return ('array', inner) if inner else None
    for pat, k in ((r'^(?:TSubclassOf|TSoftClassPtr)<class (\w+)>\*?$', 'class'), (r'^TScriptInterface<class (\w+)>\*?$', 'iface'),
                   (r'^(?:TWeakObjectPtr|TLazyObjectPtr|TSoftObjectPtr)<class (\w+)>\*?$', 'obj'),
                   (r'^class (\w+)\*\*?$', 'obj'), (r'^struct (\w+)\*?$', 'struct')):
        m = re.match(pat, t)
        if m: return (k, R(path=cpp[m.group(1)])) if m.group(1) in cpp else None
    return None


OPERAND_OBJECT_PROPS = ('ObjectProperty', 'WeakObjectProperty', 'LazyObjectProperty', 'SoftObjectProperty')


def ptype(pi):
    """The static type a property holds."""
    if pi is None or pi is UNKNOWN: return None
    if pi.prop is None: return _ctype(pi.ctype)
    p, pkg = pi.prop, pi.pkg

    def of(p):
        if p.type in OPERAND_OBJECT_PROPS: return ('obj', ref(pkg, p.ref))
        if p.type in ('ClassProperty', 'SoftClassProperty'): return ('class', ref(pkg, p.meta))
        if p.type == 'InterfaceProperty': return ('iface', ref(pkg, p.ref))
        if p.type == 'StructProperty': return ('struct', ref(pkg, p.ref))
        if p.type == 'ArrayProperty' and p.subs:
            inner = of(p.subs[0])
            return ('array', inner) if inner else None
        return None
    t = of(p)
    return None if t and t[0] != 'array' and t[1] in (None, UNKNOWN) else t


class Ctx:
    """The function a rule is walking: its package, export index, class, and own properties."""
    def __init__(s, pkg, i):
        s.pkg, s.i = pkg, i
        s.cls = ref(pkg, pkg.exports[i]['outer'])
        s.props = {p.name: PI(p.name, p.type, p.flags, pkg, p) for p in pkg.struct(i).props}
        s.self = ('obj', s.cls) if kind_of(s.cls) in CLASS_KINDS else None


def object_class(pkg, idx):
    """The static type of the object an ObjectConst names: a class is ('class', itself); anything else ('obj', its class)."""
    o = pkg.obj(idx)
    if o is None: return None
    if idx > 0:
        return ('class', R(pkg, idx - 1)) if kind_of(R(pkg, idx - 1)) in CLASS_KINDS else ('obj', ref(pkg, o['cls']))
    if o['class_name'] in CLASS_KINDS: return ('class', ref(pkg, idx))
    want = o['class_package'] + '.' + o['class_name']
    if want.startswith('/Script/'): return ('obj', R(path=want))
    k = next((k for k in range(len(pkg.imports)) if pkg.path(-k - 1) == want), None)
    return ('obj', ref(pkg, -k - 1)) if k is not None else None


def operand_call_target(ctx, n, this):
    """The R a call node reaches: by pointer, the function it names; by name, what FindFunctionByName finds on the
    class of the object it runs on (UNKNOWN / None as find_func says)."""
    if n.op in (0x1C, 0x46, 0x68, 0x63): return ref(ctx.pkg, n.ops[0][1])
    if n.op in (0x1B, 0x45):
        if this is None or this[1] in (None, UNKNOWN): return UNKNOWN
        return find_func(this[1], n.ops[0][1])
    return None


def etype(ctx, n, this):
    """The static type of expression n evaluated on an object of static type `this` (None: unknown)."""
    o, pkg = n.op, ctx.pkg
    try:
        if o in (0x00, 0x48): return ptype(ctx.props.get(n.ops[0][1][-1]))
        if o in (0x01, 0x02, 0x42):
            got = find_prop(ref(pkg, n.ops[0][2]), n.ops[0][1][-1])
            return ptype(got[1]) if got and got is not UNKNOWN else None
        if o == 0x17: return this
        if o in (0x19, 0x1A): return etype(ctx, n.kids[1], context_this(ctx, n, this))
        if o == 0x12: return etype(ctx, n.kids[1], context_this(ctx, n, this))
        if o == 0x51: return etype(ctx, n.kids[0], this)
        if o in (0x1B, 0x1C, 0x45, 0x46, 0x68):
            f = finfo(operand_call_target(ctx, n, this))
            return ptype(f.ret) if f and f.ret else None
        if o == 0x20: return object_class(pkg, n.ops[0][1])
        if o in (0x2E, 0x55): return ('obj', ref(pkg, n.ops[0][1]))
        if o == 0x13: return ('class', ref(pkg, n.ops[0][1]))
        if o in (0x52, 0x54): return ('iface', ref(pkg, n.ops[0][1]))
        if o == 0x6B:
            t = etype(ctx, n.kids[0], this)
            return t[1] if t and t[0] == 'array' else None
    except (IndexError, KeyError, TypeError):
        return None
    return None


def context_this(ctx, n, this):
    """The static type of the object a context node's inner expression runs on: its object expression's value (for
    EX_ClassContext, the CDO of the class it yields). ProcessContextOpcode steps the object expression with the
    enclosing `this` and only the one inner expression with the new context (ScriptCore.cpp 2884, 2893, 2213-2223)."""
    t = etype(ctx, n.kids[0], this)
    if t is None or t[1] in (None, UNKNOWN): return None
    if n.op == 0x12: return ('obj', t[1]) if t[0] == 'class' else None
    return t if t[0] in ('obj', 'iface') else None


def runs_on(ctx, n, this):
    """(node, static type of the object it runs on) for n and everything under it. Only EX_Context's /
    EX_ClassContext's inner expression runs on the context object, and an EX_InterfaceContext's or a context's object
    expression on the enclosing one; every other operand - a call's arguments under a context included - is stepped
    with Stack.Object, the frame's own object (ScriptCore.cpp 868/899, 2199, 2213, 2884, 2893, 2966)."""
    yield n, this
    if n.op in (0x19, 0x1A, 0x12):
        yield from runs_on(ctx, n.kids[0], this)
        yield from runs_on(ctx, n.kids[1], context_this(ctx, n, this))
    elif n.op == 0x51:
        yield from runs_on(ctx, n.kids[0], this)
    else:
        for k in n.kids: yield from runs_on(ctx, k, ctx.self)


def walk_on(pkg, i):
    """(ctx, node, this) for every expression of function export i."""
    ctx = Ctx(pkg, i)
    for t in pkg.script(i):
        for n, this in runs_on(ctx, t, ctx.self): yield ctx, n, this


def is_ubergraph(pkg, i):
    return pkg.class_of(i + 1) == 'Function' and pkg.struct(i).function_flags & 0x8000


def entries(pkg, i):
    """The offsets the VM may enter ubergraph export i at: its top-level statements, less the prologue up to and
    including its EX_ComputedJump and less EX_EndOfScript."""
    top = pkg.script(i)
    cj = next((t.mem for t in top if t.op == 0x4E), -1)
    return {t.mem for t in top if t.mem > cj and t.op != 0x53}


def args_of(n):
    kids = n.kids[1:] if n.op == 0x63 else n.kids
    return [k for k in kids if k.op != 0x16]


# ---- the rules

@rule
def jump_targets_executable(pkg):
    """EX_Jump, EX_JumpIfNot and EX_PushExecutionFlow land on a statement of their own script that can run: not on
    EX_EndOfScript, whose handler is UE_LOG Fatal "Execution beyond end of script" (ScriptCore.cpp execEndOfScript
    2259-2273). The VM sets Code = &Script[Offset] with no check (execJump 2374, execJumpIfNot 2396,
    execPushExecutionFlow 2445, popped by execPopExecutionFlow 2453-2462); the editor only targets statement labels
    (KismetCompilerVMBackend.cpp 2135-2154)."""
    for i, st in functions(pkg):
        top = pkg.script(i)
        starts = {t.mem for t in top if t.op != 0x53}
        for t in top:
            for n in t.walk():
                if n.op in (0x06, 0x07, 0x4C):
                    stat('jump_targets_executable', 'jumps')
                    if n.ops[0][1] not in starts:
                        yield i, 'op %02x at mem %d goes to %d, %s' % (n.op, n.mem, n.ops[0][1], 'EX_EndOfScript' if
                            n.ops[0][1] in {t.mem for t in top} else 'not a statement')


@rule
def return_only_top_level(pkg):
    """EX_Return is a statement, never an operand: ProcessLocalScriptFunction stops only when the next statement's
    opcode is EX_Return (ScriptCore.cpp 1093-1133) and GNatives has no handler for it, so a nested one runs
    execUndefined ("Unknown code token 04", 2045-2048) and its operand then runs as code."""
    for i, st in functions(pkg):
        for t in pkg.script(i):
            for n in t.walk():
                if n is not t and n.op == 0x04:
                    yield i, 'EX_Return at mem %d inside the statement at mem %d' % (n.mem, t.mem)


@rule
def ubergraph_entry_offsets(pkg):
    """Every entry into an ubergraph is a statement after its prologue: an event stub passes the offset to
    ExecuteUbergraph_X as its one EX_IntConst argument, and a function's EventGraphCallOffset names it. The
    ubergraph's EX_ComputedJump sets Code = &Script[EntryPoint] with only a bounds check (ScriptCore.cpp 2384-2395);
    the editor patches each stub's literal from UbergraphStatementLabelMap (KismetCompilerVMBackend.cpp 1176-1185), a
    statement label of the event's first node (KismetCompilerMisc.cpp 2105-2111). Entering the prologue re-runs the
    EX_ComputedJump with the same EntryPoint, forever; EX_EndOfScript is Fatal."""
    for i, st in functions(pkg):
        for n in (x for t in pkg.script(i) for x in t.walk()):
            if n.op not in (0x1C, 0x46) or n.ops[0][1] <= 0: continue
            u = n.ops[0][1] - 1
            if pkg.exports[u]['outer'] != pkg.exports[i]['outer'] or not is_ubergraph(pkg, u): continue
            stat('ubergraph_entry_offsets', 'stub calls')
            a = args_of(n)
            if len(a) != 1 or a[0].op != 0x1D:
                yield i, 'call at mem %d into %s passes %s, not one EX_IntConst' % (
                    n.mem, pkg.exports[u]['name'], ['%02x' % k.op for k in a])
            elif a[0].ops[0][1] not in entries(pkg, u):
                yield i, 'call at mem %d enters %s at %d, not a statement after its prologue' % (
                    n.mem, pkg.exports[u]['name'], a[0].ops[0][1])
        if st.event_graph > 0:
            stat('ubergraph_entry_offsets', 'EventGraphCallOffsets')
            u = st.event_graph - 1
            if not is_ubergraph(pkg, u) or st.event_graph_offset not in entries(pkg, u):
                yield i, 'EventGraphCallOffset %d into %s, not an entry' % (st.event_graph_offset, pkg.exports[u]['name'])


@rule
def latent_linkage_offsets(pkg):
    """A latent call resumes where it was made. FLatentActionManager::ProcessLatentActions calls
    CallbackTarget->ProcessEvent(FindFunction(ExecutionFunction), &LinkID) (LatentActionManager.cpp 206-222): an
    ubergraph takes LinkID as its EntryPoint and its EX_ComputedJump jumps there unchecked (ScriptCore.cpp 2384-2395),
    any other function just gets it as its first parameter and starts from the top.
    - EX_SkipOffsetConst is an offset into the script it sits in (the backend patches it from that script's
      StatementLabelMap, KismetCompilerVMBackend.cpp 1116-1121, 2135-2154): it sits in an ubergraph, is a statement
      there after the prologue, and the FLatentActionInfo carrying it names that same ubergraph as ExecutionFunction.
      Anywhere else the offset means nothing in the script that runs it.
    - An FLatentActionInfo naming its own function with a literal EX_IntConst Linkage is held to the same: the
      function is an ubergraph and the Linkage an entry of it.
    One naming another function with an EX_IntConst only passes it that number, which the engine allows (the editor
    never does: it emits a latent call's info only in the ubergraph, fixed up to the resume statement, 1297;
    CallFunctionHandler.cpp 96-100); it is counted, not flagged. A Linkage of INDEX_NONE never resumes."""
    for i, st in functions(pkg):
        name = pkg.exports[i]['name']
        for n in (x for t in pkg.script(i) for x in t.walk()):
            if n.op == 0x5B:
                stat('latent_linkage_offsets', 'SkipOffsetConsts')
                if not is_ubergraph(pkg, i): yield i, 'EX_SkipOffsetConst at mem %d outside an ubergraph' % n.mem
                elif n.ops[0][1] not in entries(pkg, i):
                    yield i, 'EX_SkipOffsetConst at mem %d is %d, not a statement after the prologue' % (n.mem, n.ops[0][1])
            if n.op != 0x2F or pkg.path(n.ops[0][1]) != '/Script/Engine.LatentActionInfo': continue
            kids = [k for k in n.kids if k.op != 0x30]
            if len(kids) != 4 or kids[0].op not in (0x1D, 0x5B) or kids[2].op != 0x21: continue
            link, fn = kids[0].ops[0][1], kids[2].ops[0][1]
            if link == -1: continue
            stat('latent_linkage_offsets', 'LatentActionInfos')
            if fn.lower() != name.lower():
                if kids[0].op == 0x5B:
                    yield i, 'LatentActionInfo at mem %d resumes %s with an offset into %s' % (n.mem, fn, name)
                else: stat('latent_linkage_offsets', 'IntConst linkage into another function')
            elif not is_ubergraph(pkg, i):
                yield i, 'LatentActionInfo at mem %d resumes %s, which is no ubergraph: %d is only its first parameter' % (
                    n.mem, fn, link)
            elif link not in entries(pkg, i):
                yield i, 'LatentActionInfo at mem %d resumes at %d, not a statement after the prologue' % (n.mem, link)


@rule
def out_param_access(pkg):
    """An out parameter (CPF_OutParm, the return value included) is read and written only through
    EX_LocalOutVariable, which names a CPF_Parm | CPF_OutParm property; a function whose script uses one is
    FUNC_HasOutParms, as is every function with an out parameter. A script caller gives an out parameter only an
    FOutParmRec, never a frame slot (ScriptCore.cpp 865-890), so EX_LocalVariable on one reads zero and writes
    nowhere the caller sees (2050-2072); execLocalOutVariable walks Stack.OutParms with only checkSlow (2170-2192), and
    ProcessEvent builds that list only under FUNC_HasOutParms (1968-2003). The editor emits EX_LocalOutVariable
    exactly for CPF_OutParm (KismetCompilerVMBackend.cpp 1058-1061)."""
    for i, st in functions(pkg):
        own = {p.name: p for p in st.props}
        uses = False
        for n in (x for t in pkg.script(i) for x in t.walk()):
            if n.op not in (0x00, 0x48): continue
            p = own.get(n.ops[0][1][-1])
            if p is None: continue                                  # local_operands' finding
            stat('out_param_access', 'locals')
            if n.op == 0x00 and p.flags & CPF_OutParm:
                yield i, 'EX_LocalVariable at mem %d names out parameter %s' % (n.mem, p.name)
            if n.op == 0x48:
                uses = True
                if p.flags & (CPF_Parm | CPF_OutParm) != CPF_Parm | CPF_OutParm:
                    yield i, 'EX_LocalOutVariable at mem %d names %s, flags %#x: not a Parm | OutParm' % (n.mem, p.name, p.flags)
        has_out = any(p.flags & CPF_Parm and p.flags & CPF_OutParm for p in st.props)
        if (uses or has_out) and not st.function_flags & 0x400000:
            yield i, 'FunctionFlags %#x lack FUNC_HasOutParms, and it %s' % (
                st.function_flags, 'uses EX_LocalOutVariable' if uses else 'has an out parameter')


OPERAND_KINDS = {0x1C: ('Function',), 0x46: ('Function',), 0x68: ('Function',),
                 0x63: Package.FUNCTION_CLASSES, 0x2F: ('ScriptStruct', 'UserDefinedStruct'),
                 0x2E: CLASS_KINDS, 0x13: CLASS_KINDS, 0x52: CLASS_KINDS, 0x54: CLASS_KINDS, 0x55: CLASS_KINDS}


@rule
def object_operand_kinds(pkg):
    """An object operand is the kind its opcode casts it to: a UFunction for EX_FinalFunction, EX_LocalFinalFunction
    and EX_CallMath, used unchecked (ScriptCore.cpp 3005-3024, 937-958), a function for EX_CallMulticastDelegate
    (CastChecked, 3032), a UScriptStruct for EX_StructConst (CastChecked, 3376-3378), a UClass for the casts
    (execDynamicCast 3605, execMetaCast 3652, the interface casts 3671-3767). Calls and struct literals never name null;
    a cast tolerates a null class (3616). (A string-table text's object is read and dropped, 3246: any kind will do.)"""
    for i, st in functions(pkg):
        for n in (x for t in pkg.script(i) for x in t.walk()):
            want = OPERAND_KINDS.get(n.op)
            if not want: continue
            idx = n.ops[0][1]
            stat('object_operand_kinds', 'operands')
            if idx == 0:
                if n.op not in (0x2E, 0x13, 0x52, 0x54, 0x55): yield i, 'op %02x at mem %d names null' % (n.op, n.mem)
                continue
            k = pkg.class_of(idx)
            if k not in want:
                yield i, 'op %02x at mem %d names %s, a %s' % (n.op, n.mem, pkg.path(idx), k)


@rule
def field_paths_resolve(pkg):
    """Every property operand's FFieldPath resolves at load: the owner is a struct (a class, function or script
    struct) and its last name is a property of the owner or one of its supers (FieldPath.cpp 256-281, FindFProperty
    walks super structs), a 2-name path naming that property's inner field; at most 2 names (check(PathIndex <= 1),
    269). An empty path with an owner resolves to nothing. An unresolved path leaves the operand null
    (UStruct::PostLoad logs "Failed to resolve bytecode referenced field", Class.cpp 1975-1998), and
    EX_LocalVariable / EX_InstanceVariable then throw, the others dereference it (ScriptCore.cpp 2050-2098,
    Stack.h 246-262). Only EX_Let's property, a context's r-value and EX_PropertyConst may be empty (no owner)."""
    for i, st in functions(pkg):
        for n in (x for t in pkg.script(i) for x in t.walk()):
            for op in n.ops:
                if op[0] != 'prop': continue
                _, segs, owner = op
                stat('field_paths_resolve', 'paths')
                if not segs:
                    if owner: yield i, 'op %02x at mem %d: an empty path owned by %s' % (n.op, n.mem, pkg.path(owner))
                    elif n.op not in (0x0F, 0x19, 0x1A, 0x12, 0x33): yield i, 'op %02x at mem %d names no property' % (n.op, n.mem)
                    continue
                if len(segs) > 2: yield i, 'op %02x at mem %d: path %s has %d names' % (n.op, n.mem, segs, len(segs)); continue
                if not owner: yield i, 'op %02x at mem %d: %s has no owner' % (n.op, n.mem, segs); continue
                r = ref(pkg, owner)
                k = kind_of(r)
                if k is None: stat('field_paths_resolve', 'unknown owner'); continue
                if k not in STRUCT_KINDS: yield i, 'op %02x at mem %d: %s owned by %s, a %s' % (n.op, n.mem, segs, r, k); continue
                got = find_prop(r, segs[-1])
                if got is UNKNOWN: stat('field_paths_resolve', 'unknown super'); continue
                if got is None:
                    yield i, 'op %02x at mem %d: %s is no property of %s' % (n.op, n.mem, segs[-1], r); continue
                p = got[1].prop
                if len(segs) == 2 and p is not None and segs[0] not in {q.name for q in p.subs}:
                    yield i, 'op %02x at mem %d: %s has no inner field %s' % (n.op, n.mem, segs[-1], segs[0])


def _implements(cls_r, iface):
    """Whether class cls_r (a chain) lists iface or an interface extending it; None if that cannot be read."""
    unknown = False
    for c in super_chain(cls_r):
        if c is UNKNOWN: return None
        for x in interfaces_of(c):
            ch = super_chain(x)
            if iface in ch: return True
            unknown = unknown or UNKNOWN in ch
        unknown = unknown or bool(c.path)                  # a native class's own interfaces are not in the dump
    return None if unknown else False


@rule
def instance_var_owner(pkg):
    """EX_InstanceVariable / EX_DefaultVariable name a property of a class the object they run on IsA: the frame's
    object, or for the inner expression of EX_Context the context object (only that one expression runs on it,
    ScriptCore.cpp 2884-2893; a call's arguments run on the frame's object, 868/899). execInstanceVariable throws
    "Attempted to access missing property" and leaves the address null when !IsA(owner) (2075-2098), so a read gives
    nothing and a write is dropped. IsA walks SuperStruct only, never interfaces (UObjectBaseUtility.h 523,
    Class.h 481): the owner is never a function, a struct or an interface.
    The engine checks the object at run time; this checks the static type the package gives it (the variable, the
    call's return value, the cast), as the editor type-checks every member access and downcasts only through a Cast
    node, which the game keeps. So an unchecked C++ downcast (static_cast) reads as a finding here although it runs
    correctly on an object that really is of the class; a miscompiled owner or object is what it is kept for."""
    for i, st in functions(pkg):
        for ctx, n, this in walk_on(pkg, i):
            if n.op not in (0x01, 0x02): continue
            r = ref(pkg, n.ops[0][2])
            stat('instance_var_owner', 'members')
            k = kind_of(r)
            if k is None: stat('instance_var_owner', 'unknown owner'); continue
            if k not in CLASS_KINDS:
                yield i, 'op %02x at mem %d: %s owned by %s, a %s' % (n.op, n.mem, n.ops[0][1][-1], r, k); continue
            if is_interface(r):
                yield i, 'op %02x at mem %d: %s owned by interface %s' % (n.op, n.mem, n.ops[0][1][-1], r); continue
            if this is None or this[0] != 'obj': stat('instance_var_owner', 'unknown object'); continue
            ch = super_chain(this[1])
            if r in ch: continue
            if UNKNOWN in ch: stat('instance_var_owner', 'unknown chain'); continue
            yield i, 'op %02x at mem %d: %s of %s, run on a %s' % (n.op, n.mem, n.ops[0][1][-1], r, this[1])


@rule
def call_target_owner(pkg):
    """A function called by pointer runs on an object of its class: EX_FinalFunction / EX_LocalFinalFunction hand the
    UFunction to CallFunction / ProcessLocalFunction on the object the call runs on (ScriptCore.cpp 3005-3024) with
    no IsA check; a native thunk casts it to its class unchecked (ScriptMacros.h 100-102) and a script body reads
    its members off it (execInstanceVariable, 2075-2098). An interface's function runs on an object implementing it
    (UFunction::Invoke, Class.cpp 5668-5680). EX_CallMath runs on the CDO of the function's class (937-958), which
    is never an interface. A static function's thunk never touches the object, so for a static callee this is the
    editor's habit rather than a crash: it calls one of another class under EX_Context(EX_ObjectConst Default__Owner)
    (K2Node_CallFunction.cpp 1031-1043), and the game keeps that. It still decides where an authority-only or cosmetic
    static runs: CallFunction asks that object's GetFunctionCallspace (ScriptCore.cpp 975-978), which a library's CDO
    and an actor answer from the world (BlueprintFunctionLibrary.cpp 20, Actor.cpp 4191-4197) and a plain UObject
    answers Local (Object.h 1221-1224). As in instance_var_owner, the object's class is its static type."""
    for i, st in functions(pkg):
        for ctx, n, this in walk_on(pkg, i):
            if n.op not in (0x1C, 0x46, 0x68): continue
            f = finfo(ref(pkg, n.ops[0][1]))
            stat('call_target_owner', 'calls')
            if f is None or f.outer is None or kind_of(f.outer) is None: stat('call_target_owner', 'unknown callee'); continue
            if n.op == 0x68:
                if is_interface(f.outer): yield i, 'CallMath at mem %d calls %s of interface %s' % (n.mem, f.r.name, f.outer)
                continue
            if this is None: stat('call_target_owner', 'unknown object'); continue
            ch = super_chain(this[1])
            if f.outer in ch: continue
            if this[0] == 'obj' and is_interface(f.outer):
                got = _implements(this[1], f.outer)
                if got: continue
                if got is None: stat('call_target_owner', 'unknown interfaces'); continue
            if UNKNOWN in ch: stat('call_target_owner', 'unknown chain'); continue
            yield i, 'op %02x at mem %d calls %s of %s on a %s%s' % (n.op, n.mem, f.r.name, f.outer, this[1],
                                                                  ' (interface value)' if this[0] == 'iface' else '')


NET_CALLSPACE = 0x012051CC     # FUNC_NetFuncFlags | BlueprintAuthorityOnly | BlueprintCosmetic | NetRequest | NetResponse


@rule
def call_opcode_flags(pkg):
    """The call opcode fits the callee's flags. EX_LocalFinalFunction / EX_LocalVirtualFunction go through
    ProcessLocalFunction, which never asks GetFunctionCallspace (ScriptCore.cpp 1140-1156; CallFunction does,
    961-1034): an RPC, authority-only or cosmetic callee would run locally, so the editor calls those with the
    non-local opcodes (KismetCompilerVMBackend.cpp 1223-1234). EX_CallMath runs a Static | Final | Native function's
    thunk on its class's CDO, skipping the callspace too - its own checkSlow says so (937-958; backend 1223-1226,
    1255-1258). A call by pointer to a function a Blueprint can override - a script function without FUNC_Final, or a
    native BlueprintEvent (EdGraphSchema_K2.cpp 935-945) - runs exactly that function and skips any subclass's
    override: no crash, but a virtual call made static. The editor makes one only for FUNC_Final or a parent call
    (backend 1222, 1253-1282; CallFunctionHandler.cpp 496-511), the calling class then having its own function of that
    name, and the game keeps that. (A native non-event function is never overridden by script, and its thunk
    dispatches C++ overrides itself.) A delegate signature called as a function runs nothing - it has no script
    (ScriptCore.cpp 918) - and the editor never emits one (backend 1248-1252)."""
    for i, st in functions(pkg):
        cls = ref(pkg, pkg.exports[i]['outer'])
        mine = own_funcs(cls) if kind_of(cls) in CLASS_KINDS else None
        for ctx, n, this in walk_on(pkg, i):
            if n.op not in (0x1B, 0x1C, 0x45, 0x46, 0x68): continue
            f = finfo(operand_call_target(ctx, n, this))
            stat('call_opcode_flags', 'calls')
            if f is None or f.flags is None: stat('call_opcode_flags', 'unknown callee'); continue
            fl, what = f.flags, 'op %02x at mem %d calls %s (flags %#x)' % (n.op, n.mem, f.r.name, f.flags)
            if f.kind != 'Function' or fl & 0x100000: yield i, what + ': a delegate signature'; continue
            if n.op in (0x45, 0x46) and fl & NET_CALLSPACE: yield i, what + ': a local call skips its callspace'
            if n.op == 0x68 and (fl & 0x2401 != 0x2401 or fl & NET_CALLSPACE):
                yield i, what + ': EX_CallMath needs Static | Final | Native and no callspace flag'
            if n.op in (0x1C, 0x46, 0x68) and not fl & 0x1 and (not f.native or fl & 0x8000000):
                parent_call = this == ctx.self and mine is not None and mine.get(f.r.name.lower(), f.r) != f.r
                if not parent_call: yield i, what + ': a call by pointer to a function without FUNC_Final'


@rule
def call_names_resolve(pkg):
    """Every function a script names is found, by UClass::FindFunctionByName (own FuncMap, listed interfaces, then
    the super: Class.cpp 5281-5323), on the class of the object that looks it up: EX_VirtualFunction /
    EX_LocalVirtualFunction on the object the call runs on - a miss is UE_LOG Fatal "Failed to find function"
    (FindFunctionChecked, ScriptCore.cpp 1329-1337; 2998-3017); EX_InstanceDelegate on that object (3295-3300);
    EX_BindDelegate on its object operand (3302-3321); a FLatentActionInfo's ExecutionFunction on its CallbackTarget,
    else the action only logs a warning and never resumes (LatentActionManager.cpp 212-220). FuncMap is serialized,
    never rebuilt on load (Class.cpp 4411-4413). The engine looks the name up on the run-time object; this walks the
    static type the package gives it, as instance_var_owner does (a name only a subclass has, reached by an unchecked
    downcast, reads as a finding)."""
    for i, st in functions(pkg):
        for ctx, n, this in walk_on(pkg, i):
            if n.op in (0x1B, 0x45, 0x4B): name, on = n.ops[0][1], this
            elif n.op == 0x61: name, on = n.ops[0][1], etype(ctx, n.kids[1], ctx.self)
            elif n.op == 0x2F and n.ops[0][1] and pkg.path(n.ops[0][1]) == '/Script/Engine.LatentActionInfo' \
                    and len(n.kids) >= 4 and n.kids[2].op == 0x21 and n.kids[0].op in (0x1D, 0x5B) and n.kids[0].ops[0][1] != -1:
                name, on = n.kids[2].ops[0][1], etype(ctx, n.kids[3], ctx.self)
            else: continue
            if name == 'None' and n.op in (0x4B, 0x61): continue
            stat('call_names_resolve', 'names')
            if on is None or on[0] not in ('obj', 'iface') or on[1] in (None, UNKNOWN): stat('call_names_resolve', 'unknown object'); continue
            got = find_func(on[1], name)
            if got is UNKNOWN: stat('call_names_resolve', 'unknown chain'); continue
            if got is None:
                yield i, '%s at mem %d names %s, which %s does not have' % (
                    {0x1B: 'EX_VirtualFunction', 0x45: 'EX_LocalVirtualFunction', 0x4B: 'EX_InstanceDelegate',
                     0x61: 'EX_BindDelegate', 0x2F: 'LatentActionInfo'}[n.op], n.mem, name, on[1])


@rule
def interface_context_placement(pkg):
    """A context whose object is an interface value unwraps it with EX_InterfaceContext: ProcessContextOpcode steps the
    object expression into an 8-byte UObject* (ScriptCore.cpp 2881-2884), an FInterfaceProperty would copy its
    16-byte FScriptInterface over it, and only execInterfaceContext steps one into an FScriptInterface and hands back
    its object (2195-2206). Wrapping an object reference is harmless - the copy is the reference's own 8 bytes - and
    the game's own Blueprints do it (BP_GooExplosiontComponent).
    The other half, that EX_InterfaceContext appears only as the object of EX_Context / EX_Context_FailSilent, is the
    editor's placement (KismetCompilerVMBackend.cpp 353-356, 1244; K2Node_CallFunction.cpp 1010), which the game
    keeps: elsewhere it still hands back an object, 8 bytes, but where an interface value is expected (an interface
    parameter or variable) the interface pointer after it stays null, which a native call through the interface
    dereferences."""
    for i, st in functions(pkg):
        placed = set()
        for ctx, n, this in walk_on(pkg, i):
            if n.op in (0x19, 0x1A):
                stat('interface_context_placement', 'contexts')
                obj = n.kids[0]
                if obj.op == 0x51:
                    placed.add(id(obj))
                else:
                    t = etype(ctx, obj, this)
                    if t is not None and t[0] == 'iface':
                        yield i, 'context at mem %d runs on an interface value without EX_InterfaceContext' % n.mem
            elif n.op == 0x51 and id(n) not in placed:
                yield i, 'EX_InterfaceContext at mem %d is not the object of a context' % n.mem


@rule
def call_args_fit_callee(pkg):
    """A call passes exactly one argument per parameter of its callee, in order, the return value left out - for an
    imported callee and a broadcast's signature too. The editor emits one per CPF_Parm (KismetCompilerVMBackend.cpp
    1284-1326), and so do all the game's Blueprints. (call_arity's WND_SeasonLevels example is not one: its SetData
    calls run under EX_Context on an ITM_Season_Level and pass both parameters of SeasonLevelWidget:SetData; the three
    are those of WND_SeasonLevels' own SetData, which invariants.callee() finds by name on the calling class.) The VM
    steps arguments into the callee's ChildProperties
    until EX_EndFunctionParms (ScriptCore.cpp 852-902; a broadcast into its signature's, 3042-3063): one more is read
    past the last property, and a native thunk reads a fixed list, so extra bytes desync it (ScriptMacros.h 98). Fewer
    is legal to the VM - execEndFunctionParms backs up "for skipping over optional function parms" (2366-2370) - so
    for a missing plain parameter the exact count is the editor's rule, which the game keeps, not a crash: the
    parameter is then its type's zero (a script callee's memzeroed frame, a native's P_GET default), not the
    UFUNCTION's or the C++ default the source gave, a silently wrong argument. A missing out parameter of a script
    callee gets no FOutParmRec, so its EX_LocalOutVariable walks off Stack.OutParms (2170-2192)."""
    for i, st in functions(pkg):
        for ctx, n, this in walk_on(pkg, i):
            if n.op not in (0x1B, 0x1C, 0x45, 0x46, 0x68, 0x63): continue
            f = finfo(operand_call_target(ctx, n, this))
            stat('call_args_fit_callee', 'calls')
            if f is None: stat('call_args_fit_callee', 'unknown callee'); continue
            got, want = len(args_of(n)), f.args
            if got > len(want):
                yield i, 'op %02x at mem %d passes %d arguments to %s, which has %d parameters' % (n.op, n.mem, got, f.r, len(want))
            elif got < len(want):
                lost = [p.name for p in want[got:]]
                outs = [p.name for p in want[got:] if p.flags is not None and p.flags & CPF_OutParm]
                yield i, 'op %02x at mem %d passes %d arguments to %s, none for %s%s' % (
                    n.op, n.mem, got, f.r, lost, ' (out: %s)' % outs if outs and not f.native else '')


OUT_ADDRESSABLE = (0x00, 0x01, 0x02, 0x48, 0x6C)


def out_addressable(a):
    """Whether expression a leaves Stack.MostRecentPropertyAddress: a variable, or a context / struct member /
    array element whose chain ends in one, or an EX_SwitchValue all of whose values do - execSwitchValue steps the
    chosen value with the caller's result (ScriptCore.cpp 2519-2585: 2555, 2582), and the editor's Select node passes one straight
    to a reference parameter (the game's BPL_UpgradeHelpers, Basic_Dots)."""
    if a.op in OUT_ADDRESSABLE: return True
    if a.op in (0x19, 0x1A, 0x12): return out_addressable(a.kids[1])
    if a.op in (0x42, 0x6B): return out_addressable(a.kids[0])
    if a.op == 0x69: return all(out_addressable(v) for v in a.kids[2:-1:2] + a.kids[-1:])
    return False


@rule
def out_args_addressable(pkg):
    """An argument for an out or reference parameter is addressable. A script callee, and a multicast broadcast,
    step it with a null result and use only the address it leaves (ScriptCore.cpp 865-890, 3042-3054): a literal or
    a call writes through the null result (execIntConst and the rest store to RESULT_PARAM unchecked), anything else
    leaves the callee its own frame slot, so the caller never sees the output. A native's P_GET_*_REF falls back to a
    temporary (Stack.h 344-361): a const reference loses nothing, a non-const one loses the output. The editor refuses
    a read-only term for an output or a non-const reference and lets one reach only a const reference
    (CallFunctionHandler.cpp 259-273), where only a native callee's temporary can take it."""
    for i, st in functions(pkg):
        for ctx, n, this in walk_on(pkg, i):
            if n.op not in (0x1B, 0x1C, 0x45, 0x46, 0x68, 0x63): continue
            f = finfo(operand_call_target(ctx, n, this))
            if f is None: stat('out_args_addressable', 'unknown callee'); continue
            for p, a in zip(f.args, args_of(n)):
                if p.flags is None: stat('out_args_addressable', 'unknown flags'); continue
                if not p.flags & CPF_OutParm: continue
                stat('out_args_addressable', 'out args')
                if out_addressable(a): continue
                if n.op == 0x63 or not f.native:
                    yield i, 'op %02x at mem %d passes op %02x for out parameter %s of script %s' % (n.op, n.mem, a.op, p.name, f.r)
                elif not p.flags & CPF_BITS['ConstParm']:
                    yield i, 'op %02x at mem %d passes op %02x for out parameter %s of native %s: the output is lost' % (
                        n.op, n.mem, a.op, p.name, f.r)
