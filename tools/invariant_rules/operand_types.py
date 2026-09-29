import sys
from invariants import *
"""TYPING: what the VM needs of the types that meet in the bytecode - a literal and the property it is written into,
an assignment opcode and its destination, a struct member and the struct it offsets into, a cast and its operand, a
call argument and its parameter, a return value and the function's return property, the operand grammar of the ops
that carry sizes and counts, and which opcodes exist at all.

Types come from three places, the way the engine links them: a property of this package (its FProperty on disk), a
/Game package's (Package.resolve, the game's own content with --game), and a /Script one's - the Dumper-7 SDK dump of
the game (<Pkg>_classes.hpp / _structs.hpp / _parameters.hpp for property C++ types, sizes and flags,
<Pkg>_functions.cpp for function flags). A position whose type cannot be found is not checked."""
import invariants
import collections, glob, os, pickle, re, struct, tempfile

# rule -> positions whose types the rule knew and compared: calibration coverage (a rule that types nothing passes
# the game trivially), printed by the calibration driver.
CHECKED = collections.Counter()


def judged(rule_name, verdict):
    """Counts a comparison a rule could make (verdict not None) and passes the verdict on."""
    if verdict is not None: CHECKED[rule_name] += 1
    return verdict


CPF_ConstParm, CPF_Transient, CPF_ReferenceParm, CPF_EditorOnly = 0x2, 0x2000, 0x8000000, 0x800000000
FUNC_Final, FUNC_Native, FUNC_Static = 0x1, 0x400, 0x2000
FUNC_Forbidden_CallMath = 0x0120514C      # Net|NetRequest|NetResponse|NetMulticast|NetServer|NetClient|AuthorityOnly|Cosmetic
CLASS_Interface = 0x4000


# ---- the SDK: every /Script class, struct and function with its properties

SDK_DIR = os.path.join(SDK_DUMP, 'SDK', 'SDK') if SDK_DUMP else None


class Ty:
    """A property's type as the VM moves it: its FProperty class, its element size, and what it refers to - a struct's
    path, a container's inner Ty (a map's (key, value)), an object / class / interface property's class path. `bitfield`
    for a bool that shares its byte (FieldMask != 0xFF)."""
    __slots__ = ('cls', 'size', 'sub', 'bitfield')

    def __init__(s, cls, size=0, sub=None, bitfield=False): s.cls, s.size, s.sub, s.bitfield = cls, size, sub, bitfield

    def __repr__(s):
        t = s.cls.replace('Property', '')
        if s.cls == 'StructProperty': t += '<%s>' % (s.sub or '?').split('.')[-1]
        elif s.cls in ('ArrayProperty', 'SetProperty'): t += '<%r>' % (s.sub,)
        elif s.cls == 'MapProperty' and s.sub: t += '<%r, %r>' % s.sub
        return t + (':1' if s.bitfield else '')


KIND = {'IntProperty': 'int32', 'ByteProperty': 'uint8', 'FloatProperty': 'float', 'Int64Property': 'int64',
        'UInt64Property': 'uint64', 'BoolProperty': 'bool', 'NameProperty': 'name', 'StrProperty': 'str',
        'TextProperty': 'text', 'StructProperty': 'struct', 'ObjectProperty': 'object', 'ClassProperty': 'object',
        'WeakObjectProperty': 'weak', 'LazyObjectProperty': 'lazy', 'SoftObjectProperty': 'soft',
        'SoftClassProperty': 'soft', 'InterfaceProperty': 'interface', 'DelegateProperty': 'delegate',
        'MulticastInlineDelegateProperty': 'mcdelegate', 'MulticastDelegateProperty': 'mcdelegate',
        'MulticastSparseDelegateProperty': 'sparse', 'ArrayProperty': 'array', 'SetProperty': 'set',
        'MapProperty': 'map', 'FieldPathProperty': 'fieldpath', 'DoubleProperty': 'double', 'Int8Property': 'int8',
        'Int16Property': 'int16', 'UInt16Property': 'uint16', 'UInt32Property': 'uint32'}


def kind(t):
    """The VM-level kind of a Ty: two properties of one kind hold the same bytes (an Object and a Class property both a
    UObject*; a ByteProperty and a uint8 EnumProperty one byte). None if unknown."""
    if t is None: return None
    if t.cls == 'EnumProperty': return {1: 'uint8', 4: 'enum32', 8: 'enum64'}.get(t.size)
    return KIND.get(t.cls)


# What the VM moves for a value of each kind. Integers of one width are the same bytes to it: a value of one written
# where the other is read keeps every bit, so an enum and its underlying integer, int64 and uint64, a bool and a byte
# are interchangeable. A weak or lazy object property is read into the VM as a UObject* (FObjectPropertyBase::
# CopyCompleteValueToScriptVM, PropertyBaseObject.cpp 607-613, which execLocalVariable / execInstanceVariable call,
# ScriptCore.cpp 2069-2190), so as a value it is an object.
STORAGE = {'int32': 'i4', 'enum32': 'i4', 'uint32': 'i4', 'int64': 'i8', 'uint64': 'i8', 'enum64': 'i8',
           'uint8': 'i1', 'int8': 'i1', 'bool': 'i1', 'int16': 'i2', 'uint16': 'i2', 'weak': 'object', 'lazy': 'object'}


def storage(t):
    k = kind(t)
    return STORAGE.get(k, k)


def same(a, b):
    """Whether a value of Ty a fits where Ty b is read: one kind (integers of one width are one kind, STORAGE), and
    for a container the same inner type. A struct is b's struct or, of the same size, one derived from it (the VM
    copies the value's whole struct, so a derived struct that adds members would overrun; FVector_NetQuantize adds
    none to FVector). None when either is not known well enough to say."""
    ka, kb = kind(a), kind(b)
    if ka is None or kb is None: return None
    ka, kb = STORAGE.get(ka, ka), STORAGE.get(kb, kb)
    if ka != kb: return False
    if ka == 'struct':
        if not a.sub or not b.sub or a.sub.startswith('?') or b.sub.startswith('?'): return None
        if a.sub == b.sub: return True
        if a.sub.startswith('/script/') and sdk():
            if a.sub not in sdk().objs or b.sub not in sdk().objs: return None
            c, seen = a.sub, 0
            while c and seen < 32:
                if c == b.sub: return sdk().objs[a.sub].get('size') == sdk().objs[b.sub].get('size')
                c, seen = (sdk().objs.get(c) or {}).get('super'), seen + 1
        return False
    if ka in ('array', 'set'): return same(a.sub, b.sub)
    if ka == 'map':
        if not a.sub or not b.sub: return None
        k, v = same(a.sub[0], b.sub[0]), same(a.sub[1], b.sub[1])
        return None if None in (k, v) else k and v
    return True


SCALAR = {'bool': ('BoolProperty', 1), 'int8': ('Int8Property', 1), 'uint8': ('ByteProperty', 1),
          'int16': ('Int16Property', 2), 'uint16': ('UInt16Property', 2), 'int32': ('IntProperty', 4),
          'uint32': ('UInt32Property', 4), 'int64': ('Int64Property', 8), 'uint64': ('UInt64Property', 8),
          'float': ('FloatProperty', 4), 'double': ('DoubleProperty', 8), 'class FName': ('NameProperty', 8),
          'class FString': ('StrProperty', 16), 'class FText': ('TextProperty', 24)}
TEMPLATES = {'TSubclassOf': ('ClassProperty', 8), 'TWeakObjectPtr': ('WeakObjectProperty', 8),
             'TLazyObjectPtr': ('LazyObjectProperty', 28), 'TSoftObjectPtr': ('SoftObjectProperty', 40),
             'TSoftClassPtr': ('SoftClassProperty', 40), 'TScriptInterface': ('InterfaceProperty', 16),
             'TArray': ('ArrayProperty', 16), 'TSet': ('SetProperty', 80), 'TMap': ('MapProperty', 80),
             'TDelegate': ('DelegateProperty', 16), 'TMulticastInlineDelegate': ('MulticastInlineDelegateProperty', 16),
             'TMulticastSparseDelegate': ('MulticastSparseDelegateProperty', 1), 'TFieldPath': ('FieldPathProperty', 32)}


def _split_top(t):
    out, depth, cur = [], 0, ''
    for c in t:
        if c == '<': depth += 1
        elif c == '>': depth -= 1
        if c == ',' and depth == 0: out.append(cur.strip()); cur = ''
        else: cur += c
    return out + [cur.strip()]


class SdkIndex:
    """classes / structs / functions keyed by lower-case path (/script/engine.actor, /script/coreuobject.vector,
    /script/engine.kismetmathlibrary:add_intint): each {kind, super, props [(name, cpp, size, dim, bit, flags)], fflags,
    funcs {name: key}, iface}."""

    def __init__(s, d):
        s.objs, s.cpp, s.enums = {}, {}, {}
        s.offsets = {}                  # (key, member name) -> the member's offset (typing_view_puns' bounds)
        sec = re.compile(r'^// (Class|ScriptStruct|Function) (/Script/\w+)\.(\w+)(?:\.(\w+))?\s*$')
        head = re.compile(r'^(?:class|struct)\s+(?:alignas\(\w+\)\s+)?(\w+)(?:\s+final)?(?:\s*:\s*public\s+(\w+))?\s*$')
        field = re.compile(r'^\t(.+?)\s+(\w+)(?:\[0x([0-9A-Fa-f]+)\])?(\s*:\s*\d+)?;\s*// 0x([0-9A-Fa-f]+)\(0x([0-9A-Fa-f]+)\)\((.*)\)\s*$')
        enum = re.compile(r'^enum class (\w+) : (\w+)')
        stems, bases = set(), {}
        for f in sorted(glob.glob(os.path.join(d, '*.hpp'))):
            if not f.endswith(('_classes.hpp', '_structs.hpp', '_parameters.hpp')): continue
            cur, want_head, want_size = None, False, False
            for line in open(f, encoding='utf-8', errors='replace'):
                m = sec.match(line)
                if m:
                    k, pkg, a, b = m.groups()
                    key = (pkg + '.' + a + (':' + b if b else '')).lower()
                    cur = s.objs.setdefault(key, dict(kind={'Class': 'class', 'ScriptStruct': 'struct', 'Function': 'function'}[k],
                                                       super=None, props=[], fflags=None, funcs={}, iface=False, name=b or a))
                    want_head = k != 'Function'
                    want_size = True
                    if k == 'Function':
                        s.objs.setdefault((pkg + '.' + a).lower(), dict(kind='class', super=None, props=[], fflags=None,
                                                                         funcs={}, iface=False, name=a))['funcs'][b.lower()] = key
                    stems.add(os.path.basename(f).rsplit('_', 1)[0])
                    continue
                if cur is None: continue
                if want_size:
                    m = re.match(r'^// 0x[0-9A-Fa-f]+ \(0x([0-9A-Fa-f]+) - 0x[0-9A-Fa-f]+\)', line)
                    if m: cur['size'] = int(m.group(1), 16)
                    want_size = False
                    if m: continue
                if want_head:
                    m = head.match(line)
                    if m:
                        s.cpp[m.group(1)] = key
                        if m.group(2): bases[key] = m.group(2)
                        cur['iface'] = cur['kind'] == 'class' and m.group(1)[:1] == 'I' and m.group(1)[1:].lower() == cur['name'].lower()
                        want_head = False
                    continue
                m = field.match(line)
                if m:
                    cpp, name, dim, bit, off, size, flags = m.groups()
                    if name.startswith(('Pad_', 'BitPad_')) and 'Dumper-7' in flags: continue
                    cur['props'].append((name, cpp, int(size, 16), int(dim, 16) if dim else 1, bool(bit), flags))
                    s.offsets[key, name.lower()] = int(off, 16)
                    continue
                m = enum.match(line)
                if m: s.enums[m.group(1)] = SCALAR.get(m.group(2), ('?', 1))[1]
        for key, base in bases.items():
            s.objs[key]['super'] = s.cpp.get(base)
        flag_bits = dict(Final=0x1, RequiredAPI=0x2, BlueprintAuthorityOnly=0x4, BlueprintCosmetic=0x8, Net=0x40,
                         NetReliable=0x80, NetRequest=0x100, Exec=0x200, Native=0x400, Event=0x800, NetResponse=0x1000,
                         Static=0x2000, NetMulticast=0x4000, UbergraphFunction=0x8000, MulticastDelegate=0x10000,
                         Public=0x20000, Private=0x40000, Protected=0x80000, Delegate=0x100000, NetServer=0x200000,
                         HasOutParams=0x400000, HasDefaults=0x800000, NetClient=0x1000000, DLLImport=0x2000000,
                         BlueprintCallable=0x4000000, BlueprintEvent=0x8000000, BlueprintPure=0x10000000,
                         EditorOnly=0x20000000, Const=0x40000000, NetValidate=0x80000000)
        fn = re.compile(r'^// Function (\w+)\.(\w+)\.(\w+)\s*$')
        for stem in sorted(stems):
            f = os.path.join(d, stem + '_functions.cpp')
            if not os.path.exists(f): continue
            pending = None
            for line in open(f, encoding='utf-8', errors='replace'):
                if pending:
                    m = re.match(r'^// \((.*)\)\s*$', line)
                    if m:
                        bits = 0
                        for w in m.group(1).split(','): bits |= flag_bits.get(w.strip(), 0)
                        o = s.objs.setdefault(pending, dict(kind='function', super=None, props=[], fflags=None, funcs={},
                                                            iface=False, name=pending.split(':')[-1]))
                        o['fflags'] = bits
                        cls = pending.split(':')[0]
                        if cls in s.objs: s.objs[cls]['funcs'][pending.split(':')[1]] = pending
                    pending = None
                    continue
                m = fn.match(line)
                if m:
                    key = ('/script/%s.%s:%s' % m.groups()).lower()
                    if key.split(':')[0] in s.objs: pending = key

    def ty(s, cpp, size=0, bit=False):
        """A Dumper-7 C++ spelling as a Ty."""
        t = cpp.strip()
        if t.startswith('const '): t = t[6:].strip()
        t = t.rstrip('&').strip()
        if bit: return Ty('BoolProperty', 1, bitfield=True)
        if t in SCALAR:
            c, n = SCALAR[t]
            return Ty(c, n)
        m = re.match(r'^struct (\w+)$', t)
        if m: return Ty('StructProperty', size, s.cpp.get(m.group(1), '?' + m.group(1).lower()))
        if t == 'class UClass*': return Ty('ClassProperty', 8, '/script/coreuobject.object')
        m = re.match(r'^class (\w+)\s*\*$', t)
        if m: return Ty('ObjectProperty', 8, s.cpp.get(m.group(1)))
        m = re.match(r'^(\w+)<(.*)>$', t)
        if m and m.group(1) in TEMPLATES:
            c, n = TEMPLATES[m.group(1)]
            args = _split_top(m.group(2))
            if c in ('ArrayProperty', 'SetProperty'): return Ty(c, n, s.ty(args[0]))
            if c == 'MapProperty': return Ty(c, n, (s.ty(args[0]), s.ty(args[1])))
            if c in ('ClassProperty', 'WeakObjectProperty', 'LazyObjectProperty', 'SoftObjectProperty',
                     'SoftClassProperty', 'InterfaceProperty'):
                cm = re.match(r'^class (\w+)', args[0])
                return Ty(c, n, s.cpp.get(cm.group(1)) if cm else None)
            return Ty(c, n)
        if t in s.enums: return Ty('EnumProperty', s.enums[t])
        return Ty('?' + t, size)


_SDK = []


def sdk():
    """The SDK index, parsed once and cached beside the temp dir (keyed by the dump's build)."""
    if _SDK: return _SDK[0]
    if not SDK_DIR or not os.path.isdir(SDK_DIR):
        _SDK.append(None); return None
    cache = os.path.join(tempfile.gettempdir(), 'typing_sdk2_%s.pickle' % re.sub(r'\W', '_', SDK_DIR)[-60:])
    try:
        idx = pickle.load(open(cache, 'rb'))
    except Exception:
        idx = SdkIndex(SDK_DIR)
        try: pickle.dump(idx, open(cache, 'wb'))
        except Exception: pass
    _SDK.append(idx)
    return idx


CPF_WORDS = {'Parm': CPF_Parm, 'OutParm': CPF_OutParm, 'ReturnParm': CPF_ReturnParm, 'ReferenceParm': CPF_ReferenceParm,
             'ConstParm': CPF_ConstParm, 'Transient': CPF_Transient}


def _cpf(text):
    words = set(re.findall(r'\w+', text))
    return sum(v for k, v in CPF_WORDS.items() if k in words)


# ---- scopes: a struct (class, script struct, function) wherever it lives, its properties, super and functions

def prop_ty(pkg, p):
    """A package FProperty (invariants.Prop) as a Ty."""
    t = p.type
    if t == 'StructProperty': return Ty(t, p.elem_size, (pkg.path(p.ref) or '?').lower())
    if t in ('ArrayProperty', 'SetProperty'): return Ty(t, p.elem_size, prop_ty(pkg, p.subs[0]) if p.subs else None)
    if t == 'MapProperty': return Ty(t, p.elem_size, (prop_ty(pkg, p.subs[0]), prop_ty(pkg, p.subs[1])) if len(p.subs) == 2 else None)
    if t in ('ClassProperty', 'SoftClassProperty'): return Ty(t, p.elem_size, (pkg.path(p.meta) or '').lower() or None)
    if t in ('ObjectProperty', 'WeakObjectProperty', 'LazyObjectProperty', 'SoftObjectProperty', 'InterfaceProperty'):
        return Ty(t, p.elem_size, (pkg.path(p.ref) or '').lower() or None)
    if t == 'BoolProperty': return Ty(t, p.elem_size, bitfield=bool(p.bool and p.bool[3] != 0xFF))
    return Ty(t, p.elem_size)


class Scope:
    """A UStruct the rules can read: ('pkg', Package, export index) or ('sdk', key)."""
    __slots__ = ('where', 'pkg', 'i', 'key')

    def __init__(s, where, pkg=None, i=None, key=None): s.where, s.pkg, s.i, s.key = where, pkg, i, key

    def __repr__(s): return s.path()

    def path(s):
        return s.pkg.path(s.i + 1).lower() if s.where == 'pkg' else s.key

    def sdk(s): return sdk().objs.get(s.key) if s.where == 'sdk' else None

    def struct(s): return s.pkg.struct(s.i) if s.where == 'pkg' else None

    def props(s):
        """[(name, Ty, CPF flags, ArrayDim)] in declaration (ChildProperties) order."""
        k = (s.pkg.base, s.i) if s.where == 'pkg' else s.key
        if k in _PROPS: return _PROPS[k]
        if s.where == 'pkg':
            st = s.struct()
            out = [(p.name, prop_ty(s.pkg, p), p.flags, p.dim) for p in st.props] if st else []
        else:
            o = s.sdk()
            # CoreUObject's classes (Object, Field, Struct, Class, ...) hold no FProperty: their SDK members are
            # Dumper-7's own view of the C++ object (Index, Class, Name, Outer, ...), which no bytecode can name.
            out = [(n, sdk().ty(cpp, size // dim if dim else size, bit), _cpf(fl), dim)
                   for n, cpp, size, dim, bit, fl in o['props']] if o and not (
                o['kind'] == 'class' and s.key.startswith('/script/coreuobject.')) else []
        _PROPS[k] = out
        return out

    def super(s):
        if s.where == 'pkg':
            st = s.struct()
            return scope(s.pkg, st.super) if st and st.super else None
        o = s.sdk()
        return Scope('sdk', key=o['super']) if o and o['super'] in sdk().objs else None

    def chain(s):
        c, seen = s, 0
        while c is not None and seen < 64:
            yield c
            c, seen = c.super(), seen + 1

    def function(s, name):
        """This class's own UFunction of that name (a child export, or the SDK's), not its supers'."""
        if s.where == 'pkg':
            st = s.struct()
            for c in (st.children if st else []):
                if c > 0 and s.pkg.exports[c - 1]['name'].lower() == name.lower() \
                        and s.pkg.class_of(c) in Package.FUNCTION_CLASSES:
                    return Scope('pkg', s.pkg, c - 1)
            return None
        o = s.sdk()
        k = o['funcs'].get(name.lower()) if o else None
        return Scope('sdk', key=k) if k else None

    def is_interface(s):
        if s.where == 'pkg':
            st = s.struct()
            return bool(st and hasattr(st, 'class_flags') and st.class_flags & CLASS_Interface)
        o = s.sdk()
        return bool(o and o['iface'])

    def function_flags(s):
        if s.where == 'pkg':
            st = s.struct()
            return getattr(st, 'function_flags', None) if st else None
        o = s.sdk()
        return o['fflags'] if o else None

    def native(s): return s.where == 'sdk'


def scope(pkg, idx):
    """The Scope an FPackageIndex names: an export, a /Game import (Package.resolve), or a /Script import (the SDK)."""
    if not idx: return None
    if idx > 0: return Scope('pkg', pkg, idx - 1)
    path = pkg.path(idx)
    if path.startswith('/Script/'):
        idx_sdk = sdk()
        key = path.lower()
        return Scope('sdk', key=key) if idx_sdk and key in idx_sdk.objs else None
    r = pkg.resolve(idx)
    return Scope('pkg', r[0], r[1]) if r else None


_PROPS, _BYPATH = {}, {}


def scope_by_path(pkg, path):
    """The Scope of a class or struct by its lower-case full path (a Ty's sub)."""
    if not path or path.startswith('?'): return None
    if path.startswith('/script/'):
        idx_sdk = sdk()
        return Scope('sdk', key=path) if idx_sdk and path in idx_sdk.objs else None
    if (pkg.base, path) not in _BYPATH: _BYPATH[pkg.base, path] = _scope_by_game_path(pkg, path)
    return _BYPATH[pkg.base, path]


def _scope_by_game_path(pkg, path):
    for k in range(-len(pkg.imports), len(pkg.exports) + 1):
        if k and pkg.path(k).lower() == path: return scope(pkg, k)
    package = path.split('.')[0]
    here = pkg.base.replace(os.sep, '/')
    for root in [here[:here.rindex('/Content/') + len('/Content')]] + invariants.GAME_CONTENT:
        f = root + package[len('/game'):]
        if not os.path.exists(f + '.uasset'): continue
        other = invariants.load(f)
        for k in range(len(other.exports)):
            if other.path(k + 1).lower() == path: return Scope('pkg', other, k)
    return None


def member(sc, name):
    """(Ty, flags, owner Scope) of property `name` of a scope or its supers, or None. Dumper-7 respells a member that
    collides with its own UObject view as Name_0 / Class_0 / ..., so an SDK class is also asked for `name`_0."""
    for c in (sc.chain() if sc else ()):
        props = c.props()
        for want in ((name, name + '_0') if c.where == 'sdk' else (name,)):
            for n, t, fl, dim in props:
                if n.lower() == want.lower(): return t, fl, c
    return None


# ---- expression types

LIT = {0x1D: Ty('IntProperty', 4), 0x25: Ty('IntProperty', 4), 0x26: Ty('IntProperty', 4), 0x2C: Ty('IntProperty', 4),
       0x24: Ty('ByteProperty', 1), 0x1E: Ty('FloatProperty', 4), 0x35: Ty('Int64Property', 8),
       0x36: Ty('UInt64Property', 8), 0x27: Ty('BoolProperty', 1), 0x28: Ty('BoolProperty', 1),
       0x21: Ty('NameProperty', 8), 0x1F: Ty('StrProperty', 16), 0x34: Ty('StrProperty', 16),
       0x29: Ty('TextProperty', 24), 0x23: Ty('StructProperty', 12, '/script/coreuobject.vector'),
       0x22: Ty('StructProperty', 12, '/script/coreuobject.rotator'),
       0x2B: Ty('StructProperty', 48, '/script/coreuobject.transform'), 0x2A: Ty('ObjectProperty', 8),
       0x20: Ty('ObjectProperty', 8), 0x17: Ty('ObjectProperty', 8), 0x2D: Ty('InterfaceProperty', 16),
       0x67: Ty('SoftObjectProperty', 40), 0x6D: Ty('FieldPathProperty', 32), 0x4B: Ty('DelegateProperty', 16)}
LITERALS = set(LIT) | {0x2F, 0x65, 0x3D, 0x3F}
CALLS = (0x1B, 0x1C, 0x45, 0x46, 0x68)
VARIABLES = (0x00, 0x01, 0x02, 0x48, 0x6C)
CASTS = (0x2E, 0x13, 0x52, 0x54, 0x55, 0x38)
LETS = (0x0F, 0x14, 0x5F, 0x60, 0x44, 0x43)


def cast_byte(pkg, i, n):
    """The u8 operand right after an opcode (EX_PrimitiveCast's cast, EX_TextConst's literal type): Walk does not
    record u8 operands, so it is read off the export at the node's disk offset."""
    return pkg.blob(i)[n.disk + 1]


def fieldpath_ty(pkg, i, op):
    """The Ty of the property a ('prop', segs, owner) operand names: the owner struct's own or a super's property; a
    two-segment path [Inner, Container] is a container's inner property."""
    if not op or op[0] != 'prop' or not op[1]: return None
    _, segs, owner = op
    m = member(scope(pkg, owner), segs[-1])
    if not m: return None
    t = m[0]
    if len(segs) == 1: return t
    if len(segs) == 2 and t.cls in ('ArrayProperty', 'SetProperty'): return t.sub
    return None


def switch_results(n):
    """An EX_SwitchValue's result terms and its default: kids are [index, value0, result0, ..., default]."""
    return n.kids[2::2] + [n.kids[-1]]


def values(n):
    """A value term, or for an EX_SwitchValue every result and default it can step into the same slot, nested
    switches opened too (execSwitchValue steps the picked term with its own RESULT_PARAM, ScriptCore.cpp 2555, 2582)."""
    return [n] if n.op != 0x69 else [v for k in switch_results(n) for v in values(k)]


def inner_ty(pkg, op):
    """The Ty of a container's inner property an EX_ArrayConst / EX_SetConst / EX_MapConst operand names, a two-segment
    FieldPath [Inner, Container]: an array's or set's inner; a map's key or value, told apart by the inner's name
    where the map is a property of a package (the SDK keeps no inner names). None if not found."""
    if not op or op[0] != 'prop' or len(op[1]) != 2: return None
    segs, owner = op[1], op[2]
    m = member(scope(pkg, owner), segs[-1])
    if not m: return None
    t, fl, c = m
    if t.cls in ('ArrayProperty', 'SetProperty'): return t.sub
    if t.cls == 'MapProperty' and c.where == 'pkg' and isinstance(t.sub, tuple):
        p = next((p for p in (c.struct().props if c.struct() else []) if p.name.lower() == segs[-1].lower()), None)
        if p and len(p.subs) == 2:
            for k, sub in enumerate(p.subs):
                if sub.name.lower() == segs[0].lower() and p.subs[1 - k].name.lower() != segs[0].lower(): return t.sub[k]
    return None


def const_inner(pkg, n):
    """(inner,) for an EX_ArrayConst / EX_SetConst, (key, value) for an EX_MapConst, from its inner property operands;
    None if one is not found."""
    tys = tuple(inner_ty(pkg, o) for o in n.ops if o[0] == 'prop')
    return tys if tys and all(t is not None for t in tys) else None


def addressable(n):
    """An expression that leaves Stack.MostRecentPropertyAddress: a variable, a struct member or array element of one,
    a context whose member expression is one, or a SwitchValue whose every result is one (execSwitchValue steps the
    chosen term with the result it was given, ScriptCore.cpp 2560-2582)."""
    if n.op in VARIABLES: return True
    if n.op in (0x42, 0x6B): return addressable(n.kids[0])
    if n.op in (0x19, 0x1A, 0x12): return addressable(n.kids[1])
    if n.op == 0x69: return all(addressable(k) for k in n.kids[2::2] + [n.kids[-1]])
    return False


class Types:
    """The static type of each expression of one function (export i), memoised per node."""

    def __init__(s, pkg, i):
        s.pkg, s.i, s.memo = pkg, i, {}
        s.fn = Scope('pkg', pkg, i)
        s.cls = scope(pkg, pkg.exports[i]['outer'])

    def of(s, n, ctx=None):
        k = (id(n), id(ctx))
        if k not in s.memo: s.memo[k] = s._of(n, ctx)
        return s.memo[k]

    def _of(s, n, ctx):
        pkg, o = s.pkg, n.op
        if o in LIT: return LIT[o]
        if o == 0x2F: return Ty('StructProperty', 0, (pkg.path(n.ops[0][1]) or '?').lower())
        if o in VARIABLES or o == 0x42: return fieldpath_ty(pkg, s.i, n.ops[0])
        if o == 0x6B:
            a = s.of(n.kids[0])
            return a.sub if a is not None and a.cls == 'ArrayProperty' else None
        if o in (0x19, 0x1A, 0x12): return s.of(n.kids[1], s.of(n.kids[0], ctx))
        if o == 0x51:
            t = s.of(n.kids[0])
            return Ty('ObjectProperty', 8, t.sub if t is not None else None)
        if o == 0x2E:
            c = scope(pkg, n.ops[0][1])
            if c is None: return None
            return Ty('InterfaceProperty', 16, c.path()) if c.is_interface() else Ty('ObjectProperty', 8, c.path())
        if o == 0x13: return Ty('ClassProperty', 8, (pkg.path(n.ops[0][1]) or '').lower())
        if o in (0x52, 0x54): return Ty('InterfaceProperty', 16, (pkg.path(n.ops[0][1]) or '').lower())
        if o == 0x55: return Ty('ObjectProperty', 8, (pkg.path(n.ops[0][1]) or '').lower())
        if o == 0x38:
            b = cast_byte(pkg, s.i, n)
            return Ty('BoolProperty', 1) if b in (0x47, 0x49) else Ty('InterfaceProperty', 16) if b == 0x46 else None
        if o in CALLS:
            sig = s.callee(n, ctx)
            if sig is None: return None
            r = [t for name, t, fl, dim in sig.props() if fl & CPF_ReturnParm]
            return r[0] if r else Ty('void')
        if o == 0x69:                   # a switch yields whichever result it picks: typed when they all agree
            tys = [s.of(k) for k in switch_results(n)]
            if any(t is None for t in tys): return None
            return tys[0] if all(same(t, tys[0]) for t in tys[1:]) else None
        return None

    def callee(s, n, ctx=None):
        """The Scope of the function a call node reaches: by object for final calls / CallMath, by name up the class
        chain for virtual ones (this function's class, or the context object's class)."""
        kind_, v = n.ops[0][:2]
        if n.op in (0x1C, 0x46, 0x68) and kind_ == 'obj': return scope(s.pkg, v)
        if n.op in (0x1B, 0x45) and kind_ == 'name':
            cls = s.cls if ctx is None else scope_by_path(s.pkg, ctx.sub) if ctx.cls in (
                'ObjectProperty', 'ClassProperty', 'InterfaceProperty', 'WeakObjectProperty', 'LazyObjectProperty') else None
            for c in (cls.chain() if cls else ()):
                f = c.function(v)
                if f: return f
        return None

    def walk(s, n, ctx=None):
        """(node, context Ty) for n and everything under it. A context's member expression runs on the context object,
        whose type decides a call by name there; its object expression runs where the context does
        (ProcessContextOpcode steps it with `this`); anything else - a call's arguments - on the frame's own object
        (Stack.Object)."""
        yield n, ctx
        for j, k in enumerate(n.kids):
            if n.op in (0x19, 0x1A, 0x12): yield from s.walk(k, ctx if j == 0 else s.of(n.kids[0], ctx))
            else: yield from s.walk(k, None)


def nodes_of(pkg, i):
    """(Types, [(node, ctx)]) over export i's whole script."""
    T = Types(pkg, i)
    return T, [x for t in pkg.script(i) for x in T.walk(t)]


def params(sc):
    """A function Scope's parameters in order, the return value left out: [(name, Ty, flags)]."""
    return [(n, t, fl) for n, t, fl, dim in sc.props() if fl & CPF_Parm and not fl & CPF_ReturnParm]


# The UFUNCTIONs whose parameters are wildcards (ArrayParm / SetParam / MapParam / CustomStructureParam /
# ArrayTypeDependentParams meta, read off every UE 4.27 Runtime and Plugins header): a CustomThunk reads them by
# reflection, and their declared type (int32 / TArray<int32>) is a placeholder. UEdGraphSchema_K2::HasWildcardParams.
WILDCARD = {('kismetarraylibrary', f) for f in (
    'array_add', 'array_addunique', 'array_append', 'array_clear', 'array_contains', 'array_find', 'array_get',
    'array_identical', 'array_insert', 'array_isvalidindex', 'array_lastindex', 'array_length', 'array_random',
    'array_randomfromstream', 'array_remove', 'array_removeitem', 'array_resize', 'array_reverse', 'array_set',
    'array_shuffle', 'array_swap', 'setarraypropertybyname')} | {('blueprintsetlibrary', f) for f in (
    'set_add', 'set_additems', 'set_clear', 'set_contains', 'set_difference', 'set_intersection', 'set_length',
    'set_remove', 'set_removeitems', 'set_toarray', 'set_union', 'setsetpropertybyname')} | {('blueprintmaplibrary', f) for f in (
    'map_add', 'map_clear', 'map_contains', 'map_find', 'map_keys', 'map_length', 'map_remove', 'map_values',
    'setmappropertybyname')} | {('datatablefunctionlibrary', 'getdatatablerowfromname'),
                                ('kismetsystemlibrary', 'setstructurepropertybyname'),
                                ('kismetsystemlibrary', 'geteditorproperty'), ('kismetsystemlibrary', 'seteditorproperty'),
                                ('dataregistrysubsystem', 'findcacheditembp'), ('dataregistrysubsystem', 'getcacheditembp'),
                                ('dataregistrysubsystem', 'getcacheditemfromlookupbp'),
                                # FSD's own (no UE header lists it): the game's Blueprints pass it TArray<AActor*> /
                                # TArray<FTransform> for its TArray<int32> (TSK_PlayRandomMontage, BP_Gem_DeepScan_Target_New)
                                ('fsdkismetarrayextensionfunctions', 'array_getrandom'),
                                # and its cheat manager's value store: its cheat widgets pass FString, float and int32
                                # values (Cheat_SetDifficultyRow, Cheat_SpawnEnemy) for the int32 placeholder
                                ('fsdcheatmanager', 'getsavedcheatvalue'), ('fsdcheatmanager', 'setsavedcheatvalue')}


def wildcard(sc):
    """A native whose parameters include wildcards: listed above, or - for the game's own CustomThunks, which no UE
    header lists - one with a `const int32&` parameter (ConstParm | ReferenceParm IntProperty), the placeholder UHT
    reflects a wildcard as; UHT passes a real int32 by value."""
    if sc is None or sc.where != 'sdk': return False
    cls, fn = sc.key.split(':')
    if (cls.split('.')[-1], fn) in WILDCARD: return True
    return any(fl & CPF_ReferenceParm and fl & CPF_ConstParm and t.cls == 'IntProperty' for n, t, fl, dim in sc.props())


def wild_param(f, fl, t):
    """A wildcard parameter of a wildcard native: passed by reference, or a container (its placeholder inner)."""
    return wildcard(f) and bool(fl & (CPF_OutParm | CPF_ReferenceParm) or t.cls in ('ArrayProperty', 'SetProperty', 'MapProperty'))


def lit_fits(pkg, n, t):
    """Whether literal node n writes what a property of Ty t holds (KismetCompilerVMBackend.cpp EmitTermExpr 598-1049
    picks the literal op from the receiving property; each ScriptCore.cpp const handler writes its fixed native size).
    None when t is not known."""
    k = kind(t)
    if k is None: return None
    o = n.op
    if o in (0x20, 0x2A): return k in ('object', 'weak', 'lazy')               # FObjectPropertyBase, soft excluded
    if o == 0x17: return k in ('object', 'weak', 'lazy', 'interface')
    if o == 0x2F: return same(Ty('StructProperty', 0, (pkg.path(n.ops[0][1]) or '?').lower()), t)
    if o == 0x65: return k == 'array'
    if o == 0x3D: return k == 'set'
    if o == 0x3F: return k == 'map'
    if o == 0x67: return k == 'soft'
    return same(LIT[o], t)


def is_call(n): return n.op in CALLS or (n.op in (0x19, 0x1A) and is_call(n.kids[1]))


# ---- AssetGen's layout views
#
# AssetGen reads one layout as another on purpose, through a fixed set of views (the editor never does, and the game's
# own Blueprints hold none): an engine struct whose member at offset 0 has the width it wants - __AddrOf__ reads
# ScreenMessageString.Key (the first 8 bytes of any value: an object's pointer, an array's data), __NameIndex__
# IntPoint.X, __AsObject__ DebugDisplayProperty.obj (an int64 read back as a UObject*) (Cpp.cpp 822-841, 2481-2496,
# 4926-4930); the kReadViews, a TArray<T> at offset 0 read over an FDeref scratch whose Data is a raw address (1030-1062,
# ViewFieldOf 705-713, HoistRefAt 8009-8064); and its own view structs - FDeref, FDerefTextView, FMapSlot_* /
# FMapSlots_* (a TMap's storage read as a TArray), FNC_* (a one-member struct wrapping a nested container) (58-66,
# 1466-1475). The VM applies a member's offset and copies a value with no type check (ScriptCore.cpp
# execStructMemberContext 2957-2995, execLet 2647-2686, ProcessScriptFunction 873-900), so it needs only that a view
# stays inside the memory it reads: typing_view_puns checks that. The typing rules leave exactly these positions to it,
# and hold everything else to the types the editor writes.

VIEW_MEMBERS = {('/script/engine.screenmessagestring', 'key'), ('/script/coreuobject.intpoint', 'x'),
                ('/script/engine.debugdisplayproperty', 'obj'),
                ('/script/engine.materialcachedparameterentry', 'namehashes'), ('/script/engine.lodmappingdata', 'mapping'),
                ('/script/engine.customprimitivedata', 'data'), ('/script/engine.bandwidthtestitem', 'kilobyte'),
                ('/script/engine.assetmanagersearchrules', 'assetscanpaths')}


def view_struct(path):
    """One of AssetGen's own view structs, by its lower-case path (IsInternalViewStruct, NestedWrapper)."""
    name = (path or '').split('.')[-1]
    return name in ('fderef', 'fdereftextview') or name.startswith(('fmapslot_', 'fmapslots_', 'fnc_'))


def _member_expr(n):
    while n.op in (0x19, 0x1A): n = n.kids[1]
    return n


def view_member(pkg, n):
    """n (through any context) is an EX_StructMemberContext naming a view member or a member of a view struct."""
    n = _member_expr(n)
    if n.op != 0x42: return False
    owner = (pkg.path(n.ops[0][2]) or '').lower()
    return (owner, n.ops[0][1][-1].lower()) in VIEW_MEMBERS or view_struct(owner)


def raw_read(pkg, n):
    """n reads memory at a raw address: a view member, or an EX_ArrayGetByRef element of one (the FDeref read)."""
    n = _member_expr(n)
    return view_member(pkg, n) or (n.op == 0x6B and view_member(pkg, n.kids[0]))


def is_view_ty(t):
    return t is not None and t.cls == 'StructProperty' and view_struct(t.sub)


def member_pun(pkg, T, n):
    """An EX_StructMemberContext that is one of AssetGen's views: a view member, a member of a struct read through a
    raw address (`P->Field`), or a member of a view struct."""
    return view_member(pkg, n) or raw_read(pkg, n.kids[0]) or is_view_ty(T.of(n.kids[0]))


def value_pun(pkg, T, value, slot):
    """A value stepped into a slot of another type as one of AssetGen's views: a raw read, or a view struct either side."""
    return raw_read(pkg, value) or is_view_ty(T.of(value)) or is_view_ty(slot)


WIDTH = {'i1': 1, 'i2': 2, 'i4': 4, 'i8': 8, 'object': 8, 'float': 4, 'double': 8, 'name': 8, 'str': 16, 'text': 24,
         'interface': 16, 'delegate': 16, 'mcdelegate': 16, 'array': 16, 'set': 80, 'map': 80, 'soft': 40,
         'fieldpath': 32}


def width(t):
    """The bytes a value of Ty t occupies where the VM steps it (a struct its own size), or None."""
    if t is None: return None
    if t.cls == 'StructProperty': return t.size or None
    return WIDTH.get(storage(t))


def member_offset(sc, name):
    """A member's offset in its struct: the SDK's for a native struct, 0 for a package struct's first property."""
    if sc is None: return None
    if sc.where == 'sdk': return sdk().offsets.get((sc.key, name.lower())) if hasattr(sdk(), 'offsets') else None
    props = sc.props()
    return 0 if props and props[0][0].lower() == name.lower() and not sc.super() else None


# The fixed local each assignment handler steps its value into (ScriptCore.cpp execLetBool 2794 `bool NewValue`,
# execLetObj / execLetWeakObjPtr 2713 / 2750 `UObject* NewValue`, execLetDelegate 2815, execLetMulticastDelegate 2835);
# EX_Let steps it straight into the destination.
LET_SLOT = {0x14: Ty('BoolProperty', 1), 0x5F: Ty('ObjectProperty', 8), 0x60: Ty('ObjectProperty', 8),
            0x44: Ty('DelegateProperty', 16), 0x43: Ty('MulticastInlineDelegateProperty', 16)}


# ---- rules

@rule
def typing_literal_receiver(pkg):
    """A literal's opcode writes the type and size of the property it is written into: a call argument its parameter,
    a Let value its destination, a StructConst member its member, a SetArray / SetSet / SetMap element the container's
    inner / key / value, an ArrayConst / SetConst / MapConst element the inner property it names, a SwitchValue case
    the index, a Return value the return property - and a SwitchValue in any of those places, each result and its
    default; EX_LetBool / EX_LetObj / EX_LetDelegate step theirs into a fixed local (LET_SLOT). Each const handler
    writes its fixed size into RESULT_PARAM (ScriptCore.cpp execIntConst 3142, execFloatConst 3167, execByteConst 3329,
    execVectorConst 3343, ...), so the engine needs the width: EX_IntConst into a 1-byte parameter overwrites the 3
    bytes after it. The kind beyond the width (EX_FloatConst into an int32) is the editor's, which picks the op from
    the receiving property (KismetCompilerVMBackend.cpp EmitTermExpr 598-1049), and the game keeps it: a wrong kind of
    the right width is a wrong value. An ArrayConst / SetConst / MapConst builds its container with the element size
    of the inner property it names (3499-3559), so that property has the receiving container's inner type. Wildcard
    parameters of a CustomThunk (KismetArrayLibrary Array_*, ...) are placeholders and are not checked."""
    me = 'typing_literal_receiver'
    for i, st in functions(pkg):
        T, nodes = nodes_of(pkg, i)
        ret = [t for n_, t, fl, dim in T.fn.props() if fl & CPF_ReturnParm]
        for n, ctx in nodes:
            pairs = []
            if n.op in (0x65, 0x3D, 0x3F):
                tys = const_inner(pkg, n)
                if tys: pairs += [(a, tys[j % len(tys)], 'constant element') for j, a in enumerate(n.kids[:-1])]
            if n.op in CALLS:
                f = T.callee(n, ctx)
                if f is None: continue
                for a, (pn, pt, fl) in zip([k for k in n.kids if k.op != 0x16], params(f)):
                    if wild_param(f, fl, pt): continue
                    pairs.append((a, pt, 'argument %s of %s' % (pn, f.path())))
            elif n.op in LETS:
                pairs.append((n.kids[1], LET_SLOT.get(n.op) or T.of(n.kids[0]), 'assignment'))
            elif n.op == 0x2F:
                for a, (mn, mt) in zip(n.kids[:-1], struct_link(scope(pkg, n.ops[0][1]))):
                    pairs.append((a, mt, 'member %s' % mn))
            elif n.op in (0x31, 0x39, 0x3B):
                c = T.of(n.kids[0])
                if c is None or c.sub is None: continue
                els = n.kids[1:-1]
                if n.op == 0x3B and isinstance(c.sub, tuple):
                    pairs += [(a, c.sub[j % 2], 'map %s' % ('key', 'value')[j % 2]) for j, a in enumerate(els)]
                elif n.op != 0x3B and not isinstance(c.sub, tuple):
                    pairs += [(a, c.sub, 'element') for a in els]
            elif n.op == 0x69:
                count = n.ops[0][1]
                idx = T.of(n.kids[0])
                pairs += [(n.kids[1 + 2 * c], idx, 'switch case') for c in range(count)]
            elif n.op == 0x04 and ret:
                pairs.append((n.kids[0], ret[0], 'return value'))
            for a0, t, where in pairs:
                for a in values(a0):
                    if a.op in LITERALS and judged(me, lit_fits(pkg, a, t)) is False:
                        yield i, 'op %02x at mem %d writes %r into %s (%r)' % (a.op, a.mem, LIT.get(a.op, a.op), where, t)
                    if a.op in (0x65, 0x3D, 0x3F) and t is not None:
                        tys, want = const_inner(pkg, a), (t.sub if isinstance(t.sub, tuple) else (t.sub,))
                        if tys and len(tys) == len(want) and judged(me, None if None in [same(x, y) for x, y in zip(tys, want)]
                                                                    else all(same(x, y) for x, y in zip(tys, want))) is False:
                            yield i, 'op %02x at mem %d builds %r elements into %s (%r)' % (a.op, a.mem, tys, where, t)


def struct_link(sc):
    """A struct's PropertyLink as EX_StructConst steps it (ScriptCore.cpp execStructConst 3376-3406, Class.cpp
    UStruct::Link 948-981 - TFieldIterator: own properties, then the super's): [(name, Ty)] per ArrayDim element,
    CPF_Transient and CPF_EditorOnly left out. [] if the struct is not found."""
    out = []
    for c in (sc.chain() if sc else ()):
        out += [(n, t) for n, t, fl, dim in c.props() if not fl & (CPF_Transient | CPF_EditorOnly) for _ in range(dim)]
    return out


@rule
def typing_struct_const_members(pkg):
    """EX_StructConst has exactly one expression per element of each property in the struct's PropertyLink, Transient
    and EditorOnly left out, before EX_EndStructConst: execStructConst steps that list and P_FINISH skips one byte
    blind (ScriptCore.cpp 3376-3406), so a missing or extra member desyncs the stream. Each non-literal member has its
    member's type (a literal is typing_literal_receiver's)."""
    for i, st in functions(pkg):
        T, nodes = nodes_of(pkg, i)
        for n, ctx in nodes:
            if n.op != 0x2F: continue
            sc = scope(pkg, n.ops[0][1])
            if sc is None or (sc.where == 'sdk' and not sc.sdk()['props'] and not sc.super()): continue
            link = struct_link(sc)
            kids = n.kids[:-1]
            if judged('typing_struct_const_members', len(kids) == len(link)) is False:
                yield i, 'StructConst %s at mem %d has %d members, its PropertyLink %d' % (sc.path(), n.mem, len(kids), len(link))
                continue
            for a, (mn, mt) in zip(kids, link):
                if a.op not in LITERALS and judged('typing_struct_const_members', same(T.of(a), mt)) is False:
                    yield i, 'StructConst %s member %s gets %r (op %02x), not %r' % (sc.path(), mn, T.of(a), a.op, mt)


@rule
def typing_let_opcode(pkg):
    """Each assignment opcode handles its destination's property kind, and its value has the type the handler steps
    it into. EX_LetBool needs an FBoolProperty (ScriptCore.cpp execLetBool 2762-2803: ExactCastField, then
    SetPropertyValue through it) and a bool value (a bool local, 2794); EX_LetObj / EX_LetWeakObjPtr an
    FObjectPropertyBase and a UObject* value (2688-2759); EX_LetDelegate a delegate (2807-2822);
    EX_LetMulticastDelegate a multicast delegate (2826-2842). EX_Let steps its value straight into the destination
    (2647-2686), never into a bitfield bool, whose byte it would overwrite whole (PropertyBool.cpp 365-374: only
    SetPropertyValue masks), nor a weak / lazy pointer, which a value is read as a UObject*. The engine needs the
    value's width to be the slot's; the kind beyond that (a float into an int32) is what the editor writes and the
    game keeps. EX_Let's property operand is empty or the destination's own property: the engine needs it to be at
    least the value's size, as execLet sizes the fallback temporary for a None destination by it (2665-2675), and the
    editor always writes the destination's own. AssetGen's layout views (VIEW_MEMBERS) are typing_view_puns'."""
    need = {0x14: ('bool',), 0x5F: ('object', 'weak', 'lazy', 'soft'), 0x60: ('object', 'weak', 'lazy', 'soft'),
            0x44: ('delegate',), 0x43: ('mcdelegate', 'sparse')}
    value = {0x14: ('i1',), 0x5F: ('object',), 0x60: ('object',), 0x44: ('delegate',), 0x43: ('mcdelegate',)}
    me = 'typing_let_opcode'
    for i, st in functions(pkg):
        T, nodes = nodes_of(pkg, i)
        for n, ctx in nodes:
            if n.op not in LETS: continue
            dest, val = n.kids
            dt, vt = T.of(dest), T.of(val)
            dk, vk = kind(dt), kind(vt)
            if n.op in need:
                if judged(me, None if dk is None else dk in need[n.op]) is False:
                    yield i, 'op %02x at mem %d assigns a %r destination' % (n.op, n.mem, dt)
                if vk is not None and vt.cls != 'void' and val.op not in LITERALS \
                        and judged(me, STORAGE.get(vk, vk) in value[n.op]) is False and not value_pun(pkg, T, val, LET_SLOT[n.op]):
                    yield i, 'op %02x at mem %d steps a %r value (op %02x) into its %s local' % (n.op, n.mem, vt, val.op, value[n.op][0])
                continue
            if judged(me, None if dt is None else not dt.bitfield) is False:
                yield i, 'EX_Let at mem %d writes a whole byte into the bitfield bool %r' % (n.mem, dest.ops[0][1] if dest.ops else '?')
            if dk in ('weak', 'lazy'):
                yield i, 'EX_Let at mem %d steps a value into the %r destination, which the VM reads and writes as a UObject*' % (n.mem, dt)
            if val.op not in LITERALS and judged(me, same(vt, dt)) is False and not value_pun(pkg, T, val, dt):
                yield i, 'EX_Let at mem %d steps a %r value (op %02x) into a %r destination' % (n.mem, vt, val.op, dt)
            pt = fieldpath_ty(pkg, i, n.ops[0]) if n.ops and n.ops[0][1] else None
            if pt is not None and judged(me, same(pt, dt)) is False:
                yield i, 'EX_Let at mem %d names a %r property for a %r destination' % (n.mem, pt, dt)


@rule
def typing_let_destination(pkg):
    """An assignment's destination, a container a SetArray / SetSet / SetMap fills, and a multicast delegate an Add /
    Remove / Clear / Call names, are addressable variables: the handler steps them with no result buffer and writes
    through Stack.MostRecentPropertyAddress (ScriptCore.cpp execLet 2647, execLetBool 2764, execLetObj 2690,
    execSetArray 3411, execAddMulticastDelegate 3087, ...); a constant or a call there writes through null. The
    container ops also need their container's kind (CastFieldChecked, 3416 / 3440 / 3473), and the delegate ops a
    multicast delegate (3032-3140)."""
    ops = {0x0F: None, 0x14: None, 0x5F: None, 0x60: None, 0x44: None, 0x43: None, 0x31: ('array',), 0x39: ('set',),
           0x3B: ('map',), 0x5C: ('mcdelegate', 'sparse'), 0x62: ('mcdelegate', 'sparse'), 0x5D: ('mcdelegate', 'sparse'),
           0x63: ('mcdelegate', 'sparse'), 0x61: ('delegate',)}
    for i, st in functions(pkg):
        T, nodes = nodes_of(pkg, i)
        for n, ctx in nodes:
            if n.op not in ops: continue
            dest = n.kids[0]
            if judged('typing_let_destination', addressable(dest)) is False:
                yield i, 'op %02x at mem %d assigns through op %02x, which leaves no address' % (n.op, n.mem, dest.op)
                continue
            k = kind(T.of(dest))
            if ops[n.op] and k is not None and judged('typing_let_destination', k in ops[n.op]) is False:
                yield i, 'op %02x at mem %d needs a %s, its destination is %r' % (n.op, n.mem, ops[n.op][0], T.of(dest))


def struct_chain_paths(pkg, path):
    sc = scope_by_path(pkg, path)
    return [c.path() for c in sc.chain()] if sc else [path]


@rule
def typing_struct_member_owner(pkg):
    """EX_StructMemberContext names a member of the struct its expression produces, or of a super struct, and that
    expression is addressable: execStructMemberContext steps it with no result buffer and adds the member's offset to
    MostRecentPropertyAddress (ScriptCore.cpp 2957-2995), so a literal or call there reads through null. The engine
    checks no type: a foreign member reads another layout, which the editor never writes and the game has none of.
    AssetGen's own layout views (VIEW_MEMBERS, view structs, a member read through a raw address) do it on purpose:
    those are typing_view_puns'."""
    for i, st in functions(pkg):
        T, nodes = nodes_of(pkg, i)
        for n, ctx in nodes:
            if n.op != 0x42: continue
            base = n.kids[0]
            me = 'typing_struct_member_owner'
            if judged(me, addressable(base)) is False:
                yield i, 'StructMemberContext at mem %d reads through op %02x, which leaves no address' % (n.mem, base.op)
                continue
            bt = T.of(base)
            owner = (pkg.path(n.ops[0][2]) or '').lower()
            if bt is None or not owner: continue
            if judged(me, bt.cls == 'StructProperty' or member_pun(pkg, T, n)) is False:
                yield i, 'StructMemberContext at mem %d offsets into a %r' % (n.mem, bt); continue
            if bt.cls == 'StructProperty' and bt.sub and not bt.sub.startswith('?') and judged(
                    me, owner in struct_chain_paths(pkg, bt.sub) or member_pun(pkg, T, n)) is False:
                yield i, 'StructMemberContext at mem %d names %s of %s in a %s' % (n.mem, '.'.join(n.ops[0][1]), owner, bt.sub)


@rule
def typing_return_operand(pkg):
    """EX_Return's operand fits the function's return property. execReturn steps any operand but EX_Nothing into
    RESULT_PARAM (ScriptCore.cpp 1123-1133), which is null for a ProcessEvent call of a function with no return
    value (ReturnValueOffset unset, 2014-2016): there the operand is EX_Nothing or a call that itself returns nothing.
    With a return property, the operand is EX_Nothing (the body's EX_LocalOutVariable writes already landed, 834-847)
    or a value of the return property's type. A call whose callee is not found is not checked."""
    for i, st in functions(pkg):
        T = Types(pkg, i)
        ret = [t for n_, t, fl, dim in T.fn.props() if fl & CPF_ReturnParm]
        for t in pkg.script(i):
            if t.op != 0x04: continue
            v = t.kids[0]
            if v.op == 0x0B: CHECKED['typing_return_operand'] += 1; continue
            vt = T.of(v)
            if is_call(v) and vt is None: continue
            CHECKED['typing_return_operand'] += 1
            if not ret:
                if not (is_call(v) and vt.cls == 'void'):
                    yield i, 'EX_Return at mem %d steps op %02x (%r) into the null result of a function with no return value' % (t.mem, v.op, vt)
            elif vt is not None and vt.cls != 'void' and v.op not in LITERALS and same(vt, ret[0]) is False:
                yield i, 'EX_Return at mem %d steps a %r into the %r return value' % (t.mem, vt, ret[0])
            elif vt is not None and vt.cls == 'void':
                yield i, 'EX_Return at mem %d steps a call that returns nothing into the %r return value' % (t.mem, ret[0])


@rule
def typing_cast_operands(pkg):
    """A cast's operand has the width its handler steps it into: EX_DynamicCast, EX_MetaCast, EX_ObjToInterfaceCast
    and CST_ObjectToBool a UObject* (ScriptCore.cpp 3611, 3657, 3702, 3679), EX_CrossInterfaceCast,
    EX_InterfaceToObjCast and CST_InterfaceToBool an FScriptInterface (3729, 3754, 3687). An interface stepped into a
    UObject* local overwrites 8 bytes of the native stack. EX_MetaCast's class is not an interface: the engine runs it,
    but no class is a child of an interface, so it always yields null (3660) - the editor refuses that cast
    (DynamicCastHandler.cpp 113-118, "Interfaces are not supported") and the game has none."""
    need = {0x2E: 'object', 0x13: 'object', 0x52: 'object', 0x54: 'interface', 0x55: 'interface'}
    for i, st in functions(pkg):
        T, nodes = nodes_of(pkg, i)
        for n, ctx in nodes:
            if n.op not in CASTS: continue
            want = need.get(n.op)
            if n.op == 0x38:
                want = {0x47: 'object', 0x49: 'interface'}.get(cast_byte(pkg, i, n))
            k = storage(T.of(n.kids[0]))
            if want and k is not None and judged('typing_cast_operands', k == want) is False:
                yield i, 'op %02x at mem %d casts a %r, its handler steps a %s' % (n.op, n.mem, T.of(n.kids[0]), want)
            if n.op == 0x13:
                c = scope(pkg, n.ops[0][1])
                if c is not None and c.is_interface():
                    yield i, 'MetaCast at mem %d to the interface %s' % (n.mem, c.path())


@rule
def typing_call_arguments(pkg):
    """Each call argument produces its parameter's type and size (a literal's is typing_literal_receiver's): a script
    callee steps it into the frame at the parameter's offset, a native thunk into a local of the parameter's C++ type
    (ScriptCore.cpp 891-900; ScriptMacros.h 48-58), so the engine needs the width - a wider value overwrites what
    follows; the kind beyond that is the editor's, which the game keeps. A reference (out) parameter of a script
    callee is an addressable variable: ProcessScriptFunction steps it with no buffer and keeps
    MostRecentPropertyAddress (ScriptCore.cpp 865-885); with none, an ensure fires and the callee writes its own
    frame, so the caller never sees the value (876-877). A call returning nothing passes nothing, which the editor
    never does. AssetGen's layout views (VIEW_MEMBERS) are typing_view_puns'."""
    for i, st in functions(pkg):
        T, nodes = nodes_of(pkg, i)
        for n, ctx in nodes:
            if n.op not in CALLS and n.op != 0x63: continue
            f = T.callee(n, ctx) if n.op != 0x63 else scope(pkg, n.ops[0][1])
            if f is None: continue
            args = [k for k in (n.kids if n.op != 0x63 else n.kids[1:]) if k.op != 0x16]
            for a, (pn, pt, fl) in zip(args, params(f)):
                if wild_param(f, fl, pt): continue
                if not f.native() and fl & CPF_OutParm and not addressable(a):
                    yield i, 'call at mem %d passes op %02x to the reference parameter %s of %s' % (n.mem, a.op, pn, f.path())
                if a.op in LITERALS: continue
                at = T.of(a)
                if at is not None and at.cls == 'void':
                    yield i, 'call at mem %d passes a call returning nothing as %s of %s' % (n.mem, pn, f.path()); continue
                if judged('typing_call_arguments', same(at, pt)) is False and not value_pun(pkg, T, a, pt):
                    yield i, 'call at mem %d passes a %r (op %02x) as %s %r of %s' % (n.mem, at, a.op, pn, pt, f.path())


@rule
def typing_slots(pkg):
    """Sub-expressions the VM steps into fixed native locals have that local's type (ScriptCore.cpp): the object of
    EX_Context / EX_Context_FailSilent / EX_ClassContext a UObject* (2883, 2212; an interface goes through
    EX_InterfaceContext, whose operand is an FScriptInterface, 2198); the EX_JumpIfNot and EX_PopExecutionFlowIfNot
    condition a bool (2406, 2478); the EX_ComputedJump operand and the EX_ArrayGetByRef index an int32 (2389, 2602),
    and EX_ArrayGetByRef's array an addressable TArray (2590-2609); the EX_AddMulticastDelegate /
    EX_RemoveMulticastDelegate r-value an FScriptDelegate (3095, 3115); the EX_BindDelegate object a UObject*
    (3313). A wider value overwrites the native stack."""
    for i, st in functions(pkg):
        T, nodes = nodes_of(pkg, i)
        for n, ctx in nodes:
            o = n.op
            slots = []
            if o in (0x19, 0x1A, 0x12): slots.append((n.kids[0], ('object',), 'context object'))
            elif o == 0x51: slots.append((n.kids[0], ('interface', 'object'), 'interface context'))   # an object fills its first half
            elif o in (0x07, 0x4F): slots.append((n.kids[0], ('i1',), 'condition'))
            elif o == 0x4E: slots.append((n.kids[0], ('i4',), 'computed jump'))
            elif o == 0x6B:
                slots.append((n.kids[1], ('i4',), 'array index'))
                slots.append((n.kids[0], ('array',), 'array'))
                if not addressable(n.kids[0]):
                    yield i, 'ArrayGetByRef at mem %d indexes op %02x, which leaves no address' % (n.mem, n.kids[0].op)
            elif o in (0x5C, 0x62): slots.append((n.kids[1], ('delegate',), 'delegate'))
            elif o == 0x61: slots.append((n.kids[1], ('object',), 'bound object'))
            for a, want, what in slots:
                k = storage(T.of(a))
                if k is not None and T.of(a).cls != 'void' and judged('typing_slots', k in want) is False:
                    yield i, 'op %02x at mem %d: its %s is a %r (op %02x), the VM steps a %s' % (o, n.mem, what, T.of(a), a.op, want[0])


# Every EExprToken of Script.h 4.27 (163-276) that the loader transfers (ScriptSerialization.h) and a GNatives handler
# runs or its parent consumes (the End* terminators, EX_EndFunctionParms, EX_Return, EX_EndOfScript).
OPCODES_427 = {0x00, 0x01, 0x02, 0x04, 0x06, 0x07, 0x09, 0x0B, 0x0F, 0x12, 0x13, 0x14, 0x16, 0x17, 0x19, 0x1A, 0x1B,
               0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C,
               0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
               0x3F, 0x40, 0x42, 0x43, 0x44, 0x45, 0x46, 0x48, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53,
               0x54, 0x55, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
               0x69, 0x6B, 0x6C, 0x6D}


@rule
def typing_opcodes_known(pkg):
    """Every opcode in a script is one UE 4.27 both loads and runs (Script.h 163-276): an unassigned byte loads as a
    one-byte no-op with a warning (ScriptSerialization.h 551-556) and runs as execUndefined, which logs "Unknown code
    token" and reads none of its operands (ScriptCore.cpp 2045-2048; every GNatives slot starts as it, 698-708). EX_Skip
    (0x18) and EX_EndParmValue (0x15) load (ScriptSerialization.h 253, 510-515) but register no handler, so they run as
    execUndefined; EX_DeprecatedOp4A does run, as a no-op (ScriptCore.cpp 2282-2287). EX_InstrumentationEvent (0x6A)
    transfers no operand bytes at load and reads its event type from memory never written (ScriptSerialization.h
    275-281). A script that does not decode is a finding too."""
    for i, st in functions(pkg):
        try: top = pkg.script(i)
        except (Exception, SystemExit) as e:
            yield i, 'script does not decode: %s' % e; continue
        CHECKED['typing_opcodes_known'] += 1
        for t in top:
            for n in t.walk():
                if n.op not in OPCODES_427:
                    yield i, 'op %02x at mem %d is not a UE 4.27 opcode the loader and the VM both implement' % (n.op, n.mem)


@rule
def typing_operand_grammar(pkg):
    """The operands a VM handler trusts. EX_SwitchValue: each case's next-case offset is the end of that case's result
    and the end offset the end of the default, absolute (ScriptCore.cpp 2519-2584 sets Code = &Script[offset]), and
    the index is addressable - it is stepped with no result and IndexProperty is dereferenced (2524-2546). EX_SetSet /
    EX_SetMap: no element follows a count of 0 (3438-3459, 3471-3493: with 0 the loop is skipped, the End token only
    check()ed, and P_FINISH skips one byte - of the first element; a count above the elements only reserves), and a
    map's expressions pair up (each pass steps a key and a value). EX_ArrayConst: the count is the number of elements
    (an ensure, 3513: the engine reports a mismatch and goes on, the count having only reserved). EX_TextConst: a literal type of 0..4 with that type's string operands (ScriptSerialization.h
    73-105, ScriptCore.cpp 3198-3263). EX_PrimitiveCast: CST_ObjectToBool (0x47) or CST_InterfaceToBool (0x49) -
    CST_ObjectToInterface runs execObjectToInterface on an operand stream laid out for neither (734-762, 3693)."""
    for i, st in functions(pkg):
        for t in pkg.script(i):
            for n in t.walk():
                if n.op in (0x69, 0x39, 0x3B, 0x65, 0x29, 0x38): CHECKED['typing_operand_grammar'] += 1
                if n.op == 0x69:
                    ints = [v for k, v, *_ in n.ops if k == 'int']
                    count, end, nexts = ints[0], ints[1], ints[2:]
                    for c in range(count):
                        r = n.kids[2 + 2 * c]
                        if c < len(nexts) and nexts[c] != r.mem + r.size:
                            yield i, 'SwitchValue at mem %d: case %d skips to %d, its result ends at %d' % (n.mem, c, nexts[c], r.mem + r.size)
                    d = n.kids[-1]
                    if end != d.mem + d.size:
                        yield i, 'SwitchValue at mem %d ends at %d, its default at %d' % (n.mem, end, d.mem + d.size)
                    if not addressable(n.kids[0]):
                        yield i, 'SwitchValue at mem %d: its index (op %02x) leaves no address' % (n.mem, n.kids[0].op)
                elif n.op in (0x39, 0x3B):
                    count = next(v for k, v, *_ in n.ops if k == 'int')
                    els = len(n.kids) - 2
                    if count == 0 and els:
                        yield i, 'op %02x at mem %d says 0 elements and has %d' % (n.op, n.mem, els // (2 if n.op == 0x3B else 1))
                    if n.op == 0x3B and els % 2:
                        yield i, 'SetMap at mem %d has an odd %d key / value expressions' % (n.mem, els)
                elif n.op == 0x65:
                    count = next(v for k, v, *_ in n.ops if k == 'int')
                    if count != len(n.kids) - 1:
                        yield i, 'ArrayConst at mem %d says %d elements and has %d' % (n.mem, count, len(n.kids) - 1)
                elif n.op == 0x29:
                    lt = cast_byte(pkg, i, n)
                    want = {0: 0, 1: 3, 2: 1, 3: 1, 4: 2}.get(lt)
                    if want is None:
                        yield i, 'TextConst at mem %d has literal type %d' % (n.mem, lt)
                    elif len(n.kids) != want or any(k.op not in (0x1F, 0x34) for k in n.kids):
                        yield i, 'TextConst at mem %d type %d has operands %s' % (n.mem, lt, ['%02x' % k.op for k in n.kids])
                elif n.op == 0x38:
                    b = cast_byte(pkg, i, n)
                    if b not in (0x47, 0x49):
                        yield i, 'PrimitiveCast at mem %d with cast %#x' % (n.mem, b)


@rule
def typing_callmath_callee(pkg):
    """EX_CallMath calls a function that is FUNC_Static | FUNC_Final | FUNC_Native, carries no net, authority-only or
    cosmetic flag, is not an interface's, and has no wildcard parameter (KismetCompilerVMBackend.cpp 1222-1231): it
    runs GetNativeFunc() on the class default object with no context (ScriptCore.cpp execCallMathFunction 937-959). A
    script function's native is ProcessInternal, which runs the caller's own remaining bytecode; a net function skips
    its callspace; and a wildcard CustomThunk that fails (a null container, KismetArrayLibrary.h 278-286) sets
    bArrayContextFailed and returns without consuming its arguments, which only a context rewinds (2896-2902) -
    outside one the next statement is the argument list, ending in EX_EndFunctionParms, whose handler steps back onto
    itself forever (2366-2370)."""
    for i, st in functions(pkg):
        for t in pkg.script(i):
            for n in t.walk():
                if n.op != 0x68: continue
                f = scope(pkg, n.ops[0][1])
                if f is None: continue
                CHECKED['typing_callmath_callee'] += 1
                ff = f.function_flags()
                name = f.path()
                if not f.native():
                    yield i, 'CallMath at mem %d calls the script function %s' % (n.mem, name); continue
                if ff is not None and (ff & 0x2401 != 0x2401 or ff & FUNC_Forbidden_CallMath):
                    yield i, 'CallMath at mem %d calls %s, FunctionFlags %#x' % (n.mem, name, ff)
                cls = Scope('sdk', key=f.key.split(':')[0])
                if cls.is_interface():
                    yield i, 'CallMath at mem %d calls the interface function %s' % (n.mem, name)
                if wildcard(f):
                    yield i, 'CallMath at mem %d calls %s, whose wildcard CustomThunk needs a context' % (n.mem, name)


@rule
def typing_switch_cases(pkg):
    """EX_SwitchValue's case values have its index property's type, and its results and default one type.
    execSwitchValue steps each case value into a temporary of IndexProperty->GetSize() bytes and compares it with
    IndexProperty->Identical (ScriptCore.cpp 2546-2553), and steps whichever result it picks, or the default, into the
    one RESULT_PARAM it was given (2555, 2582): a case value wider than the index overruns the temporary, and a result
    of another type than the others does not fit the slot they fill. The editor coerces every case to the index
    property and every result to the default term's property (KismetCompilerVMBackend.cpp EmitSwitchValue 1832,
    1835). A literal case value is typing_literal_receiver's."""
    me = 'typing_switch_cases'
    for i, st in functions(pkg):
        T, nodes = nodes_of(pkg, i)
        for n, ctx in nodes:
            if n.op != 0x69: continue
            idx = T.of(n.kids[0])
            for c in range(n.ops[0][1]):
                v = n.kids[1 + 2 * c]
                if v.op not in LITERALS and judged(me, same(T.of(v), idx)) is False:
                    yield i, 'SwitchValue at mem %d: case %d is a %r (op %02x), its index a %r' % (n.mem, c, T.of(v), v.op, idx)
            res = [(r, T.of(r)) for r in switch_results(n)]
            first = next(((r, t) for r, t in res if t is not None and r.op not in LITERALS), None)
            if first is None: continue
            for r, t in res:
                if r is first[0]: continue
                ok = lit_fits(pkg, r, first[1]) if r.op in LITERALS else same(t, first[1])
                if judged(me, ok) is False:
                    yield i, 'SwitchValue at mem %d: a result at mem %d is a %r (op %02x), another a %r' % (n.mem, r.mem, t, r.op, first[1])
