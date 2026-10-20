#!/usr/bin/env python3
"""usage: bpdecomp.py <Content dir> <out dir> --mod </Game/root>=<ModName> [--mod ...] [--names <uncooked Content dir>]
                      [--only <package path prefix>]

A decompiler from cooked UE 4.27 Blueprint packages back to AssetGen C++: every UserDefinedEnum, UserDefinedStruct,
Blueprint interface, Blueprint class and widget Blueprint (its tree, event bindings and render-transform animations
laid out in its constructor) under each --mod root becomes a header (the declaration) and, for a class with code, a
.cpp (the bodies), laid out in folders that mirror the package paths, so `bpbuild` cooks them back to the same
paths. Each mod gets an umbrella header that includes everything in dependency order and a <ModName>.cpp holding its
UE_MOD_PACKAGE.

The bodies are rebuilt from the bytecode alone: the execution-flow stack is resolved statically (each Pop names the
address its Push left, per program point), the control-flow graph is structured into if / while / goto with Ramsey's
"Beyond Relooper" algorithm, and the editor's temporaries are folded back into the expressions they were cut from
where that cannot change what runs. --names points at the uncooked editor assets the packages were cooked from, for
what cooking strips: an enum's display names.

What is not decompiled is written as a `// bpdecomp:` comment where it was, and listed at the end of the run, so the
output always says what it is missing rather than compiling into something else."""
import json, os, re, struct, sys
from collections import defaultdict, OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import invariants

HERE = os.path.dirname(os.path.abspath(__file__))
UEAPI = os.path.normpath(os.path.join(HERE, '..', '..', 'BpMods', 'UeApi'))

# ---- property and function flags (ObjectMacros.h / Script.h, 4.27)

CPF_Parm, CPF_OutParm, CPF_ReturnParm, CPF_ConstParm, CPF_ReferenceParm = 0x80, 0x100, 0x400, 0x2, 0x8000000
CPF_Transient, CPF_Net, CPF_RepNotify, CPF_DisableEditOnInstance = 0x2000, 0x20, 0x100000000, 0x10000
CPF_BlueprintReadOnly, CPF_Edit, CPF_ExposeOnSpawn = 0x10, 0x1, 0x1000000000000
FUNC_Final, FUNC_BlueprintAuthorityOnly, FUNC_BlueprintCosmetic, FUNC_Net = 0x1, 0x4, 0x8, 0x40
FUNC_NetReliable, FUNC_Event, FUNC_Static, FUNC_NetMulticast = 0x80, 0x800, 0x2000, 0x4000
FUNC_UbergraphFunction, FUNC_Public, FUNC_Private, FUNC_Protected = 0x8000, 0x20000, 0x40000, 0x80000
FUNC_Delegate, FUNC_NetServer, FUNC_HasOutParms, FUNC_HasDefaults = 0x100000, 0x200000, 0x400000, 0x800000
FUNC_NetClient, FUNC_BlueprintCallable, FUNC_BlueprintEvent, FUNC_BlueprintPure = 0x1000000, 0x4000000, 0x8000000, 0x10000000
FUNC_Const = 0x40000000
CLASS_Interface, CLASS_Abstract, CLASS_Const = 0x4000, 0x1, 0x10000

CPP_KEYWORDS = set('''alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t
class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default
delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable
namespace new noexcept not not_eq nullptr operator or or_eq private protected public register reinterpret_cast requires
return short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try
typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq EOF NULL TRUE FALSE
min max'''.split())


def ident(name):
    """A C++ identifier for a UE name: what C++ cannot spell becomes '_', a leading digit gets a '_' in front, and a
    keyword or a C macro name a '_' behind."""
    s = re.sub(r'\W', '_', name)
    if not s or s[0].isdigit(): s = '_' + s
    if s in CPP_KEYWORDS: s += '_'
    return s


def authored(name):
    """A UserDefinedStruct member's authored name: `Kills_3_0123...` without the `_<n>_<32 hex>` the editor appends."""
    m = re.match(r'^(.*)_\d+_[0-9A-F]{32}$', name)
    return m.group(1) if m else name


# ---- what UeApi knows: native classes, structs, enums, the game's Blueprint classes, operator and conversion tables

class Api:
    def __init__(s, ueapi=UEAPI):
        s.dir = ueapi
        s.classes = {}          # (package, name) -> C++ name, for /Script classes and /Game Blueprint classes
        s.headers = {}          # C++ class name -> header (relative to UeApi)
        s.bases = {}            # C++ class name -> its first base
        s.sigs = {}             # (C++ class, function) -> (return type, [(type, name)]) parsed lazily from headers
        s._text = {}
        for f in sorted(os.listdir(ueapi)):
            if f.endswith('.h'): s._scan(f)
        game = os.path.join(ueapi, 'Game')
        if os.path.isdir(game):
            for f in sorted(os.listdir(game)):
                if f.endswith('.h'): s._scan('Game/' + f)
        types = json.load(open(os.path.join(ueapi, 'Types.json'), encoding='utf-8'))
        s.structs = {(v['package'], v['name']): k for k, v in types['structs'].items()}
        s.fields = {v['name']: dict((n, ty) for ty, n in v.get('fields', [])) for v in types['structs'].values()}
        # an engine struct whose UeApi stub has a constructor over every member (its fields in order, the base's first):
        # no aggregate, so its value is that constructor call, not designated braces
        s.ctor_fields = {v['name']: [n for _, n in v['fields']] for v in types['structs'].values()
                         if v.get('complete') and v.get('fields')}
        s.struct_cpp = {v['name']: k for k, v in types['structs'].items()}
        s.enums = {(v['package'], v['name']): (k, v.get('form')) for k, v in types['enums'].items()}
        s.ops = {}
        for o in json.load(open(os.path.join(ueapi, 'Ops.json'), encoding='utf-8')):
            s.ops.setdefault(o['fn'], o)
        s.convs = {c['fn']: c for c in json.load(open(os.path.join(ueapi, 'Conv.json'), encoding='utf-8'))}

    CLASS_RE = re.compile(r'^class (\w+)\b[^;{\n]*\n?\{\s*public:\s*UE_CLASS\("([^"]+)", "([^"]+)"\)', re.M)
    ALIAS_RE = re.compile(r'^using (\w+) = ((?:::)?(?:\w+::)+\w+);', re.M)
    BASE_RE = re.compile(r'^class (\w+) : public (?:::)?(?:\w+::)*(\w+)', re.M)   # a Blueprint base is qualified
    NS_RE = re.compile(r'^namespace Game \{(.*?)\{\s*$', re.M)

    def _scan(s, rel):
        text = open(os.path.join(s.dir, rel), encoding='utf-8-sig', errors='replace').read()
        s._text[rel] = text
        aliases = {full.split('::')[-1]: short for short, full in s.ALIAS_RE.findall(text)}
        for cpp, base in s.BASE_RE.findall(text): s.bases.setdefault(cpp, base)
        for cpp, package, name in s.CLASS_RE.findall(text):
            if package.startswith('/Game/'):
                ns = '::Game::' + '::'.join(package.split('/')[2:-1])   # rooted: Game::Game would hide a bare Game::
                spelled = cpp if aliases.get(cpp) == cpp else ns + '::' + cpp
                s.classes[(package, name)] = spelled
            else:
                s.classes[(package, name)] = cpp
            s.headers.setdefault(cpp, rel)

    MEMBER_RE = re.compile(r'^\s+(?:class\s+|struct\s+|enum\s+class\s+)?[\w:<>,* ]+?[\s*&](\w+)(?:\[\d+\])?;', re.M)
    MARK_RE = re.compile(r'(\w+)__UeName = "([^"]*)"')

    def members(s, cpp):
        """Class cpp's data members as its UeApi header declares them: the engine's name, lowercased -> the C++ one."""
        cache = s.__dict__.setdefault('_members', {})
        if cpp in cache: return cache[cpp]
        out = {}
        rel = s.headers.get(cpp)
        text = s._text.get(rel, '') if rel else ''
        m = re.search(r'\nclass %s\b[^{;]*\{(.*?)\n\};' % re.escape(cpp), text, re.S)
        if m:
            body = m.group(1)
            for name in s.MEMBER_RE.findall(body): out.setdefault(name.lower(), name)
            for c, real in s.MARK_RE.findall(body): out[real.lower()] = c
        cache[cpp] = out
        return out

    BNDEVT_RE = re.compile(r'\bBndEvt__\w*?_K2Node_ComponentBoundEvent_\d+_(\w+?__DelegateSignature)\(([^)]*)\)')

    def delegate_sig(s, name):
        """`void(float Health)` of a native delegate signature function (HealthChangedSig__DelegateSignature), which no
        UeApi declaration names, read off a game Blueprint's bound event of it, which has its parameters."""
        if not hasattr(s, '_sigs'):
            s._sigs = {}
            for text in s._text.values():
                for sig, params in s.BNDEVT_RE.findall(text): s._sigs.setdefault(sig, 'void(%s)' % params.strip())
        return s._sigs.get(name)

    def member_type(s, cpp, name):
        """The C++ type class cpp's UeApi header gives data member `name` (the engine's name, any case), or None."""
        rel = s.headers.get(cpp)
        text = s._text.get(rel, '') if rel else ''
        m = re.search(r'\nclass %s\b[^{;]*\{(.*?)\n\};' % re.escape(cpp), text, re.S)
        if not m: return None
        real = s.members(cpp).get(name.lower())
        if not real: return None
        t = re.search(r'^\s+(?:UE_\w+\s+)*((?:class\s+)?[\w:<>,* ]+?)\s*\b%s(?:\[\d+\])?;' % re.escape(real), m.group(1), re.M)
        return t.group(1).replace('class ', '').strip() if t else None

    def header_of(s, cpp):
        return s.headers.get(cpp.split('::')[-1])


# ---- the mod model: every asset of the mod roots, read off its cooked package

class Asset:
    """One package of a mod: its kind (enum / struct / interface / class / data asset / raw), its C++ name and
    namespace, and the Package it was read from."""
    def __init__(s, pkg, path, kind, export):
        s.pkg, s.path, s.kind, s.export = pkg, path, kind, export
        s.name = path.rsplit('/', 1)[-1]
        s.cpp = ident(s.name)
        s.mod = None            # (root, ModName)
        s.ns = []               # the namespace path under the mod root, one entry per folder
        s.deps = set()          # paths of mod assets this one's declaration needs complete (by-value members, bases)
        s.notes = []            # what was not decompiled


class Mod:
    def __init__(s, root, name):
        s.root, s.name, s.assets = root, name, []


def classify(pkg):
    """(kind, export index) of a package's main object: enum, struct, interface, class, widget, data (a data asset
    instance of some class), or raw (textures, materials, fonts, sounds - kept as cooked)."""
    for i, e in enumerate(pkg.exports):
        if e['outer']: continue
        c = pkg.class_of(i + 1)
        if c == 'UserDefinedEnum': return 'enum', i
        if c == 'UserDefinedStruct': return 'struct', i
        if c in ('BlueprintGeneratedClass', 'WidgetBlueprintGeneratedClass'):
            st = pkg.struct(i)
            if st.class_flags & CLASS_Interface: return 'interface', i
            return 'widget' if c == 'WidgetBlueprintGeneratedClass' else 'class', i
    for i, e in enumerate(pkg.exports):
        if e['outer'] or e['name'].startswith('Default__'): continue
        if e['flags'] & 0x1 and pkg.class_of(i + 1).endswith('_C'): return 'data', i   # RF_Public, a BP class's object
    return 'raw', None


class Model:
    def __init__(s, content, mods, names_dir=None, api=None):
        s.content = content.replace('\\', '/').rstrip('/')
        s.api = api or Api()
        # A name given twice is one mod over two roots: the first is its UE_MOD_PACKAGE, and an asset under another
        # sits in a `Game::...` namespace, which AssetGen cooks at that /Game path whatever the mod's package.
        s.mods, roots = [], []
        for r, n in mods:
            m = next((x for x in s.mods if x.name == n), None)
            if m is None: m = Mod(r.rstrip('/'), n); s.mods.append(m)
            roots.append((r.rstrip('/'), m))
        s.assets = OrderedDict()     # package path -> Asset
        s.names_dir = names_dir
        for root, mod in roots:
            folder = s.content + root[len('/Game'):]
            for dp, dn, fn in os.walk(folder):
                dn.sort()
                for f in sorted(fn):
                    if not f.endswith('.uasset'): continue
                    base = os.path.join(dp, f)[:-7].replace('\\', '/')
                    path = '/Game' + base[len(s.content):]
                    pkg = invariants.load(base)
                    kind, i = classify(pkg)
                    a = Asset(pkg, path, kind, i)
                    a.mod = mod
                    a.ns = [ident(p) for p in path[len(mod.root) + 1:].split('/')[:-1]] if root == mod.root else                         ['Game'] + [ident(p) for p in path[len('/Game/'):].split('/')[:-1]]
                    mod.assets.append(a)
                    s.assets[path] = a

    def asset_of_object(s, objpath):
        """The mod asset an object path lies in (/Game/A/B.B_C -> /Game/A/B), or None."""
        return s.assets.get(objpath.split('.', 1)[0])

    def var_names(s):
        """Every name a mod function gives a parameter or local, as C++ spells it."""
        if not hasattr(s, '_var_names'):
            s._var_names = set()
            for a in s.assets.values():
                for i, _e in enumerate(a.pkg.exports):
                    if a.pkg.class_of(i + 1) == 'Function':
                        st = a.pkg.struct(i)
                        if st: s._var_names.update(ident(p.name) for p in st.props)
        return s._var_names

    def ancestors(s, name):
        """The C++ class names (last component) class `name` derives from, itself first."""
        by = {a.cpp: a for a in s.assets.values() if a.kind in ('class', 'widget') and getattr(a, 'cpp', None)}
        out = []
        while name and name not in out:
            out.append(name)
            a = by.get(name)
            if a is None: name = s.api.bases.get(name); continue
            st = a.pkg.struct(a.export)
            sup = a.pkg.path(st.super) if st is not None and st.super else ''
            sa = s.asset_of_object(sup)
            top, _, n = sup.partition('.')
            name = sa.cpp if sa else (s.api.classes.get((top, n)) or '').split('::')[-1]
        return out

    def display_names(s, a):
        """name -> display name of an enum's entries, off its DisplayNameMap: the cooked package's (cooking keeps it),
        else the uncooked asset's under --names."""
        out = s._display_map(a.pkg)
        if out or not s.names_dir: return out
        base = s.names_dir.rstrip('/\\') + a.path[len('/Game'):]
        if not os.path.exists(base + '.uasset'): return {}
        return s._display_map(invariants.load(base))

    @staticmethod
    def _display_map(pkg):
        out = {}
        for i, e in enumerate(pkg.exports):
            if pkg.class_of(i + 1) != 'UserDefinedEnum': continue
            t = pkg.tag(i, 'DisplayNameMap')
            if not t: continue
            v, o = t['value'], 4
            n = struct.unpack_from('<i', v, o)[0]; o += 4
            for _ in range(n):
                k = pkg.names[struct.unpack_from('<i', v, o)[0]]; o += 8
                text, o = read_ftext(v, o)
                out[k] = text
        return out


def read_fstring(b, o):
    n = struct.unpack_from('<i', b, o)[0]; o += 4
    if n == 0: return '', o
    if n > 0: return b[o:o + n - 1].decode('latin-1'), o + n
    return b[o:o - 2 * n - 2].decode('utf-16-le'), o - 2 * n


def read_ftext(b, o):
    """An FText's source string (Text.cpp's FText serialization, 4.27): none, base (namespace, key, source) or
    culture-invariant."""
    o += 4
    h = struct.unpack_from('<b', b, o)[0]; o += 1
    if h == -1:
        has = struct.unpack_from('<i', b, o)[0]; o += 4
        if has: return read_fstring(b, o)
        return '', o
    if h == 0:
        _, o = read_fstring(b, o); _, o = read_fstring(b, o)
        return read_fstring(b, o)
    raise ValueError('FText history %d' % h)


# ---- types: an FProperty as the C++ type AssetGen reads back into the same property

class Types:
    def __init__(s, model):
        s.m, s.api = model, model.api
        s.missing = set()       # names no table spells, for the report

    def objpath(s, pkg, idx):
        p = pkg.path(idx)
        return p

    def spell_mod(s, target, cur):
        """A mod asset's C++ name as seen from asset cur: bare inside the same namespace, else fully qualified. Also
        qualified when some function names a parameter or local so (`EEE_Manager*& EEE_Manager`): there the bare
        name is the variable."""
        if cur is not None and target.ns == cur.ns and target.cpp not in s.m.var_names(): return target.cpp
        return '::' + '::'.join(target.ns + [target.cpp]) if target.ns else target.cpp

    def cls(s, pkg, idx, cur):
        """The C++ name of the class an FPackageIndex names."""
        if not idx: return 'UObject'
        p = s.objpath(pkg, idx)
        top, _, name = p.partition('.')
        a = s.m.asset_of_object(p)
        if a is not None: return s.spell_mod(a, cur)
        if (top, name) in s.api.classes: return s.api.classes[(top, name)]
        if name.endswith('_C') and (top, name) not in s.api.classes:
            s.missing.add('class ' + p)
            return 'UObject /* %s */' % p
        s.missing.add('class ' + p)
        return 'U' + name

    def struct(s, pkg, idx, cur):
        p = s.objpath(pkg, idx)
        top, _, name = p.partition('.')
        a = s.m.asset_of_object(p)
        if a is not None: return s.spell_mod(a, cur)
        if (top, name) in s.api.structs: return s.api.structs[(top, name)]
        s.missing.add('struct ' + p)
        return 'F' + name

    def enum(s, pkg, idx, cur, as_byte=True):
        p = s.objpath(pkg, idx)
        top, _, name = p.partition('.')
        a = s.m.asset_of_object(p)
        if a is not None: return s.spell_mod(a, cur)
        if (top, name) in s.api.enums:
            cpp, form = s.api.enums[(top, name)]
            return 'TEnum<%s>' % cpp if as_byte and form != 'EnumClass' else cpp
        s.missing.add('enum ' + p)
        return 'uint8'

    SIMPLE = {'IntProperty': 'int32', 'Int64Property': 'int64', 'FloatProperty': 'float', 'DoubleProperty': 'double',
              'BoolProperty': 'bool', 'StrProperty': 'FString', 'NameProperty': 'FName', 'TextProperty': 'FText',
              'UInt16Property': 'uint16', 'UInt32Property': 'uint32', 'UInt64Property': 'uint64',
              'Int16Property': 'int16', 'Int8Property': 'int8'}

    def of(s, pkg, p, cur):
        t = p.type
        if t in s.SIMPLE: return s.SIMPLE[t]
        if t == 'ByteProperty': return s.enum(pkg, p.ref, cur) if p.ref else 'uint8'
        if t == 'EnumProperty': return s.enum(pkg, p.enum, cur, as_byte=False)
        if t == 'ObjectProperty': return s.cls(pkg, p.ref, cur) + '*'
        if t == 'WeakObjectProperty': return s.cls(pkg, p.ref, cur) + '* /* weak */'
        if t == 'ClassProperty':
            meta = s.cls(pkg, p.meta, cur)
            return 'UClass*' if meta == 'UObject' else 'TSubclassOf<%s>' % meta
        if t == 'SoftObjectProperty': return 'TSoftObjectPtr<%s>' % s.cls(pkg, p.ref, cur)
        if t == 'SoftClassProperty':        # UeApi spells the any-class soft reference TSoftClassPtr<UClass>
            meta = s.cls(pkg, p.meta, cur)
            return 'TSoftClassPtr<%s>' % ('UClass' if meta == 'UObject' else meta)
        if t == 'InterfaceProperty': return 'TScriptInterface<%s>' % s.cls(pkg, p.ref, cur)
        if t == 'StructProperty': return s.struct(pkg, p.ref, cur)
        if t == 'ArrayProperty': return 'TArray<%s>' % s.of(pkg, p.subs[0], cur)
        if t == 'SetProperty': return 'TSet<%s>' % s.of(pkg, p.subs[0], cur)
        if t == 'MapProperty': return 'TMap<%s, %s>' % (s.of(pkg, p.subs[0], cur), s.of(pkg, p.subs[1], cur))
        if t == 'DelegateProperty': return 'TDelegate<%s>' % s.signature(pkg, p.ref, cur)
        if t in ('MulticastInlineDelegateProperty', 'MulticastDelegateProperty'):
            return 'TMulticastInlineDelegate<%s>' % s.signature(pkg, p.ref, cur)
        s.missing.add('property type ' + t)
        return 'int32 /* %s */' % t

    def params(s, pkg, fidx, cur):
        """[(C++ type with its reference, name, prop)] and the return type of function export fidx (a 0-based export
        index of pkg) or an import of a mod function."""
        at = pkg.resolve(fidx) if fidx < 0 else (pkg, fidx - 1)
        if at is None: return None
        fp, k = at
        st = fp.struct(k)
        ret, out = 'void', []
        for p in st.props:
            if not p.flags & CPF_Parm: continue
            ty = s.of(fp, p, cur)
            if p.flags & CPF_ReturnParm: ret = ty; continue
            if p.flags & CPF_OutParm and not p.flags & CPF_ConstParm: ty += '&'
            elif p.flags & CPF_ReferenceParm or (p.flags & CPF_ConstParm and p.flags & CPF_OutParm): ty = 'const %s&' % ty
            out.append((ty, p.name, p))
        return ret, out

    def signature(s, pkg, idx, cur):
        """`void(int32 Points, AActor* By)` of a delegate's signature function."""
        r = s.params(pkg, idx, cur) if idx else None
        if r is None:
            return (idx and s.api.delegate_sig(pkg.obj(idx)['name'])) or 'void()'
        ret, ps = r
        return '%s(%s)' % (ret, ', '.join('%s %s' % (t, ident(n)) for t, n, _ in ps))
