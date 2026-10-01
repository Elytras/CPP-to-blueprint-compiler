import sys
from invariants import *
# TYPES: sweep rules for UserDefinedStruct and UserDefinedEnum exports and for the values of their types. Each rule
# states what UE 4.27 relies on and cites the engine source that makes it one. Calibrated on every package of the game's
# own cooked content (its FSD/Content): none of them fires there.
import glob, os, re, struct

CPF_Parm, CPF_Net, CPF_RepNotify = 0x80, 0x20, 0x100000000
FUNC_UbergraphFunction, FUNC_HasDefaults = 0x8000, 0x800000
STRUCT_Immutable = 0x20
EDITOR_MEMBER = re.compile(r'(.+)_([0-9]+)_[0-9A-F]{32}$')      # FMemberVariableNameHelper::Generate's form


# ---- UserDefinedStruct members and values

def authored_name(fname):
    """The name a cooked game reads a UserDefinedStruct member by when no editor data is loaded:
    UUserDefinedStruct::GetAuthoredNameForField (UserDefinedStruct.cpp 217-247). A name longer than 35 characters loses
    its last 33, then everything from the last '_' found past index 0; anything else is the FName itself."""
    if len(fname) > 35:
        cut = fname[:-33]
        k = cut.rfind('_')
        if k > 0: return cut[:k]
    return fname


def uds_member(st, name):
    """The member a serialized tag name reaches in a UserDefinedStruct: FindPropertyByName (an FName compare, which
    ignores case), else CustomFindProperty's first member whose authored name equals it (FString ==, which ignores case)
    (Class.cpp 1182-1186, 1403-1405; UserDefinedStruct.cpp 192-215). None if neither does."""
    low = name.lower()
    return next((q for q in st.props if q.name.lower() == low), None) \
        or next((q for q in st.props if authored_name(q.name).lower() == low), None)


def uds_exports(pkg):
    for i in range(len(pkg.exports)):
        if pkg.class_of(i + 1) == 'UserDefinedStruct': yield i, pkg.struct(i)


def uds_at(pkg, idx):
    """(Package, export index) of the UserDefinedStruct an FPackageIndex names, else None (a native struct, or a
    package not found)."""
    if not idx: return None
    try: r = pkg.resolve(idx)
    except Exception: return None
    return r if r and r[0].class_of(r[1] + 1) == 'UserDefinedStruct' else None


def uds_struct_guid(upkg, k):
    """The Guid tag of a UserDefinedStruct export: what GetCustomGuid returns (UserDefinedStruct.cpp 345-348)."""
    t = upkg.tag(k, 'Guid')
    return bytes(t['value']) if t and t['size'] == 16 else None


@rule
def uds_members(pkg):
    """A UserDefinedStruct is not STRUCT_Immutable: the flag is not in STRUCT_ComputedFlags, so loading keeps it
    (Class.h 627, 681, 1631), and UseBinarySerialization then reads the Data stream and every value of the
    struct as raw memory instead of tags (Class.cpp 2706-2711). Each member reads back as the name it was authored
    under: GetAuthoredNameForField's cut (UserDefinedStruct.cpp 217-247) gives an editor-form name
    <Name>_<N>_<32 hex> back as <Name>, and any other name back as itself, and no two members share an authored name,
    case aside (CustomFindProperty takes the first match, UserDefinedStruct.cpp 192-215). No member is CPF_Net /
    CPF_RepNotify or names a RepNotify function or condition: a struct replicates as a whole and RepNotify is looked
    up on class-level properties only (RepLayout.cpp 5321-5384, 5956-5959; the editor writes None,
    UserDefinedStructureCompilerUtils.cpp 278)."""
    for i, st in uds_exports(pkg):
        if st.struct_flags & STRUCT_Immutable: yield i, 'StructFlags %#x has STRUCT_Immutable' % st.struct_flags
        seen = {}
        for q in st.props:
            m = EDITOR_MEMBER.match(q.name)
            want, got = (m.group(1) if m else q.name), authored_name(q.name)
            if got != want: yield i, 'member %s reads back as %r, not %r' % (q.name, got, want)
            if got.lower() in seen: yield i, 'members %s and %s both read back as %r' % (seen[got.lower()], q.name, got)
            seen.setdefault(got.lower(), q.name)
            if q.flags & (CPF_Net | CPF_RepNotify) or q.notify != 'None' or q.cond:
                yield i, 'member %s: flags %#x, RepNotify %s, condition %d' % (q.name, q.flags, q.notify, q.cond)


def uds_nonzero(upkg, k, depth=0):
    """True when the struct's default instance is not all zero bytes, or a member's type is not zero-constructible
    (FText, a soft or lazy pointer, a struct member of such a struct): UUserDefinedStruct::UpdateStructFlags then
    drops STRUCT_ZeroConstructor (UserDefinedStruct.cpp 442-523), and a property of the struct is no
    CPF_ZeroConstructor (PropertyStruct.cpp 114-117). False when that is certain not to happen, None when a value
    cannot be judged here (an enum value of a game enum)."""
    st = upkg.struct(k)
    unknown = False
    for q in st.props:
        if q.type in ('TextProperty', 'SoftObjectProperty', 'SoftClassProperty', 'LazyObjectProperty'): return True
        u = uds_at(upkg, q.ref) if q.type == 'StructProperty' and depth < 8 else None
        if u:
            z = uds_nonzero(*u, depth=depth + 1)
            if z: return True
            unknown |= z is None
    for t in getattr(st, 'defaults', []):
        q = uds_member(st, t['name'])
        z = value_nonzero(upkg, k, t, upkg, q, depth)
        if z: return True
        unknown |= z is None
    return None if unknown else False


def value_nonzero(pkg, i, t, qp, q, depth=0):
    """Whether tag t (of export i) holds a value that is not all zero bytes in memory; None when it cannot be told."""
    ty, v = t['type'], bytes(t['value'])
    if ty == 'BoolProperty': return bool(t['bool'])
    if ty in ('IntProperty', 'Int64Property', 'UInt32Property', 'UInt64Property', 'UInt16Property', 'Int16Property',
              'Int8Property', 'FloatProperty', 'DoubleProperty', 'ObjectProperty', 'ClassProperty',
              'InterfaceProperty', 'WeakObjectProperty'):
        return any(v)
    if ty == 'NameProperty': return pkg.names[struct.unpack_from('<i', v)[0]] != 'None'
    if ty == 'StrProperty': return struct.unpack_from('<i', v)[0] != 0
    if ty in ('ArrayProperty', 'SetProperty'): return struct.unpack_from('<i', v, 0 if ty == 'ArrayProperty' else 4)[0] != 0
    if ty == 'MapProperty': return struct.unpack_from('<i', v, 4)[0] != 0
    if ty in ('ByteProperty', 'EnumProperty'):
        if t['enum'] == 'None': return any(v)
        body = enum_of(pkg, t['enum'], qp, q)
        if body is None: return None
        name = pkg.names[struct.unpack_from('<i', v)[0]]
        hit = enum_resolve(t['enum'], body, name)
        return None if hit is None else hit[1] != 0
    if ty == 'StructProperty':
        u = uds_at(qp, q.ref) if q is not None and q.type == 'StructProperty' else None
        if u:                   # the nested value is loaded over the nested struct's own default instance
            if uds_nonzero(*u, depth=depth + 1): return True
            st = u[0].struct(u[1])
            unknown = False
            for n in pkg.tags(i, t['at']):
                z = value_nonzero(pkg, i, n, u[0], uds_member(st, n['name']), depth + 1)
                if z: return True
                unknown |= z is None
            return None if unknown else False
        if t['struct'] in ('Vector', 'Vector2D', 'Vector4', 'Rotator', 'Quat', 'LinearColor', 'Color', 'IntPoint',
                           'IntVector', 'Guid'):
            return any(v)
        return None
    return None


@rule
def uds_local_defaults(pkg):
    """A function (not the ubergraph, whose persistent frame is initialized whole) with a local of a UserDefinedStruct
    whose default instance is not all zero carries FUNC_HasDefaults. The frame is Memzero'd (ScriptCore.cpp 825-826,
    1952-1954) and InitializeValue runs only from Function->FirstPropertyToInit on (ScriptCore.cpp 910-915,
    2005-2010), which UFunction::Link sets only under FUNC_HasDefaults (Class.cpp 5652-5660); the editor sets both
    for any local that is not CPF_ZeroConstructor (KismetCompiler.cpp 2329-2335). Without the flag the local starts
    as zeros, not as the struct's defaults (UUserDefinedStruct::InitializeStruct, UserDefinedStruct.cpp 254-276)."""
    for i, st in functions(pkg):
        if st.function_flags & (FUNC_UbergraphFunction | FUNC_HasDefaults): continue
        for q in st.props:
            if q.flags & CPF_Parm or q.type != 'StructProperty': continue
            u = uds_at(pkg, q.ref)
            if u and uds_nonzero(*u):
                yield i, 'local %s is a %s with non-zero defaults, and FunctionFlags %#x lack FUNC_HasDefaults' % (
                    q.name, u[0].exports[u[1]]['name'], st.function_flags)
                break


# ---- walking tagged values with the properties they belong to

SCALAR_SIZE = {'IntProperty': 4, 'UInt32Property': 4, 'FloatProperty': 4, 'Int64Property': 8, 'UInt64Property': 8,
               'DoubleProperty': 8, 'NameProperty': 8, 'ObjectProperty': 4, 'ClassProperty': 4, 'BoolProperty': 1,
               'Int16Property': 2, 'UInt16Property': 2, 'Int8Property': 1, 'EnumProperty': 8, 'InterfaceProperty': 4,
               'WeakObjectProperty': 4}


class Owner:
    """The properties a tag list belongs to: a Blueprint class and its Blueprint parents, or a UserDefinedStruct."""
    def __init__(s, props, uds=None): s.props, s.uds = props, uds      # props: lower name -> (Package, Prop)
    def find(s, name):
        if s.uds:
            q = uds_member(s.uds[0].struct(s.uds[1]), name)
            return (s.uds[0], q) if q else None
        return s.props.get(name.lower())


def class_owner(pkg, cidx):
    """Owner of an object of the class an FPackageIndex names, when it is a Blueprint class: its properties and its
    Blueprint parents' (a native parent's are not known here). None for a native class."""
    props, first = {}, None
    for _ in range(32):
        try: r = pkg.resolve(cidx) if cidx else None
        except Exception: r = None
        if not r: break
        cp, k = r
        st = cp.struct(k)
        if not st or not hasattr(st, 'class_flags'): break
        first = first or st
        for q in st.props: props.setdefault(q.name.lower(), (cp, q))
        pkg, cidx = cp, st.super
    return Owner(props) if first else None


def uds_owner(u): return Owner(None, u)


def owned_lists(pkg):
    """(export index, start, Owner, what) of every tag list whose properties are known: an object of a Blueprint
    class (its CDO, templates, instances), and a UserDefinedStruct's Data stream."""
    for i, e in enumerate(pkg.exports):
        c = pkg.class_of(i + 1)
        if c == 'UserDefinedStruct':
            st = pkg.struct(i)
            if hasattr(st, 'defaults'):
                start = next(k[1] for k, v in pkg._tags.items() if v is st.defaults)
                yield i, start, uds_owner((pkg, i)), 'Data'
            continue
        if pkg.is_struct(i) or c in ('UserDefinedEnum', 'Enum') or e['cls'] == 0: continue
        o = class_owner(pkg, e['cls'])
        if o: yield i, 0, o, e['name']


def walk(pkg, i, start, owner, path, depth=0):
    """Every value in the tag list at `start` of export i and under it, as (kind, path, tag, Package, Prop, at, size,
    enum name): kind 'tag' for a tag, 'inner' for an array's inner struct tag, 'elem' for a set / map / array element
    of an enum, 'name' for a nested tag name that no member takes. Prop is None where the property is not known."""
    tl = pkg.tags(i, start)
    for t in tl:
        hit = owner.find(t['name']) if owner else None
        p, q = hit if hit else (None, None)
        where = path + '.' + t['name']
        if owner and owner.uds and not hit: yield 'name', where, t, None, None, t['at'], t['size'], None
        yield 'tag', where, t, p, q, t['at'], t['size'], t.get('enum')
        if depth < 12: yield from descend(pkg, i, t, p, q, where, depth)


def descend(pkg, i, t, p, q, where, depth):
    ty, at, end = t['type'], t['at'], t['at'] + t['size']
    if ty == 'StructProperty':
        u = uds_at(p, q.ref) if q is not None and q.type == 'StructProperty' else None
        if u and struct_accepts(t, u): yield from walk(pkg, i, at, uds_owner(u), where, depth + 1)
        return
    if ty not in ('ArrayProperty', 'SetProperty', 'MapProperty'): return
    b = pkg.blob(i)
    subs = q.subs if q is not None and q.type == ty else []
    types = [t['inner']] + ([t['value_type']] if ty == 'MapProperty' else [])
    o = at
    try:
        if ty != 'ArrayProperty':
            removed = struct.unpack_from('<i', b, o)[0]; o += 4
            if removed: return                              # keys to remove: not in a cooked default, not walked
        count = struct.unpack_from('<i', b, o)[0]; o += 4
        inner_tag = None
        if ty == 'ArrayProperty' and t['inner'] == 'StructProperty':
            r = Rd(b, o)
            inner_tag = dict(name=pkg._name(r), type=pkg._name(r))
            inner_tag['size'], inner_tag['index'] = r.i32(), r.i32()
            inner_tag['struct'] = pkg._name(r); inner_tag['struct_guid'] = b[r.o:r.o + 16]; r.o += 16
            if r.u8(): r.o += 16
            o = r.o
            sp = subs[0] if subs else None
            yield 'inner', where + '[]', inner_tag, p, sp, o, inner_tag['size'], None
            u = uds_at(p, sp.ref) if sp is not None and sp.type == 'StructProperty' else None
            if not (u and struct_accepts(inner_tag, u)): return
            for k in range(count):
                yield from walk(pkg, i, o, uds_owner(u), '%s[%d]' % (where, k), depth + 1)
                o = pkg.tags(i, o).end
            return
        for k in range(count):
            for j, ety in enumerate(types):
                sp = subs[j] if j < len(subs) else None
                o = yield from element(pkg, i, b, o, ety, p, sp, '%s[%d]%s' % (where, k, '.key' if ty == 'MapProperty' and j == 0 else '.value' if ty == 'MapProperty' else ''), depth)
                if o is None or o > end: return
    except (struct.error, IndexError):
        return


def element(pkg, i, b, o, ety, p, sp, where, depth):
    """One container element at o: yields what walk yields for it, returns where it ends (None if not known)."""
    if ety == 'StructProperty':
        u = uds_at(p, sp.ref) if sp is not None and sp.type == 'StructProperty' else None
        if not u: return None
        yield from walk(pkg, i, o, uds_owner(u), where, depth + 1)
        return pkg.tags(i, o).end
    if ety == 'ByteProperty':
        enum = enum_prop_name(p, sp)
        if enum is None: return None                        # 1 byte or an 8-byte name: only the property knows
        if enum == 'None': return o + 1
        yield 'elem', where, None, p, sp, o, 8, enum
        return o + 8
    if ety == 'EnumProperty':
        enum = enum_prop_name(p, sp)
        yield 'elem', where, None, p, sp, o, 8, enum
        return o + 8
    if ety == 'StrProperty': return fstring_end(b, o)
    return o + SCALAR_SIZE[ety] if ety in SCALAR_SIZE else None


def struct_accepts(t, u):
    """Whether a StructProperty tag's value is read into UserDefinedStruct u: the tag's StructName is u's name, or its
    StructGuid is valid and equal to u's Guid (FStructProperty::ConvertFromType, PropertyStruct.cpp 354-362, 392-413;
    for an array's inner tag, PropertyArray.cpp 207-240)."""
    name = u[0].exports[u[1]]['name']
    guid = bytes(t['struct_guid'])
    return t['struct'].lower() == name.lower() or (any(guid) and guid == uds_struct_guid(*u))


@rule
def uds_tags(pkg):
    """Every tagged value of a UserDefinedStruct type is read: its tag (an array's inner tag too) names the struct, or
    carries its Guid - else the value is skipped whole and the member keeps its default (PropertyStruct.cpp 354-362,
    392-413; PropertyArray.cpp 207-240; the editor writes both, PropertyTag.cpp 25-29). Inside the value, every tag
    name reaches a member, by FName or by authored name (Class.cpp 1182-1186, 1403-1405; UserDefinedStruct.cpp
    192-247) - a name no member takes is skipped with its value."""
    for i, start, owner, what in owned_lists(pkg):
        for kind, where, t, p, q, at, size, enum in walk(pkg, i, start, owner, what):
            if kind == 'name':
                yield i, '%s: no member of the struct is named %s, so its value is dropped' % (where, t['name'])
            elif kind in ('tag', 'inner') and t['type'] == 'StructProperty' and q is not None and q.type == 'StructProperty':
                u = uds_at(p, q.ref)
                if u and not struct_accepts(t, u):
                    yield i, '%s: tagged struct %s guid %s, the property is %s guid %s' % (
                        where, t['struct'], bytes(t['struct_guid']).hex(), u[0].exports[u[1]]['name'],
                        (uds_struct_guid(*u) or b'').hex())


# ---- UserDefinedEnum and enum values

def enum_body(pkg, i):
    """(entries [(FName, value)], CppForm, where the read ended) of a UEnum / UUserDefinedEnum export, as
    UEnum::Serialize writes it after the UObject part (Enum.cpp 33-95): int32 count, count x (FName, int64), uint8
    CppForm. UField writes no Next, UUserDefinedEnum nothing more (Class.cpp 218-226; UserDefinedEnum.cpp 17-38)."""
    b = pkg.blob(i)
    r = Rd(b, pkg.tags(i).end)
    if r.i32(): r.o += 16                                   # FLazyObjectPtr::PossiblySerializeObjectGuid
    entries = [(pkg._name(r), r.i64()) for _ in range(r.i32())]
    return entries, r.u8(), r.o


_TYPES_NATIVE_ENUMS = {}
TYPES_UEAPI_DIRS = [UEAPI_DIR]


def native_enums():
    """name -> short enumerator names (the closing _MAX included) of every native enum of the game and the engine:
    the enums Types.json lists, their enumerators as the generated UeApi headers declare them. {} when no generated
    UeApi is found."""
    if not _TYPES_NATIVE_ENUMS:
        import json
        d = next((d for d in TYPES_UEAPI_DIRS if d and os.path.exists(os.path.join(d, 'Engine.h'))), None)
        known = set(json.load(open(os.path.join(d, 'Types.json'), encoding='utf-8'))['enums']) if d else set()
        for h in sorted(glob.glob(os.path.join(d, '*.h'))) if d else []:
            text = open(h, encoding='utf-8', errors='replace').read()
            for m in re.finditer(r'^enum\s+(?:class\s+)?(\w+)\s*(?::\s*\w+)?\s*\{([^}]*)\}', text, re.M):
                if m.group(1) in known:
                    _TYPES_NATIVE_ENUMS.setdefault(m.group(1), re.findall(r'^\s*(\w+)\s*(?:=|,|$)', m.group(2), re.M))
        _TYPES_NATIVE_ENUMS.setdefault('', [])
    return _TYPES_NATIVE_ENUMS


_BODIES = {}


def body_at(p, k):
    """('ude', entries, CppForm, object name) of the UserDefinedEnum export k of package p, read once."""
    key = (p.base, k)
    if key not in _BODIES:
        entries, form, _ = enum_body(p, k)
        _BODIES[key] = ('ude', entries, form, p.exports[k]['name'])
    return _BODIES[key]


def enum_of(pkg, name, p=None, q=None):
    """The entries of the enum a value belongs to: through its property's enum reference when the property is known
    (a struct member in another package names its enum there), else by the FName its tag carries, among this
    package's objects, then among the game's and engine's native enums. ('ude', entries, CppForm, name) for a
    UserDefinedEnum found (Package.resolve), ('native', short names) for an enum the UeApi headers declare, None when
    neither is found."""
    idx = (q.ref if q.type == 'ByteProperty' else q.enum if q.type == 'EnumProperty' else 0) if q is not None else 0
    if idx:
        try: r = p.resolve(idx)
        except Exception: r = None
        if r and r[0].class_of(r[1] + 1) in ('UserDefinedEnum', 'Enum'): return body_at(*r)
        o = p.obj(idx)
        return ('native', native_enums()[o['name']]) if idx < 0 and o['class_name'] == 'Enum' and o['name'] in native_enums() else None
    for k, e in enumerate(pkg.exports):
        if e['name'] == name and pkg.class_of(k + 1) in ('UserDefinedEnum', 'Enum'): return body_at(pkg, k)
    for k, imp in enumerate(pkg.imports):
        if imp['name'] == name and imp['class_name'] in ('UserDefinedEnum', 'Enum'):
            try: r = pkg.resolve(-k - 1)
            except Exception: r = None
            if r: return body_at(*r)
    return ('native', native_enums()[name]) if name in native_enums() else None


def fstring_end(b, o):
    n = struct.unpack_from('<i', b, o)[0]
    return o + 4 + (n if n >= 0 else -2 * n)


def text_end(b, o):
    """Past one FText of a cooked stream (Text.cpp operator<<): uint32 flags, int8 history type, then for None (-1)
    a bool and the culture-invariant string it announces, for Base (0) namespace, key and source string. None for any
    other history."""
    h = struct.unpack_from('<b', b, o + 4)[0]; o += 5
    if h == -1: return fstring_end(b, o + 4) if struct.unpack_from('<i', b, o)[0] else o + 4
    return fstring_end(b, fstring_end(b, fstring_end(b, o))) if h == 0 else None


def display_keys(pkg, t):
    """The keys of a DisplayNameMap tag (TMap<FName, FText>): keys to remove, count, then (FName, FText) pairs, as
    far as the texts can be walked."""
    v, keys = bytes(t['value']), []
    try:
        o = 8 + 8 * struct.unpack_from('<i', v, 0)[0]
        for _ in range(struct.unpack_from('<i', v, o - 4)[0]):
            keys.append(pkg.names[struct.unpack_from('<i', v, o)[0]])
            o = text_end(v, o + 8)
            if o is None: break
    except (struct.error, IndexError): pass
    return keys


def enum_prop_name(p, q):
    """The FName of the enum a Byte / Enum property holds values of: 'None' for a plain byte, None if not known."""
    if q is None: return None
    idx = q.ref if q.type == 'ByteProperty' else q.enum if q.type == 'EnumProperty' else 0
    if q.type == 'ByteProperty' and not idx: return 'None'
    o = p.obj(idx) if idx else None
    return o['name'] if o else None


def enum_resolve(enum_name, body, value):
    """What UEnum::GetIndexByName makes of a serialized name (Enum.cpp 177-195, 464-527): an entry equal to it, case
    aside, else - past any 'X::' prefix, which it drops - the short name or '<Enum>::<short>' (the latter unless the
    enum is Regular). (entry, value) or None."""
    short = value.split('::', 1)[1] if '::' in value else value
    if body[0] == 'native':
        return (value, None) if short.lower() in {n.lower() for n in body[1]} else None
    entries, form = body[1], body[2]
    want = {value.lower(), short.lower()} | ({('%s::%s' % (enum_name, short)).lower()} if form != 0 else set())
    return next(((n, v) for n, v in entries if n.lower() in want), None)


@rule
def enum_payload(pkg):
    """A UserDefinedEnum export is exactly its tags, the object-GUID flag, the entries and CppForm - one byte more or
    less is the serial-size Fatal (Enum.cpp 33-95). CppForm is Namespaced (1): GenerateFullEnumName check()s it, and
    Regular would make every short-name lookup miss (UserDefinedEnum.cpp 40-46; Enum.cpp 144-152, 464-527). Every
    entry is '<the enum's object name>::<short>' and they are unique (Enum.cpp 340-359; one global name map,
    Enum.cpp 83-94). The last entry is the <prefix>_MAX sentinel, strictly above every other value: the engine takes
    GetMaxEnumValue as the invalid marker for an unresolved name and GetValidValue (UserDefinedEnum.cpp 178-210;
    PropertyByte.cpp 68-78; EnumProperty.cpp 130-159). A DisplayNameMap key is a short name, never 'Enum::X'
    (UserDefinedEnum.cpp 150-176)."""
    for i, e in enumerate(pkg.exports):
        if pkg.class_of(i + 1) != 'UserDefinedEnum': continue
        entries, form, end = enum_body(pkg, i)
        if end != e['size']: yield i, 'payload read to %d of %d bytes' % (end, e['size'])
        if form != 1: yield i, 'CppForm %d, not Namespaced' % form
        names = [n for n, _ in entries]
        bad = [n for n in names if not n.startswith(e['name'] + '::')]
        if bad: yield i, 'entries not under %s::: %s' % (e['name'], bad[:4])
        dup = sorted({n for n in names if [m.lower() for m in names].count(n.lower()) > 1})
        if dup: yield i, 'entries named twice: %s' % dup
        if not entries or not names[-1].endswith('_MAX'):
            yield i, 'last entry %s is not a _MAX sentinel' % (names[-1] if names else None)
        elif any(v >= entries[-1][1] for _, v in entries[:-1]):
            yield i, 'sentinel %s = %d is not above every other value %s' % (entries[-1][0], entries[-1][1], [v for _, v in entries[:-1]])
        dn = pkg.tag(i, 'DisplayNameMap')
        for key in (display_keys(pkg, dn) if dn else []):
            if '::' in key: yield i, 'DisplayNameMap key %s is a full name' % key


@rule
def enum_byte_range(pkg):
    """A UserDefinedEnum held in a ByteProperty - every Blueprint variable, parameter and struct member of one, as the
    Kismet compiler makes them (KismetCompilerMisc.cpp 1071-1087) - has every value, _MAX included, in 0..255: the
    value is stored as uint8 (PropertyByte.cpp 68-78, 99-111), so 256 would read back as 0."""
    for i in range(len(pkg.exports)):
        st = pkg.struct(i)
        if not st: continue
        stack = list(st.props)
        while stack:
            q = stack.pop()
            stack += q.subs
            if q.type != 'ByteProperty' or not q.ref: continue
            try: r = pkg.resolve(q.ref)
            except Exception: r = None
            if not r or r[0].class_of(r[1] + 1) != 'UserDefinedEnum': continue
            out = [(n, v) for n, v in enum_body(*r)[0] if not 0 <= v <= 255]
            if out: yield i, 'ByteProperty %s holds %s, whose %s do not fit a byte' % (q.name, r[0].exports[r[1]]['name'], out[:3])


@rule
def enum_tags(pkg):
    """A tagged Byte value of an enum (tag EnumName != None), an Enum value, and an enum element of an array, set or
    map, is an 8-byte FName that the enum resolves: ByteProperty / EnumProperty SerializeItem read an FName under an
    enum (PropertyByte.cpp 39-79, EnumProperty.cpp 130-159), and GetIndexByName maps a name it cannot resolve to
    GetMaxEnumValue with only a log line (Enum.cpp 177-195, 464-565). A 1-byte value there is read as 8 and the Tag.Size
    check that would catch it is compiled out of Shipping (Class.cpp 1491-1500)."""
    for i, start, owner, what in list(owned_lists(pkg)) + [(i, 0, None, e['name']) for i, e in enumerate(pkg.exports)
                                                           if not pkg.is_struct(i) and class_owner(pkg, e['cls']) is None
                                                           and pkg.class_of(i + 1) not in ('UserDefinedEnum', 'Enum')]:
        b = pkg.blob(i)
        if owner is None:
            try: pkg.tags(i)
            except (IndexError, struct.error): continue        # a native object that serializes itself (RigVM)
        for kind, where, t, p, q, at, size, enum in walk(pkg, i, start, owner, what):
            if kind == 'tag' and (t['type'] == 'EnumProperty' or t['type'] == 'ByteProperty' and t['enum'] != 'None'):
                enum = t['enum']
            elif kind != 'elem' or not enum or enum == 'None':
                continue
            if size != 8:
                yield i, '%s: a value of enum %s is %d bytes, not an 8-byte name' % (where, enum, size); continue
            n, num = struct.unpack_from('<ii', b, at)
            name = pkg.names[n] + ('_%d' % (num - 1) if num else '')
            body = enum_of(pkg, enum, p, q)
            if body is not None and enum_resolve(body[3] if body[0] == 'ude' else enum, body, name) is None:
                yield i, '%s: %s is no entry of %s, so it loads as the enum\'s _MAX' % (where, name, enum)


@rule
def enum_native_clash(pkg):
    """A UserDefinedEnum's entry names are not those of a game or engine enum: AddNamesToMasterList registers every
    entry FName in one global map, keeps the first on a collision with only a log line, and LookupEnumName then
    answers with the other enum (Enum.cpp 83-94, 261-275, 610; UserDefinedEnum.cpp 192-204; Property.cpp 1043). A
    UserDefinedEnum named like a native enum shares its '<Enum>::<short>' names wherever the short names meet."""
    natives = native_enums()
    for i, e in enumerate(pkg.exports):
        if pkg.class_of(i + 1) != 'UserDefinedEnum' or e['name'] not in natives: continue
        mine = {n.split('::', 1)[-1].lower() for n, _ in enum_body(pkg, i)[0]}
        both = sorted(s for s in natives[e['name']] if s.lower() in mine)
        if both: yield i, 'enum %s shares %s with the native enum of that name' % (e['name'], ['%s::%s' % (e['name'], s) for s in both[:4]])


FUNC_Net = 0x40


def field_prop(pkg, owner, name):
    """(Package, Prop) of the property a bytecode field path names: `name` among the properties of the struct its
    ResolvedOwner is, in this package or the /Game package an import leads to. None when that is not known."""
    try: r = pkg.resolve(owner) if owner else None
    except Exception: r = None
    st = r[0].struct(r[1]) if r else None
    q = next((q for q in st.props if q.name == name), None) if st else None
    return (r[0], q) if q else None


def ude_max(p, q):
    """(enum name, GetMaxEnumValue) of the UserDefinedEnum a Byte / Enum property holds, else None: the largest value
    over every entry, the sentinel included (Enum.cpp 210-229)."""
    idx = q.ref if q.type == 'ByteProperty' else q.enum if q.type == 'EnumProperty' else 0
    try: r = p.resolve(idx) if idx else None
    except Exception: r = None
    if not r or r[0].class_of(r[1] + 1) != 'UserDefinedEnum': return None
    entries = body_at(*r)[1]
    return (r[0].exports[r[1]]['name'], max(v for _, v in entries)) if entries else None


def const_value(pkg, i, n):
    """The value of an EX_ByteConst / EX_IntConstByte / EX_IntConst node of export i's script, else None."""
    if n.op in (0x24, 0x2C): return pkg.blob(i)[n.disk + 1]
    if n.op == 0x1D: return n.ops[0][1]
    return None


@rule
def enum_net_values(pkg):
    """A constant a script stores into a replicated (CPF_Net) property of a UserDefinedEnum, or passes to an RPC
    parameter of one, is at most the enum's GetMaxEnumValue: FByteProperty / FEnumProperty NetSerializeItem send only
    CeilLogTwo64(GetMaxEnumValue() + 1) bits (PropertyByte.cpp 99-111, EnumProperty.cpp 182-194; Enum.cpp 210-229),
    so a larger value arrives with its high bits dropped. (A value computed at run time is out of a static check's
    reach; the constants are what a compiler chooses.)"""
    for i, st in functions(pkg):
        for n in statements(pkg, i)[0]:
            if n.op == 0x0F and len(n.kids) == 2:
                v = const_value(pkg, i, n.kids[1])
                hit = field_prop(pkg, n.ops[0][2], n.ops[0][1][-1]) if v is not None else None
                if not hit or not hit[1].flags & CPF_Net: continue
                m = ude_max(*hit)
                if m and v > m[1]:
                    yield i, 'mem %d stores %d into replicated %s, of %s whose largest value is %d' % (n.mem, v, hit[1].name, m[0], m[1])
            elif n.op in (0x1B, 0x1C, 0x45, 0x46):
                f = callee(pkg, i, n)
                if f is None or not f.function_flags & FUNC_Net: continue
                parms = [q for q in f.props if q.flags & CPF_Parm and not q.flags & 0x400]
                for q, a in zip(parms, [k for k in n.kids if k.op != 0x16]):
                    v = const_value(pkg, i, a)
                    m = ude_max(pkg, q) if v is not None else None
                    if m and v > m[1]:
                        yield i, 'mem %d passes %d to RPC parameter %s, of %s whose largest value is %d' % (n.mem, v, q.name, m[0], m[1])


TYPES_RULES = ['uds_members', 'uds_local_defaults', 'uds_tags', 'enum_payload', 'enum_byte_range', 'enum_tags', 'enum_native_clash',
               'enum_net_values']
