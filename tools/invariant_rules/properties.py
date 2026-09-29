import sys
from invariants import *
# PROPS: the rules for a package's FProperty fields and the tagged defaults that name them.
#
# Two kinds. The property rules read every FProperty a struct export declares (container and enum sub-fields included):
# its field class, its flags, its type tail, its ElementSize. The tag rules read every export's FPropertyTags - CDOs,
# component archetypes, asset instances, subobjects, a UserDefinedStruct's default instance - and hold each against the
# property it names: its own class's (this package or a parent Blueprint's, through Package.resolve) or a native class's,
# read off UeApi's headers (and, when one is on this machine, the Dumper-7 object dump UeApi was generated from, which
# also lists the members UeApi leaves out). What neither knows (a native class missing, a /Game package not found) is
# skipped, never reported: STATS counts the skips, CHECKED what each rule did judge.
#
# Each rule's docstring cites the UE 4.27 source (Engine/Source/Runtime/CoreUObject unless named) that makes it one.
import glob, json, os, re, struct
import invariants

TOOLS = os.path.dirname(os.path.abspath(invariants.__file__))
# The generated headers (Engine.h, ...) are not in git: a worktree's UeApi may hold only the hand-written ones.
UEAPI = UEAPI_DIR if os.path.exists(os.path.join(UEAPI_DIR, 'Engine.h')) else None
STATS = {}                  # rule -> {reason: count} of what a rule could not judge and skipped
CHECKED = {}                # rule -> how many properties / tags / values it judged


def _skip(rule_name, why):
    d = STATS.setdefault(rule_name, {})
    d[why] = d.get(why, 0) + 1


def _judged(rule_name, n=1):
    CHECKED[rule_name] = CHECKED.get(rule_name, 0) + n


# ---- what the engine's headers say: UeApi's classes, enums and struct layouts

class _Api:
    """UeApi read once: every native class by path (/Script/Engine.Actor) with its parent's path and its members' UE
    names -> C++ types (a member UeApi spells otherwise carries <Name>__UeName), every native enum's enumerators, and
    Types.json's struct sizes / alignments."""
    def __init__(s):
        s.classes, by_cpp, s.enums, s.enum_cpp = {}, {}, {}, {}
        for f in sorted(glob.glob(os.path.join(UEAPI, '*.h'))):
            text = open(f, encoding='utf-8-sig', errors='replace').read().replace('\r\n', '\n')
            for m in re.finditer(r'^class (\w+)(?:\s*:\s*public ([\w:]+)[^\n{]*)?\s*\n\{\n(.*?)\n\};', text, re.M | re.S):
                cpp, parent, body = m.groups()
                meta = re.search(r'UE_CLASS\("([^"]+)", "([^"]+)"\)', body)
                if not meta: continue
                rename = dict(re.findall(r'static constexpr const char\* (\w+)__UeName = "([^"]+)";', body))
                members = {}
                for line in body.split('\n'):
                    l = line.strip()
                    if not l.endswith(';') or l.startswith(('static ', 'using ', 'UE_', 'friend ', 'enum ', '//', 'typedef ')):
                        continue
                    if re.search(r'\)\s*(const\s*)?(override\s*)?;$', l): continue          # a function
                    mm = re.match(r'(.*?)\s*\b(\w+)\s*;$', l)
                    if mm and mm.group(1): members[rename.get(mm.group(2), mm.group(2)).lower()] = mm.group(1)
                path = meta.group(1) + '.' + meta.group(2)
                s.classes[path] = dict(cpp=cpp, parent=parent, members=members, interface=bool(re.match(r'I[A-Z]', cpp)))
                by_cpp[cpp] = path
            for m in re.finditer(r'^enum class (\w+) : \w+\s*\n\{\n(.*?)\n\};', text, re.M | re.S):
                s.enum_cpp[m.group(1)] = re.findall(r'^\s*(\w+)\s*=', m.group(2), re.M)
        for c in s.classes.values(): c['parent'] = by_cpp.get(c['parent']) if c['parent'] else None
        s.by_cpp = by_cpp
        types = json.load(open(os.path.join(UEAPI, 'Types.json'), encoding='utf-8'))
        s.structs = {v['package'] + '.' + v['name']: (v['size'], v['align']) for v in types['structs'].values()}
        s.struct_cpp = {k: v['package'] + '.' + v['name'] for k, v in types['structs'].items()}
        for cpp, v in types['enums'].items():
            if cpp in s.enum_cpp: s.enums[v['package'] + '.' + v['name']] = s.enum_cpp[cpp]
        s.enum_by_name = {}
        for cpp, v in types['enums'].items():
            if cpp in s.enum_cpp: s.enum_by_name.setdefault(v['name'].lower(), []).append(v['package'] + '.' + v['name'])
        # UObject's reflection classes UeApi leaves out: a class object's own chain.
        for path, parent in (('/Script/CoreUObject.Object', None), ('/Script/CoreUObject.Field', '/Script/CoreUObject.Object'),
                             ('/Script/CoreUObject.Struct', '/Script/CoreUObject.Field'),
                             ('/Script/CoreUObject.Class', '/Script/CoreUObject.Struct'),
                             ('/Script/Engine.BlueprintGeneratedClass', '/Script/CoreUObject.Class'),
                             ('/Script/UMG.WidgetBlueprintGeneratedClass', '/Script/Engine.BlueprintGeneratedClass'),
                             ('/Script/Engine.AnimBlueprintGeneratedClass', '/Script/Engine.BlueprintGeneratedClass')):
            s.classes.setdefault(path, dict(cpp='', parent=parent, members={}, interface=False))


    def dumped(s):
        """{class path: {lower property name: field class}} off Dumper-7's GObjects-Dump-WithProperties.txt, when one
        is here (OBJECT_DUMP): every reflected property of every native class, the static arrays and other members
        UeApi leaves out included. {} without one; the rules then know only UeApi's members."""
        if not hasattr(s, '_dumped'):
            s._dumped, cur = {}, None
            if OBJECT_DUMP:
                for line in open(OBJECT_DUMP, encoding='utf-8', errors='replace'):
                    if line.startswith('[') and '}     ' in line:
                        if cur is not None:
                            f = line.split('}     ', 1)[1].split()
                            if len(f) == 2: cur[f[1].lower()] = f[0]
                        continue
                    m = re.match(r'\[\w+\] \{0x\w+\} Class (/Script/\S+)$', line.rstrip())
                    cur = s._dumped.setdefault(m.group(1), {}) if m else None
        return s._dumped


# Optional: the object dump of the game build the SDK came from (genueapi.py's input sits beside it). Newest first.
OBJECT_DUMP = os.path.join(SDK_DUMP, 'GObjects-Dump-WithProperties.txt') if SDK_DUMP else None
_API = []


def api():
    if not _API: _API.append(_Api())
    return _API[0]


# ---- a package's properties, and the class an export is an instance of

def props(pkg):
    """(export index, dotted path, Prop) of every FProperty a struct export of pkg declares, sub-fields included."""
    if '_props_all' not in pkg.__dict__:
        pkg._props_all = [x for i in range(len(pkg.exports)) if pkg.struct(i) for p in pkg.struct(i).props for x in _walk(i, p.name, p)]
    return pkg._props_all


def _walk(i, where, p):
    yield i, where, p
    for s in p.subs: yield from _walk(i, where + '.' + s.name, s)


def content_roots(pkg):
    here = pkg.base.replace(os.sep, '/')
    return [here[:here.rindex('/Content/') + len('/Content')]] + list(GAME_CONTENT)


def load_path(pkg, path):
    """The Package a /Game object path (/Game/A/B.B_C) lives in, looked up as Package.resolve does; None if absent."""
    name = path.split('.')[0]
    for root in content_roots(pkg):
        if os.path.exists(root + name[len('/Game'):] + '.uasset'): return load(root + name[len('/Game'):])
    return None


def class_path_of(pkg, idx):
    """The full path of the class of the object idx names: /Script/Engine.StaticMesh, /Game/A/BP_X.BP_X_C."""
    o = pkg.obj(idx)
    if idx < 0: return o['class_package'] + '.' + o['class_name']
    return pkg.path(o['cls']) if o['cls'] else '/Script/CoreUObject.Class'


def chain(pkg, path):
    """[class path, its parent's, ...] up to UObject, or None where a level cannot be read. A /Game class is read off
    its package (the super index of its class export), a /Script one off UeApi."""
    out = []
    while path:
        if path in out or len(out) > 64: return None
        out.append(path)
        if path.startswith('/Script/'):
            c = api().classes.get(path)
            if c is None: return None
            path = c['parent']
            continue
        other = load_path(pkg, path)
        if other is None: return None
        k = next((k for k in range(len(other.exports)) if other.path(k + 1).lower() == path.lower()), None)
        st = other.struct(k) if k is not None else None
        if st is None or not hasattr(st, 'class_flags'): return None
        path = other.path(st.super) if st.super else None
        pkg = other
    return out


def is_child(pkg, child, parent):
    """Whether class path child IsChildOf class path parent; None when its chain cannot be read."""
    c = chain(pkg, child)
    if c is None: return None
    return parent.lower() in (x.lower() for x in c)


def scope(pkg, i):
    """What export i's tags can name: {lower name: (Package, Prop)} of each Blueprint level of its class (this package's
    class exports and parent Blueprints' packages), {lower name: (UeApi C++ type or None, the object dump's field class
    or None)} of the native classes above them, and the native class path the chain reaches; None when a level cannot
    be read."""
    cls = pkg.exports[i]['cls']
    own, native = {}, {}
    p, idx = pkg, cls
    for _ in range(64):
        if idx == 0: return None
        path = p.path(idx)
        if idx < 0 and path.startswith('/Script/'):
            c = chain(p, path)
            if c is None: return None
            for q in reversed(c):
                for k, v in api().dumped().get(q, {}).items(): native[k] = (native.get(k, (None, None))[0], v)
                for k, v in api().classes[q]['members'].items(): native[k] = (v, native.get(k, (None, None))[1])
            return own, native, path
        r = p.resolve(idx)
        if not r: return None
        p, k = r
        st = p.struct(k)
        if st is None or not hasattr(st, 'class_flags'): return None
        for q in st.props: own.setdefault(q.name.lower(), (p, q))
        idx = st.super
    return None


def tagged(pkg):
    """(export index, tag list, what the tags can name as scope() gives it) of every export that is not a struct but
    an instance of a class (CDO, archetype, asset, subobject), plus each UserDefinedStruct's default instance against
    its own properties."""
    if '_tagged_all' not in pkg.__dict__: pkg._tagged_all = list(_tagged(pkg))
    return pkg._tagged_all


def _tagged(pkg):
    for i, e in enumerate(pkg.exports):
        if pkg.is_struct(i):
            st = pkg.struct(i)
            if getattr(st, 'defaults', None) is not None:
                st.defaults.start = next(k[1] for k, v in pkg._tags.items() if v is st.defaults)
                yield i, st.defaults, ({q.name.lower(): (pkg, q) for q in st.props}, {}, 'UserDefinedStruct')
            continue
        if e['cls'] == 0: continue
        try: tl = pkg.tags(i)
        except Exception: continue          # a payload that does not start with tags; the tag rules have nothing to read
        if not tl: continue
        tl.start = 0
        yield i, tl, scope(pkg, i)


# ---- the property rules

CONCRETE = {'BoolProperty', 'ByteProperty', 'Int8Property', 'Int16Property', 'IntProperty', 'Int64Property',
            'UInt16Property', 'UInt32Property', 'UInt64Property', 'FloatProperty', 'DoubleProperty', 'NameProperty',
            'StrProperty', 'TextProperty', 'ObjectProperty', 'ClassProperty', 'WeakObjectProperty', 'LazyObjectProperty',
            'SoftObjectProperty', 'SoftClassProperty', 'InterfaceProperty', 'StructProperty', 'ArrayProperty',
            'SetProperty', 'MapProperty', 'EnumProperty', 'DelegateProperty', 'MulticastInlineDelegateProperty',
            'MulticastSparseDelegateProperty', 'FieldPathProperty'}
INTEGRAL = {'ByteProperty', 'Int8Property', 'Int16Property', 'IntProperty', 'Int64Property', 'UInt16Property',
            'UInt32Property', 'UInt64Property'}


@rule
def prop_bool_size(pkg):
    """A BoolProperty's tail is FieldSize, ByteOffset, ByteMask, FieldMask, BoolSize, NativeBool; BoolSize is 1, 2, 4
    or 8 and FieldSize and ElementSize equal it. The loader rebuilds the bool from BoolSize and NativeBool alone
    (PropertyBool.cpp Serialize 125-151), SetBoolSize check()s FieldSize == ElementSize != 0 (61-91), and any other size
    is a Fatal log from GetMinAlignment when the struct links, Shipping included (93-110)."""
    for i, where, p in props(pkg):
        if p.type != 'BoolProperty': continue
        _judged('prop_bool_size')
        field, size = p.bool[0], p.bool[4]
        if size not in (1, 2, 4, 8) or field != size or p.elem_size != size:
            yield i, '%s: bool tail %s, ElementSize %d' % (where, bytes(p.bool).hex(), p.elem_size)


@rule
def prop_bool_native(pkg):
    """A Blueprint's bools are native C++ bools, tail 01 00 01 FF 01 01: the editor makes every one with
    SetBoolSize(sizeof(bool), true) (Editor/KismetCompiler/Private/KismetCompilerMisc.cpp 1056-1061). NativeBool 0 loads
    as SetBoolSize(1, false), a one-bit bitfield at bit 0 (PropertyBool.cpp 61-91, 125-151): not POD or zero-constructed,
    IsNativeBool() false (FieldMask != 0xFF, UnrealType.h 2017-2020), only bit 0 of its byte read or written."""
    for i, where, p in props(pkg):
        if p.type == 'BoolProperty': _judged('prop_bool_native')
        if p.type == 'BoolProperty' and p.bool != (1, 0, 1, 0xFF, 1, 1):
            yield i, '%s: bool tail %s, not a native bool (01 00 01 ff 01 01)' % (where, bytes(p.bool).hex())


@rule
def prop_field_classes(pkg):
    """Every field class, sub-fields included, is a concrete property class: FField::Construct builds whatever name it
    reads (Field.cpp 975-981), and the abstract bases Property / NumericProperty / ObjectPropertyBase /
    MulticastDelegateProperty inherit FProperty::LinkInternal's check(0) (Property.cpp 968-971) and PURE_VIRTUAL value
    operations, fatal in every build. Array and Set carry one sub-field and Map two, none of them None (PropertyArray.cpp
    57-68 links Inner unchecked, PropertySet.cpp 212-216 / PropertyMap.cpp 247-252 check them), and an EnumProperty's
    underlying field is an integer property (EnumProperty.cpp 373-382; the editor's is a ByteProperty,
    KismetCompilerMisc.cpp 1075-1081)."""
    for i, where, p in props(pkg):
        _judged('prop_field_classes')
        if p.type not in CONCRETE: yield i, '%s: field class %s is not a concrete property class' % (where, p.type)
        want = {'ArrayProperty': 1, 'SetProperty': 1, 'MapProperty': 2, 'EnumProperty': 1}.get(p.type, 0)
        if len(p.subs) != want or any(s.type == 'None' for s in p.subs):
            yield i, '%s: %s has sub-fields %s' % (where, p.type, [s.type for s in p.subs])
        if p.type == 'EnumProperty' and p.subs and p.subs[0].type not in INTEGRAL:
            yield i, '%s: enum over %s, not an integer property' % (where, p.subs[0].type)


CPF_ComputedFlags, CPF_EditorOnly, CPF_SkipSerialization = 0x0008001040000200, 0x800000000, 0x0080000000000000


@rule
def prop_flags_on_disk(pkg):
    """A saved property's flags hold none of CPF_ComputedFlags (POD, NoDestructor, ZeroConstructor, HasGetValueTypeHash:
    ObjectMacros.h 443), which the saver masks out and the loader discards and LinkInternal recomputes (Property.cpp
    565-571); never CPF_EditorOnly, which a cooked package filters and a game build check()s against (Property.cpp
    573-577, Class.cpp 1733-1745); and ArrayDim 1, since a Blueprint has no static arrays and Size = ArrayDim *
    ElementSize is its footprint in the layout, read unvalidated (Property.cpp 562, UnrealType.h 1101-1114)."""
    for i, where, p in props(pkg):
        _judged('prop_flags_on_disk')
        if p.flags & CPF_ComputedFlags: yield i, '%s: flags %#x hold computed flags %#x' % (where, p.flags, p.flags & CPF_ComputedFlags)
        if p.flags & CPF_EditorOnly: yield i, '%s: flags %#x hold CPF_EditorOnly' % (where, p.flags)
        if p.dim != 1: yield i, '%s: ArrayDim %d' % (where, p.dim)


# ElementSize per field class: the runtime size, and the editor's where an FName (12 bytes with its display index)
# makes it bigger - the game's cooks carry the editor's. FFieldPath has WITH_EDITORONLY_DATA members (FieldPath.h 44-50).
SIZES = {'ByteProperty': (1,), 'Int8Property': (1,), 'Int16Property': (2,), 'UInt16Property': (2,), 'IntProperty': (4,),
         'UInt32Property': (4,), 'FloatProperty': (4,), 'Int64Property': (8,), 'UInt64Property': (8,), 'DoubleProperty': (8,),
         'ObjectProperty': (8,), 'ClassProperty': (8,), 'WeakObjectProperty': (8,), 'LazyObjectProperty': (28,),
         'StrProperty': (16,), 'ArrayProperty': (16,), 'InterfaceProperty': (16,), 'TextProperty': (24,),
         'SetProperty': (80,), 'MapProperty': (80,), 'MulticastInlineDelegateProperty': (16,),
         'MulticastSparseDelegateProperty': (1,), 'FieldPathProperty': (32, 48), 'NameProperty': (8, 12),
         'DelegateProperty': (16, 20), 'SoftObjectProperty': (40, 48), 'SoftClassProperty': (40, 48)}
ALIGN = {'ByteProperty': 1, 'Int8Property': 1, 'Int16Property': 2, 'UInt16Property': 2, 'IntProperty': 4, 'UInt32Property': 4,
         'FloatProperty': 4, 'Int64Property': 8, 'UInt64Property': 8, 'DoubleProperty': 8, 'ObjectProperty': 8,
         'ClassProperty': 8, 'WeakObjectProperty': 4, 'LazyObjectProperty': 4, 'StrProperty': 8, 'ArrayProperty': 8,
         'InterfaceProperty': 8, 'TextProperty': 8, 'SetProperty': 8, 'MapProperty': 8, 'MulticastInlineDelegateProperty': 8,
         'MulticastSparseDelegateProperty': 1, 'FieldPathProperty': 8, 'NameProperty': 4, 'DelegateProperty': 4,
         'SoftObjectProperty': 8, 'SoftClassProperty': 8}
# A native struct's size in an editor build where WITH_EDITORONLY_DATA members or 12-byte FNames make it bigger than
# Types.json's (runtime) one: what the game's own cooks carry, measured on every one of its packages.
EDITOR_STRUCT_SIZES = {
    '/Script/AIModule.BlackboardKeySelector': 48, '/Script/AnimGraphRuntime.AnimNode_AimOffsetLookAt': 464,
    '/Script/AnimGraphRuntime.AnimNode_ApplyAdditive': 224,
    '/Script/AnimGraphRuntime.AnimNode_BlendSpaceEvaluator': 248,
    '/Script/AnimGraphRuntime.AnimNode_BlendSpacePlayer': 240, '/Script/AnimGraphRuntime.AnimNode_Constraint': 480,
    '/Script/AnimGraphRuntime.AnimNode_LayeredBoneBlend': 200, '/Script/AnimGraphRuntime.AnimNode_LookAt': 800,
    '/Script/AnimGraphRuntime.AnimNode_MakeDynamicAdditive': 72, '/Script/AnimGraphRuntime.AnimNode_ModifyBone': 352,
    '/Script/AnimGraphRuntime.AnimNode_RigidBody': 2224, '/Script/AnimGraphRuntime.AnimNode_RotateRootBone': 168,
    '/Script/AnimGraphRuntime.AnimNode_RotationOffsetBlendSpace': 424,
    '/Script/AnimGraphRuntime.AnimNode_SequenceEvaluator': 88, '/Script/AnimGraphRuntime.AnimNode_Slot': 88,
    '/Script/AnimGraphRuntime.AnimNode_StateResult': 64, '/Script/AnimGraphRuntime.AnimNode_TwoWayBlend': 216,
    '/Script/ControlRig.AnimNode_ControlRig': 896, '/Script/CoreUObject.SoftClassPath': 32,
    '/Script/CoreUObject.SoftObjectPath': 32, '/Script/Engine.AnimNode_ApplyMeshSpaceAdditive': 232,
    '/Script/Engine.AnimNode_ConvertComponentToLocalSpace': 40,
    '/Script/Engine.AnimNode_ConvertLocalToComponentSpace': 40, '/Script/Engine.AnimNode_LinkedAnimGraph': 176,
    '/Script/Engine.AnimNode_LinkedInputPose': 296, '/Script/Engine.AnimNode_Root': 64,
    '/Script/Engine.AnimNode_SaveCachedPose': 352, '/Script/Engine.AnimNode_SequencePlayer': 136,
    '/Script/Engine.AnimNode_UseCachedPose': 56, '/Script/Engine.HitResult': 144,
    '/Script/Engine.InputAxisKeyMapping': 48, '/Script/Engine.MaterialParameterInfo': 20,
    '/Script/Engine.MinimalViewInfo': 1568, '/Script/Engine.PointerToUberGraphFrame': 16,
    '/Script/Engine.PoseLink': 24, '/Script/Engine.PostProcessSettings': 1424, '/Script/FSD.ActionIconMapping': 48,
    '/Script/FSD.AnimNode_Tentacle': 544, '/Script/FSD.ChallengeInfo': 176, '/Script/FSD.ClaimableRewardEntry': 160,
    '/Script/FSD.ClaimableRewardView': 152, '/Script/FSD.CustomKeySetting': 64,
    '/Script/FSD.DamageTypeDescription': 128, '/Script/FSD.DialogStruct': 96,
    '/Script/FSD.DifficultyMutatorInfo': 96, '/Script/FSD.DisplayContent': 168,
    '/Script/FSD.JettyBootEventSettings': 96, '/Script/FSD.OssiumCrystalBaseData': 144,
    '/Script/FSD.SchematicType': 128, '/Script/FSD.TutorialHint': 120, '/Script/GameplayTags.GameplayTag': 12,
    '/Script/InputCore.Key': 32, '/Script/SlateCore.ButtonStyle': 768, '/Script/SlateCore.KeyEvent': 64,
    '/Script/SlateCore.PointerEvent': 120, '/Script/SlateCore.ProgressBarStyle': 488,
    '/Script/SlateCore.ScrollBarStyle': 1448, '/Script/SlateCore.SlateBrush': 160,
    '/Script/SlateCore.SlateFontInfo': 104, '/Script/SlateCore.SlateSound': 32,
    '/Script/SlateCore.TextBlockStyle': 712, '/Script/UMG.EventReply': 192
}


def struct_layout(pkg, ref):
    """(size, alignment) of the struct an FPackageIndex names, as FStructProperty::LinkInternal takes it:
    Align(PropertiesSize, MinAlignment) (PropertyStruct.cpp 103). A native one from Types.json, a UserDefinedStruct laid
    out from its own properties' ElementSizes; None if unknown."""
    path = pkg.path(ref)
    if pkg.class_of(ref) == 'ScriptStruct' and ref < 0:
        return api().structs.get(path)
    r = pkg.resolve(ref)
    if not r: return None
    other, k = r
    st = other.struct(k)
    if st is None: return None
    end, align = 0, 1
    for q in st.props:
        a = prop_align(other, q)
        if a is None: return None
        end = (end + a - 1) // a * a + q.dim * q.elem_size
        align = max(align, a)
    return (end + align - 1) // align * align, align


def prop_align(pkg, p):
    if p.type == 'BoolProperty': return p.bool[4]
    if p.type == 'EnumProperty': return prop_align(pkg, p.subs[0]) if p.subs else None
    if p.type == 'StructProperty':
        lay = struct_layout(pkg, p.ref)
        return lay and lay[1]
    return ALIGN.get(p.type)


@rule
def prop_element_size(pkg):
    """ElementSize is the property's size as the engine links it: sizeof the C++ type (UnrealType.h 1143-1146), the
    underlying property's for an enum (EnumProperty.cpp 379), Align(PropertiesSize, MinAlignment) of its struct for a
    struct (PropertyStruct.cpp 103). LinkInternal overwrites it at load, so a wrong one is AssetGen's own size model
    differing from the engine's, which matters where that model feeds what the engine does not recompute (a
    UserDefinedStruct's layout, raw offset reads). Bools are prop_bool_size's."""
    for i, where, p in props(pkg):
        if p.type in SIZES:
            _judged('prop_element_size')
            if p.elem_size not in SIZES[p.type]: yield i, '%s: %s ElementSize %d, not %s' % (where, p.type, p.elem_size, SIZES[p.type])
        elif p.type == 'EnumProperty':
            if p.subs and p.elem_size != p.subs[0].elem_size:
                yield i, '%s: EnumProperty ElementSize %d, its %s %d' % (where, p.elem_size, p.subs[0].type, p.subs[0].elem_size)
        elif p.type == 'StructProperty' and p.ref:
            lay = struct_layout(pkg, p.ref)
            if lay is None: _skip('prop_element_size', 'struct layout unknown'); continue
            path = pkg.path(p.ref)
            _judged('prop_element_size')
            ok = {lay[0], EDITOR_STRUCT_SIZES.get(path, lay[0])}
            if p.elem_size not in ok: yield i, '%s: StructProperty %s ElementSize %d, not %s' % (where, path, p.elem_size, sorted(ok))


@rule
def prop_ref_targets(pkg):
    """A property's reference tail names the right kind of object. A StructProperty's Struct is a ScriptStruct under
    /Script or a UserDefinedStruct; a null or wrong one is replaced by a fallback struct with the wrong size, shifting
    every later offset (PropertyStruct.cpp 83-103, 189-224). A Class / SoftClassProperty's PropertyClass is
    /Script/CoreUObject.Class and its MetaClass a class (PropertyClass.cpp 48-74, 98-133; KismetCompilerMisc.cpp
    1017-1038); any other PropertyClass nulls every class value (PropertyBaseObject.cpp 583). An object-family
    property names a class (the editor always sets it). An InterfaceProperty names an interface class other than
    UInterface itself: with any other the loaded FScriptInterface keeps a null interface pointer (PropertyInterface.cpp
    252-279, 114-119). A delegate names its signature function, a byte / enum property its UEnum."""
    for i, where, p in props(pkg):
        t, cls = p.type, (lambda idx: pkg.class_of(idx) if idx else None)
        if p.ref or p.enum or p.type in ('ClassProperty', 'SoftClassProperty', 'InterfaceProperty', 'StructProperty'): _judged('prop_ref_targets')
        if t == 'StructProperty':
            c = cls(p.ref)
            if c not in ('ScriptStruct', 'UserDefinedStruct') or (c == 'ScriptStruct' and p.ref < 0 and not pkg.path(p.ref).startswith('/Script/')):
                yield i, '%s: Struct %s (%s)' % (where, pkg.path(p.ref), c)
        elif t in ('ClassProperty', 'SoftClassProperty'):
            if pkg.path(p.ref) != '/Script/CoreUObject.Class' or not p.meta or not (cls(p.meta) or '').endswith('Class'):
                yield i, '%s: %s PropertyClass %s MetaClass %s' % (where, t, pkg.path(p.ref), pkg.path(p.meta))
        elif t in ('ObjectProperty', 'WeakObjectProperty', 'LazyObjectProperty', 'SoftObjectProperty'):
            if not p.ref or not (cls(p.ref) or '').endswith('Class'): yield i, '%s: %s PropertyClass %s' % (where, t, pkg.path(p.ref))
        elif t == 'InterfaceProperty':
            path = pkg.path(p.ref)
            if not p.ref or path == '/Script/CoreUObject.Interface' or not (cls(p.ref) or '').endswith('Class'):
                yield i, '%s: InterfaceClass %s' % (where, path); continue
            if path.startswith('/Script/'):
                c = api().classes.get(path)
                if c is None: _skip('prop_ref_targets', 'native interface class not in UeApi'); continue
                if not c['interface']: yield i, '%s: InterfaceClass %s is not an interface' % (where, path)
                continue
            r = pkg.resolve(p.ref)
            if not r: _skip('prop_ref_targets', 'interface package not found'); continue
            st = r[0].struct(r[1])
            if not st or not getattr(st, 'class_flags', 0) & 0x4000:
                yield i, '%s: InterfaceClass %s is not CLASS_Interface' % (where, path)
        elif t in ('DelegateProperty', 'MulticastInlineDelegateProperty', 'MulticastSparseDelegateProperty'):
            if cls(p.ref) not in Package.FUNCTION_CLASSES: yield i, '%s: %s signature %s' % (where, t, pkg.path(p.ref))
        elif t == 'ByteProperty':
            if p.ref and cls(p.ref) not in ('Enum', 'UserDefinedEnum'): yield i, '%s: ByteProperty Enum %s' % (where, pkg.path(p.ref))
        elif t == 'EnumProperty':
            if cls(p.enum) not in ('Enum', 'UserDefinedEnum'): yield i, '%s: EnumProperty Enum %s' % (where, pkg.path(p.enum))


# Hashability of a set element / map key (Property.cpp 1517-1522 check()s CPF_HasGetValueTypeHash). Never hashable as
# loaded: bool (PropertyBool.cpp 111-124 never adds it), array (PropertyArray.cpp 57-68 adds no computed flag), and the
# TProperty types whose C++ type has no GetTypeHash (UnrealType.h 1021-1029): FText, FScriptSet, FScriptMap and the
# script delegates. A native struct is hashable when its CppStructOps has GetTypeHash (PropertyStruct.cpp 104-112):
# the engine structs below, from a scan of Engine/Source/Runtime and Engine/Plugins for a GetTypeHash overload
# (plus the ones that inherit one: SoftClassPath, the Vector_NetQuantize family). A UserDefinedStruct always is (no
# CppStructOps). A struct of the game's own modules is not judged: its source is not here.
UNHASHABLE = {'BoolProperty', 'TextProperty', 'ArrayProperty', 'SetProperty', 'MapProperty', 'DelegateProperty',
              'MulticastInlineDelegateProperty', 'MulticastSparseDelegateProperty'}
HASHABLE_STRUCTS = {
    'AdaptorTriangleID', 'AssetData', 'CachedRigElement', 'Color', 'CurveTableRowHandle', 'DateTime', 'EdGraphPinReference',
    'EdgeID', 'FontData', 'FontOutlineSettings', 'GameplayTag', 'Guid', 'HLODInstancingKey', 'InputChord', 'IntPoint',
    'IntVector', 'Key', 'LinearColor', 'MovieSceneEvaluationKey', 'MovieSceneEvaluationOperand', 'MovieSceneObjectBindingID',
    'MovieSceneTrackInstanceInput', 'NavAgentProperties', 'NiagaraAssetVersion', 'NiagaraDataSetID',
    'NiagaraEmitterNameSettingsRef', 'NiagaraFunctionSignature', 'NiagaraID', 'NiagaraTypeDefinition',
    'NiagaraVMExecutableDataId', 'NiagaraVariableBase', 'PolygonGroupID', 'PolygonID', 'PrimaryAssetId', 'PrimaryAssetType',
    'RigElementKey', 'RigElementKeyCollection', 'SlateFontInfo', 'SolverTrailingData', 'TimerHandle', 'Timespan', 'TriangleID',
    'Vector', 'Vector2D', 'Vector4', 'VertexID', 'VertexInstanceID', 'SoftObjectPath', 'SoftClassPath', 'Vector_NetQuantize',
    'Vector_NetQuantize10', 'Vector_NetQuantize100', 'Vector_NetQuantizeNormal'}
GAME_MODULES = ('/Script/FSD', '/Script/FSDEngine', '/Script/FSDRawInput', '/Script/FSDAnsel', '/Script/DiscordSDK')


@rule
def set_keys_hashable(pkg):
    """A SetProperty's element and a MapProperty's key hash: loading a non-empty default and every Add / Find hash each
    element through GetValueTypeHash, which check()s CPF_HasGetValueTypeHash (Property.cpp 1517-1522; PropertySet.cpp
    348, 358; PropertyMap.cpp 392). The editor refuses an unhashable one (KismetCompilerMisc.cpp 1224-1236 map key,
    1277-1294 set element). See UNHASHABLE / HASHABLE_STRUCTS for which types are."""
    for i, where, p in props(pkg):
        if p.type not in ('SetProperty', 'MapProperty') or not p.subs: continue
        k = p.subs[0]
        _judged('set_keys_hashable')
        if k.type in UNHASHABLE: yield i, '%s: %s key %s is not hashable' % (where, p.type, k.type)
        elif k.type == 'StructProperty' and k.ref < 0 and pkg.class_of(k.ref) == 'ScriptStruct':
            path = pkg.path(k.ref)
            if path.split('.')[0] in GAME_MODULES: _skip('set_keys_hashable', 'game module struct key'); continue
            if path.split('.')[-1] not in HASHABLE_STRUCTS: yield i, '%s: %s key struct %s has no GetTypeHash' % (where, p.type, path)


# ---- the tag rules

GETID = {'ClassProperty': 'ObjectProperty', 'WeakObjectProperty': 'ObjectProperty', 'SoftClassProperty': 'SoftObjectProperty'}


def getid(t): return GETID.get(t, t)


def uds_guid(pkg, ref):
    """A UserDefinedStruct's Guid tag value, or None."""
    r = pkg.resolve(ref)
    if not r: return None
    t = r[0].tag(r[1], 'Guid')
    return t and t['value']


@rule
def tag_types_match(pkg):
    """Each tag's Type is the named property's GetID(): the field class, but ObjectProperty for Class / WeakObject and
    SoftObjectProperty for SoftClass (PropertyTag.cpp 17; PropertyBaseObject.cpp 528-531; PropertySoftObjectPtr.cpp
    31-35). A container tag carries its inner / key / value GetID()s (PropertyTag.cpp 44-56), a struct tag its struct's
    name (25-29). A mismatch the loader cannot convert logs 'Type mismatch' or 'struct type mismatch' and skips the
    value, so the default is silently lost (Class.cpp 1460-1470, PropertyStruct.cpp 350-413, PropertyArray.cpp 707-762).
    The one pair a Blueprint can meet that converts is let through: an enum byte tag into an EnumProperty and back
    (see converts()). A native class's member is held against the object dump's field class, else its UeApi C++ type."""
    for i, tl, sc in tagged(pkg):
        if sc is None: _skip('tag_types_match', 'class chain unread'); continue
        own, native, _ = sc
        for t in tl:
            key = t['name'].lower()
            if key in own:
                owner, p = own[key]
                bad = _tag_vs_prop(owner, t, p)
            elif key in native:
                cpp, field = native[key]
                if field and t['type'] != getid(field) and not converts(t['type'], getid(field), t.get('enum')):
                    bad = 'type %s, the native property is %s' % (t['type'], field)
                else: bad = _tag_vs_cpp(t, cpp) if cpp else None
            else: continue
            _judged('tag_types_match')
            if bad: yield i, 'tag %s: %s' % (t['name'], bad)


def converts(tag_type, prop_id, enum_name):
    """Whether the loader converts a tag of another type into the property rather than skip it: an enum byte tag into an
    EnumProperty and an EnumProperty tag into a ByteProperty, by the enumerator's name (EnumProperty.cpp 400-471,
    PropertyByte.cpp 204-240). Other conversions (numeric widenings, Vector -> Vector4) are no Blueprint's."""
    return {tag_type, prop_id} == {'ByteProperty', 'EnumProperty'} and enum_name not in (None, 'None')


def _tag_vs_prop(owner, t, p):
    if t['type'] != getid(p.type) and not converts(t['type'], getid(p.type), t.get('enum')):
        return 'type %s, the property is %s' % (t['type'], p.type)
    if p.type in ('ArrayProperty', 'SetProperty') and t['inner'] != getid(p.subs[0].type):
        return 'inner %s, the property %s' % (t['inner'], p.subs[0].type)
    if p.type == 'MapProperty' and (t['inner'], t['value_type']) != (getid(p.subs[0].type), getid(p.subs[1].type)):
        return 'key / value %s / %s, the property %s / %s' % (t['inner'], t['value_type'], p.subs[0].type, p.subs[1].type)
    if p.type == 'StructProperty':
        want = owner.obj(p.ref)['name'] if p.ref else None
        if t['struct'] != want: return 'struct %s, the property %s' % (t['struct'], want)
        if owner.class_of(p.ref) == 'UserDefinedStruct' and any(t['struct_guid']):
            g = uds_guid(owner, p.ref)
            if g is not None and g != t['struct_guid']: return 'struct guid %s, the struct %s' % (t['struct_guid'].hex(), g.hex())
    return None


def cpp_tag(ctype):
    """(tag type(s), struct name or inner tag type) a native member of C++ type ctype saves with; None if unknown."""
    t = re.sub(r'\b(class|struct|const|enum)\s+', '', ctype).strip()
    simple = {'float': 'FloatProperty', 'double': 'DoubleProperty', 'int32': 'IntProperty', 'int': 'IntProperty',
              'uint32': 'UInt32Property', 'int64': 'Int64Property', 'uint64': 'UInt64Property', 'int16': 'Int16Property',
              'uint16': 'UInt16Property', 'int8': 'Int8Property', 'uint8': 'ByteProperty', 'bool': 'BoolProperty',
              'FName': 'NameProperty', 'FString': 'StrProperty', 'FText': 'TextProperty'}
    if t in simple: return {simple[t]}, None
    if t.endswith('*'): return {'ObjectProperty'}, None
    m = re.match(r'(\w+)<(.*)>$', t)
    if m:
        tpl, arg = m.groups()
        if tpl in ('TSubclassOf', 'TWeakObjectPtr'): return {'ObjectProperty'}, None
        if tpl in ('TSoftObjectPtr', 'TSoftClassPtr'): return {'SoftObjectProperty'}, None
        if tpl == 'TLazyObjectPtr': return {'LazyObjectProperty'}, None
        if tpl == 'TScriptInterface': return {'InterfaceProperty'}, None
        if tpl == 'TEnumAsByte': return {'ByteProperty'}, None
        if tpl in ('TArray', 'TSet'):
            inner = cpp_tag(arg)
            return ({'ArrayProperty' if tpl == 'TArray' else 'SetProperty'}, inner and inner[0])
        if tpl == 'TMap': return {'MapProperty'}, None
        return None
    if t in api().struct_cpp: return {'StructProperty'}, api().struct_cpp[t].split('.')[-1]
    if t in api().enum_cpp: return {'ByteProperty', 'EnumProperty'}, None
    return None


def _tag_vs_cpp(t, ctype):
    want = cpp_tag(ctype)
    if want is None: return None
    types, extra = want
    if t['type'] not in types: return 'type %s, the native member is %s' % (t['type'], ctype)
    if t['type'] == 'StructProperty' and extra and t['struct'].lower() != extra.lower():
        return 'struct %s, the native member is %s' % (t['struct'], ctype)
    if t['type'] in ('ArrayProperty', 'SetProperty') and extra and t['inner'] not in extra:
        return 'inner %s, the native member is %s' % (t['inner'], ctype)
    return None


@rule
def tag_names_resolve(pkg):
    """Every tag names a property of the object's class or a class above it, as an FName (case-insensitive): the loader
    finds it by name along PropertyLink (Class.cpp 1331-1406), with no redirects on a cooked load (1296), and a name
    it does not find is skipped without a word, the default silently lost. The property it names is not
    CPF_EditorOnly (skipped silently, 1441-1444) nor CPF_SkipSerialization (skipped with a warning, 1452-1455;
    Property.cpp 853-881). Native members come from UeApi and the object dump; a class UeApi lacks, or a /Game parent
    not found, is skipped, and without the dump so is a name no UeApi member has (UeApi leaves static arrays out)."""
    for i, tl, sc in tagged(pkg):
        if sc is None: _skip('tag_names_resolve', 'class chain unread'); continue
        own, native, top = sc
        for t in tl:
            key = t['name'].lower()
            _judged('tag_names_resolve')
            if key in own:
                p = own[key][1]
                if p.flags & (CPF_EditorOnly | CPF_SkipSerialization):
                    yield i, 'tag %s names a property flagged %#x' % (t['name'], p.flags & (CPF_EditorOnly | CPF_SkipSerialization))
            elif key not in native:
                # UeApi leaves a native class's static arrays out (MovieScene2DTransformSection's Translation[2]): only
                # the object dump lists every member, so without one a native miss is not judged.
                if not api().dumped(): _skip('tag_names_resolve', 'native member unknown without the object dump'); continue
                yield i, 'tag %s names no property of %s or a class above it' % (t['name'], pkg.path(pkg.exports[i]['cls']) or top)


# A tag value's size by type where the engine reads a fixed one (Size is checked against what was read only by
# check(), off in Shipping, where a wrong one desynchronises every later tag: Class.cpp 1491-1499). A bool's value is
# in its tag, Size 0 (PropertyTag.cpp 218-231); an object reference is one FPackageIndex, an interface too
# (PropertyInterface.cpp 134-151); a byte with an enum and an enum property are an FName (PropertyByte.cpp 39-98,
# EnumProperty.cpp 114-170).
TAG_SIZES = {'BoolProperty': 0, 'Int8Property': 1, 'Int16Property': 2, 'UInt16Property': 2, 'IntProperty': 4,
             'UInt32Property': 4, 'FloatProperty': 4, 'Int64Property': 8, 'UInt64Property': 8, 'DoubleProperty': 8,
             'NameProperty': 8, 'ObjectProperty': 4, 'InterfaceProperty': 4, 'EnumProperty': 8}


@rule
def tag_value_sizes(pkg):
    """A tag's Size is its value's: 0 for a bool, 4 for an object or interface reference, 8 for a name or an enum
    value, 1 for a plain byte and 8 for an enum byte, sizeof for a number (see TAG_SIZES). The loader reads that many
    and only check()s Size against it: in Shipping a wrong Size shifts every later tag (Class.cpp 1491-1499)."""
    for i, tl, sc in tagged(pkg):
        for t in tl:
            want = TAG_SIZES.get(t['type'])
            if t['type'] == 'ByteProperty': want = 1 if t['enum'] == 'None' else 8
            if want is not None: _judged('tag_value_sizes')
            if want is not None and t['size'] != want: yield i, 'tag %s: %s Size %d, not %d' % (t['name'], t['type'], t['size'], want)


def _raw_tags(pkg, b, o, end):
    """The tags from o, each (name, type, size, ArrayIndex, HasPropertyGuid, value start, struct / inner name), and where
    the None ends; raises unless every name is one and every type a tag type."""
    names, out = pkg.names, []

    def name(at):
        i, n = struct.unpack_from('<ii', b, at)
        if not 0 <= i < len(names) or n < 0: raise ValueError('not a name')
        return names[i] + ('_%d' % (n - 1) if n else ''), at + 8
    while True:
        if o + 8 > end: raise ValueError('past the end')
        nm, o = name(o)
        if nm == 'None': return out, o
        ty, o = name(o)
        if getid(ty) not in CONCRETE: raise ValueError('not a tag type')
        size, index = struct.unpack_from('<ii', b, o); o += 8
        extra = None
        if ty == 'StructProperty': extra, o = name(o); o += 16
        elif ty == 'BoolProperty': o += 1
        elif ty in ('ByteProperty', 'EnumProperty', 'ArrayProperty', 'SetProperty'): extra, o = name(o)
        elif ty == 'MapProperty': extra, o = name(o); _, o = name(o)
        guid = b[o]; o += 1
        if guid: o += 16
        if size < 0 or o + size > end: raise ValueError('value past the end')
        out.append((nm, ty, size, index, guid, o, extra))
        o += size


def _nested(pkg, b, t):
    """The tag lists inside one tag's value that the engine reads as tags: a struct written as tags, each element of an
    array of structs (after the count and the inner tag). Binary structs fail to parse and are left alone."""
    nm, ty, size, index, guid, at, extra = t
    end = at + size
    if ty == 'StructProperty' and size >= 8:
        try:
            tl, o = _raw_tags(pkg, b, at, end)
            if o == end: yield tl
        except (ValueError, struct.error): pass
    elif ty == 'ArrayProperty' and extra == 'StructProperty' and size >= 4:
        # The count, one inner FPropertyTag (no None after it), then each element's tags (PropertyArray.cpp 200-215).
        try:
            n = struct.unpack_from('<i', b, at)[0]
            head = _one_tag(pkg, b, at + 4)
            if head is None: return
            o, lists = head[1], [[head[0]]]
            for _ in range(n):
                tl, o = _raw_tags(pkg, b, o, end)
                lists.append(tl)
        except (ValueError, struct.error): return
        if o == end: yield from lists


def _one_tag(pkg, b, o):
    """One FPropertyTag header at o (an array of structs' inner tag), and where its value starts."""
    names = pkg.names
    i, n = struct.unpack_from('<ii', b, o)
    if not 0 <= i < len(names): return None
    nm = names[i] + ('_%d' % (n - 1) if n else ''); o += 8
    i, n = struct.unpack_from('<ii', b, o)
    if not 0 <= i < len(names) or names[i] != 'StructProperty': return None
    o += 8
    size, index = struct.unpack_from('<ii', b, o); o += 8
    o += 8 + 16                                         # StructName, StructGuid
    guid = b[o]; o += 1
    if guid: o += 16
    return (nm, 'StructProperty', size, index, guid, o, None), o


@rule
def tag_header_cooked(pkg):
    """Every tag has HasPropertyGuid 0 and an ArrayIndex in [0, ArrayDim) of the property it names. The saver writes a
    property guid only when not cooking and a cooked load never uses one (Class.cpp 1561-1567, 1295; PropertyTag.cpp
    189-204); an index outside [0, ArrayDim) is skipped with a warning, the default lost (Class.cpp 1446-1451). A
    Blueprint property's ArrayDim is 1, and so is every native member UeApi declares (it leaves static arrays out); a
    native static array (StaticMesh's LocalUVDensities[4]) or a tag nested in a struct value is held to >= 0 only."""
    for i, tl, sc in tagged(pkg):
        own, native = (sc[0], sc[1]) if sc else ({}, {})
        b = pkg.blob(i)
        try: raw, _ = _raw_tags(pkg, b, tl.start, len(b))     # the same list, ArrayIndex and HasPropertyGuid kept
        except (ValueError, struct.error): continue
        for t in raw:
            _judged('tag_header_cooked')
            key = t[0].lower()
            dim = own[key][1].dim if key in own else 1 if key in native and native[key][0] else None
            if t[4]: yield i, 'tag %s: HasPropertyGuid %d' % (t[0], t[4])
            if t[3] < 0 or (dim is not None and t[3] >= dim): yield i, 'tag %s: ArrayIndex %d, ArrayDim %s' % (t[0], t[3], dim)
            for nested in _nested(pkg, b, t):
                for n in nested:
                    if n[4]: yield i, 'tag %s.%s: HasPropertyGuid %d' % (t[0], n[0], n[4])
                    if n[3] < 0: yield i, 'tag %s.%s: ArrayIndex %d' % (t[0], n[0], n[3])


# ---- container values, element by element, where every element has a fixed size

def _fixed(p):
    return {'IntProperty': 4, 'FloatProperty': 4, 'ObjectProperty': 4, 'ClassProperty': 4, 'WeakObjectProperty': 4,
            'InterfaceProperty': 4, 'NameProperty': 8, 'Int64Property': 8, 'EnumProperty': 8, 'BoolProperty': 1,
            'UInt32Property': 4, 'DoubleProperty': 8, 'Int8Property': 1, 'Int16Property': 2, 'UInt16Property': 2,
            'UInt64Property': 8, 'ByteProperty': 8 if p.ref else 1}.get(p.type)


def _elem_end(p, b, o):
    """Where one element of property p that starts at o ends: a fixed size, or an FString's length; None otherwise."""
    n = _fixed(p)
    if n is not None: return o + n
    if p.type == 'StrProperty':
        n = struct.unpack_from('<i', b, o)[0]
        return o + 4 + (n if n >= 0 else -2 * n)
    return None


def decodable(p):
    return all(_fixed(s) is not None or s.type == 'StrProperty' for s in p.subs)


def values(t, p):
    """[(Prop, bytes)] of every value a tag of property p holds: the value itself, or each element of an array / set
    (the ones to remove included) / map whose elements are fixed-size or strings. [] when they are not."""
    b = t['value']
    if p.type not in ('ArrayProperty', 'SetProperty', 'MapProperty'): return [(p, b)]
    if not decodable(p): return []
    subs, out, o = p.subs, [], 0
    try:
        rounds = [(False,)] if p.type == 'ArrayProperty' else [(False,), (True,)]
        for (pairs,) in rounds:
            n = struct.unpack_from('<i', b, o)[0]; o += 4
            for _ in range(n):
                e = _elem_end(subs[0], b, o); out.append((subs[0], b[o:e])); o = e
                if p.type == 'MapProperty' and pairs:
                    e = _elem_end(subs[1], b, o); out.append((subs[1], b[o:e])); o = e
    except struct.error: return []
    return out if o == len(b) else []


def frames(t, p):
    """Whether container tag t's value is exactly its counts and its elements for property p (see
    tag_container_frames); p's elements are fixed-size or strings (decodable)."""
    b, o = t['value'], 0
    try:
        for pairs in ((False,) if p.type == 'ArrayProperty' else (False, True)):
            n = struct.unpack_from('<i', b, o)[0]; o += 4
            if n < 0: return False
            for _ in range(n):
                o = _elem_end(p.subs[0], b, o)
                if p.type == 'MapProperty' and pairs: o = _elem_end(p.subs[1], b, o)
                if o > len(b): return False
    except struct.error: return False
    return o == len(b)


@rule
def tag_container_frames(pkg):
    """A container default is its counts and its elements, nothing else: an array int32 Num then the elements
    (PropertyArray.cpp 113-215), a set int32 NumElementsToRemove, those elements, int32 Num, the elements (PropertySet.cpp
    285-358), a map int32 NumKeysToRemove, those keys, int32 NumEntries, the pairs (PropertyMap.cpp 328-400); each element
    as its property's SerializeItem writes it: an object or interface reference one FPackageIndex (PropertyObject.cpp 88,
    PropertyInterface.cpp 134-151), an enum or an enum byte an FName (EnumProperty.cpp 114-170, PropertyByte.cpp),
    a bool one byte. The loader reads exactly that and only check()s it against the tag's Size (Class.cpp 1491-1499): a
    missing count or a wrong element width desynchronises every later tag in Shipping. Judged for the class's own
    containers whose elements are fixed-size or strings."""
    for i, tl, sc in tagged(pkg):
        if sc is None: continue
        own = sc[0]
        for t in tl:
            if t['name'].lower() not in own: continue
            p = own[t['name'].lower()][1]
            if p.type not in ('ArrayProperty', 'SetProperty', 'MapProperty') or t['type'] != p.type or not decodable(p): continue
            _judged('tag_container_frames')
            if not frames(t, p):
                yield i, 'tag %s: %s of %s does not frame as counts and elements: %s' % (
                    t['name'], p.type, '/'.join(s.type for s in p.subs), t['value'][:48].hex())


# ---- enums

def enumerators(pkg, ref):
    """The names of the UEnum an FPackageIndex names: a native one's off UeApi, a UserDefinedEnum's off its package
    (UEnum::Serialize: its tags, the object guid flag, then Names as (FName, int64) pairs). None if unknown."""
    path = pkg.path(ref)
    if ref < 0 and path.startswith('/Script/'):
        return api().enums.get(path)
    r = pkg.resolve(ref)
    if not r: return None
    other, k = r
    b = other.blob(k)
    rd = invariants.Rd(b, other.tags(k).end)
    if rd.i32(): rd.o += 16
    out = []
    for _ in range(rd.i32()):
        i, n = rd.i32(), rd.i32()
        out.append(other.names[i] + ('_%d' % (n - 1) if n else '')); rd.o += 8
    return out


def _bare(n): return n.split('::', 1)[-1].lower()


def enum_value_ok(value, names):
    """'' when value names an entry of the enum as UEnum::GetIndexByName matches it (either spelling, with or without
    the Enum:: prefix, any case: Enum.cpp 464-530), else why not. The _MAX closer counts: it is in Names, the saver
    writes it for that value (IsValidEnumValue, EnumProperty.cpp 160-174) and the game's own cooks carry it
    (Screen_DiscordScreen's LastState = ECommunityUIState_MAX)."""
    if _bare(value) not in [_bare(n) for n in names]: return '%s is not an enumerator' % value
    return ''


@rule
def enum_tag_encoding(pkg):
    """An enum-typed byte carries its enum's name in the tag and an FName value; a plain byte 'None' and one byte
    (PropertyTag.cpp 37-43, PropertyByte.cpp 39-98, 205-232). An EnumProperty's tag names its enum (PropertyTag.cpp 30-36).
    An enum-typed byte tagged 'None' is read as one byte of an FName and the stream desynchronises in Shipping, where
    check(Tag.Size == Loaded) is off (Class.cpp 1491-1499). An array of enums holds 8-byte FNames (EnumProperty.cpp
    114-170)."""
    for i, tl, sc in tagged(pkg):
        if sc is None: continue
        own = sc[0]
        for t in tl:
            if t['name'].lower() not in own: continue
            owner, p = own[t['name'].lower()]
            if p.type in ('ByteProperty', 'EnumProperty') or p.subs: _judged('enum_tag_encoding')
            if p.type == 'ByteProperty' and t['type'] == 'ByteProperty':
                want = owner.obj(p.ref)['name'] if p.ref else 'None'
                if t['enum'] != want: yield i, 'tag %s: EnumName %s, the property\'s enum %s' % (t['name'], t['enum'], want)
            elif p.type == 'EnumProperty' and t['type'] == 'EnumProperty':
                want = owner.obj(p.enum)['name'] if p.enum else None
                if t['enum'] != want: yield i, 'tag %s: EnumName %s, the property\'s enum %s' % (t['name'], t['enum'], want)
            elif p.type in ('ArrayProperty', 'SetProperty', 'MapProperty') and t['type'] == p.type:
                if any(s.type == 'EnumProperty' or (s.type == 'ByteProperty' and s.ref) for s in p.subs) and decodable(p) \
                        and not values(t, p):
                    yield i, 'tag %s: %s of enums does not decode as 8-byte names' % (t['name'], p.type)


@rule
def enum_default_names(pkg):
    """An enum default, and each enum element of a container default, is an FName naming an entry of its enum. The
    loader maps any other name (None included) to the enum's _MAX value with an error log (EnumProperty.cpp 129-170,
    PropertyByte.cpp 81-98), so the default silently becomes out of range; the editor's compiler only warns about an
    invalid one (Editor/KismetCompiler/Private/KismetCompilerMisc.cpp 646-687). The _MAX entry itself is a name the
    saver writes and the game's cooks carry, so it passes. The enum is the named property's (this package's class or a
    parent Blueprint's), else the native enum of the tag's EnumName."""
    for i, tl, sc in tagged(pkg):
        own = sc[0] if sc else {}
        for t in tl:
            key = t['name'].lower()
            if key in own:
                owner, p = own[key]
                if t['type'] != getid(p.type): continue         # tag_types_match's finding
                for q, raw in values(t, p):
                    ref = q.enum if q.type == 'EnumProperty' else q.ref if q.type == 'ByteProperty' else 0
                    if not ref or len(raw) != 8: continue
                    names = enumerators(owner, ref)
                    if names is None: _skip('enum_default_names', 'enum unread'); continue
                    v = _name_at(pkg, raw)
                    _judged('enum_default_names')
                    why = enum_value_ok(v, names)
                    if why: yield i, 'tag %s: %s (%s)' % (t['name'], why, owner.path(ref))
            elif t['type'] in ('ByteProperty', 'EnumProperty') and t['enum'] != 'None' and t['size'] == 8:
                paths = api().enum_by_name.get(t['enum'].lower(), [])
                if len(paths) != 1: _skip('enum_default_names', 'native enum not in UeApi'); continue
                v = _name_at(pkg, t['value'])
                _judged('enum_default_names')
                why = enum_value_ok(v, api().enums[paths[0]])
                if why: yield i, 'tag %s: %s (%s)' % (t['name'], why, paths[0])


def _name_at(pkg, raw):
    i, n = struct.unpack_from('<ii', raw)
    return pkg.names[i] + ('_%d' % (n - 1) if n else '')


# ---- object defaults

@rule
def object_default_classes(pkg):
    """An object default - a tag's value or a container element - is an instance of the property's PropertyClass or a
    subclass, and a class default derives from its MetaClass. The loader nulls an object of any other class with a
    warning, the default silently lost (PropertyBaseObject.cpp 549-595, gate at 583), and loads a class that misses the
    MetaClass unchecked for typed native code to trust (PropertyClass.cpp 98-133). A class chain that cannot be read
    (a /Game package not found, a native class UeApi lacks) is skipped."""
    for i, tl, sc in tagged(pkg):
        if sc is None: continue
        own = sc[0]
        for t in tl:
            if t['name'].lower() not in own: continue
            owner, p = own[t['name'].lower()]
            if t['type'] != getid(p.type): continue             # tag_types_match's finding
            for q, raw in values(t, p):
                if q.type not in ('ObjectProperty', 'ClassProperty') or len(raw) != 4: continue
                v = struct.unpack('<i', raw)[0]
                if not v: continue
                if v < -len(pkg.imports) or v > len(pkg.exports): yield i, 'tag %s: object %d is no import or export' % (t['name'], v); continue
                need = owner.path(q.ref)
                ok = is_child(pkg, class_path_of(pkg, v), need)
                if ok is None: _skip('object_default_classes', 'class chain unread'); continue
                _judged('object_default_classes')
                if not ok: yield i, 'tag %s: %s is a %s, not a %s' % (t['name'], pkg.path(v), class_path_of(pkg, v), need)
                if q.type == 'ClassProperty' and q.meta:
                    meta = owner.path(q.meta)
                    ok = is_child(pkg, pkg.path(v), meta)
                    if ok is None: _skip('object_default_classes', 'class chain unread'); continue
                    if not ok: yield i, 'tag %s: class %s does not derive from %s' % (t['name'], pkg.path(v), meta)
