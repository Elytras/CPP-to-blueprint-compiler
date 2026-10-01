import sys
from invariants import *
# FUNC work package: what the engine relies on in a cooked Blueprint function's flags, its parameters' flags, an
# override's or interface implementation's link to the function it replaces, and which call opcode reaches which callee.
#
# The rules that follow a function into the engine's own classes (an override of AActor::ReceiveTick, a call to
# UGameplayStatics::ApplyDamage) read NATIVE_SDK, a table of every /Script function's EFunctionFlags and parameter block
# and every native class's super, built once from the game's Dumper-7 dump (SDK_DUMP) by _fn_sdk.py. Without it they check the
# Blueprint half only.
import json, os, re, tempfile, struct as _st

NATIVE_SDK = os.path.join(tempfile.gettempdir(), 'invariants_fn_sdk_%s.json' % re.sub(r'\W', '_', os.path.basename(SDK_DUMP or 'none')))

FUNC_Final, FUNC_BlueprintAuthorityOnly, FUNC_BlueprintCosmetic = 0x1, 0x4, 0x8
FUNC_Net, FUNC_NetReliable, FUNC_NetRequest, FUNC_Exec, FUNC_Native = 0x40, 0x80, 0x100, 0x200, 0x400
FUNC_NetResponse, FUNC_Static, FUNC_NetMulticast, FUNC_UbergraphFunction = 0x1000, 0x2000, 0x4000, 0x8000
FUNC_Public, FUNC_Private, FUNC_Protected = 0x20000, 0x40000, 0x80000
FUNC_NetServer, FUNC_HasOutParms, FUNC_HasDefaults, FUNC_NetClient = 0x200000, 0x400000, 0x800000, 0x1000000
FUNC_BlueprintCallable, FUNC_BlueprintEvent, FUNC_BlueprintPure = 0x4000000, 0x8000000, 0x10000000
FUNC_NetFuncFlags = FUNC_Net | FUNC_NetReliable | FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast     # Script.h:157
FUNC_FuncInherit = 0x4C000A0C                                                                              # Script.h:155
FUNC_AccessSpecifiers = FUNC_Public | FUNC_Private | FUNC_Protected
FUNC_Routed = FUNC_NetFuncFlags | FUNC_BlueprintAuthorityOnly | FUNC_BlueprintCosmetic | FUNC_NetRequest | FUNC_NetResponse
CPF_ConstParm, CPF_ReferenceParm = 0x2, 0x8000000
# The parameter flags a caller's call depends on: which properties are parameters, which the VM and ProcessEvent pass
# by address (CPF_OutParm, ScriptCore.cpp:865-900, 1971-1996) and which is the result (CPF_ReturnParm, Class.cpp:5648).
# IsSignatureCompatibleWith also compares CPF_ReferenceParm and a few editor bits (all but
# GetDefaultIgnoredSignatureCompatibilityFlags, Class.h:1914-1921); no runtime path reads those on a parameter.
SIG_FLAGS = CPF_Parm | CPF_OutParm | CPF_ReturnParm

# Native structs with STRUCT_ZeroConstructor, so a local of one gets CPF_ZeroConstructor at link. The flag comes from
# TStructOpsTypeTraits::WithZeroConstructor, false by default (Class.h:691) and set for these in Property.cpp:32-373
# (DateTime and Timespan at 287 and 304), or is DISCOVERED for a TIsPODType struct whose constructor leaves zeroed
# memory zero (UScriptStruct::PrepareCppStructOps, Class.cpp:2632-2658): Margin (Margin.h:191). The game agrees: its
# cooked functions hold a Margin local without FUNC_HasDefaults, which the Kismet compiler does only for a
# CPF_ZeroConstructor local (KismetCompiler.cpp:2330).
ZERO_NATIVE_STRUCTS = {'Vector', 'IntPoint', 'IntVector', 'Vector2D', 'Vector4', 'Plane', 'Rotator', 'Box', 'Box2D',
                       'Matrix', 'BoxSphereBounds', 'LinearColor', 'Color', 'TwoVectors', 'Guid', 'RandomStream',
                       'DateTime', 'Timespan', 'SoftObjectPath', 'SoftClassPath', 'PrimaryAssetType', 'PrimaryAssetId',
                       'Margin'}
# Property types that lack CPF_ZeroConstructor (TIsZeroConstructType is not specialized for FScriptArray / FScriptMap /
# FScriptSet / FSoftObjectPtr) but whose zeroed memory IS their default-constructed state, so an unconstructed local of
# one behaves as a constructed one. The Kismet compiler still sets FUNC_HasDefaults for them; the engine does not need it.
ZERO_STATE_TYPES = {'ArrayProperty', 'MapProperty', 'SetProperty', 'SoftObjectProperty', 'SoftClassProperty'}


# ---- the native table

_NATIVE = []


def native():
    """The NATIVE_SDK table (flags, params, supers, interfaces), or None when it is not there."""
    if not _NATIVE:
        t = None
        if SDK_DUMP and not os.path.exists(NATIVE_SDK):     # built once from the dump (_fn_sdk), then cached
            from invariant_rules import _fn_sdk
            json.dump(_fn_sdk.build(os.path.join(SDK_DUMP, 'SDK', 'SDK')), open(NATIVE_SDK, 'w', encoding='utf-8'))
        if SDK_DUMP and os.path.exists(NATIVE_SDK):
            t = json.load(open(NATIVE_SDK, encoding='utf-8'))
            t['interfaces'] = set(t['interfaces'])
            t['keys'] = {k.lower(): k for k in t['flags']}        # FNames compare case-insensitively
            t['params'] = {k.lower(): v for k, v in t['params'].items()}
        _NATIVE.append(t)
    return _NATIVE[0]


# ---- a function, wherever it lives

class Fn:
    """A UFunction a rule compares against: a Blueprint one (pkg, export index) or a native one (its /Script path)."""
    def __init__(s, pkg=None, i=None, path=None):
        s.pkg, s.i, s.path = pkg, i, path
        if pkg is not None:
            s.st = pkg.struct(i)
            s.flags = s.st.function_flags
            s.name = pkg.exports[i]['name']
            s.where = pkg.path(i + 1)
        else:
            path = native()['keys'][path.lower()]
            s.path = path
            s.flags = native()['flags'][path]
            s.name = path.rsplit(':', 1)[1]
            s.where = path

    def native(s): return s.pkg is None


def fn_at(pkg, idx):
    """The Fn an FPackageIndex names: an export, a /Game import resolved into its package, a /Script import found in
    NATIVE_SDK. None when it cannot be read."""
    if idx == 0: return None
    if idx > 0: return Fn(pkg, idx - 1)
    path = pkg.path(idx)
    if path.startswith('/Script/'):
        t = native()
        return Fn(path=path) if t and path.lower() in t['keys'] else None
    r = pkg.resolve(idx)
    return Fn(r[0], r[1]) if r and r[0].class_of(r[1] + 1) in Package.FUNCTION_CLASSES else None


def class_level(pkg, idx):
    """A class an FPackageIndex names, as one level of a FindFunctionByName walk: ('bp', Package, export index),
    ('native', '/Script/Pkg.Class'), ('unknown', path) when it cannot be read, None for no class."""
    if idx == 0: return None
    path = pkg.path(idx)
    if path.startswith('/Script/'): return ('native', path)
    r = pkg.resolve(idx)
    return ('bp', r[0], r[1]) if r and r[0].struct(r[1]) is not None and hasattr(r[0].struct(r[1]), 'func_map') \
        else ('unknown', path)


def find_function(level, name):
    """UClass::FindFunctionByName with its super (Class.cpp:5281-5323) from `level` up: at each class its own FuncMap,
    then every class its Interfaces list (and theirs, and their supers), then its super. An Fn, None when no class has
    one, 'unknown' when a level on the way cannot be read. A native class's own Interfaces are not in the table: a
    function it gets only from one of those is not seen."""
    while level:
        if level[0] == 'unknown': return 'unknown'
        if level[0] == 'native':
            t = native()
            if t is None: return 'unknown'
            key = (level[1] + ':' + name).lower()
            if key in t['keys']: return Fn(path=key)
            if level[1] not in t['supers']: return 'unknown'
            level = ('native', t['supers'][level[1]]) if t['supers'][level[1]] else None
            continue
        _, p, k = level
        st = p.struct(k)
        fm = {k.lower(): v for k, v in st.func_map}
        if fm.get(name.lower(), 0) > 0: return Fn(p, fm[name.lower()] - 1)
        for c, _o, _k2 in st.interfaces:
            r = find_function(class_level(p, c), name)
            if r: return r
        level = class_level(p, st.super)
    return None


def own_interface_function(pkg, ci, name):
    """The interface function a function of class export ci implements: one of that name in the chain of an interface
    the class itself lists (FBlueprintEditorUtils::FindFunctionInImplementedInterfaces, KismetCompiler.cpp:1847)."""
    for c, _o, _k2 in pkg.struct(ci).interfaces:
        r = find_function(class_level(pkg, c), name)
        if r: return r
    return None


def class_functions(pkg):
    """(class export index, function export index, its Struct) of every ordinary UFunction a class export holds: not
    the ubergraph, not a delegate signature, not a function of an interface class."""
    for ci, cst in classes(pkg):
        if cst.class_flags & 0x4000: continue
        for c in cst.children:
            if c <= 0 or pkg.class_of(c) != 'Function': continue
            st = pkg.struct(c - 1)
            if st.function_flags & FUNC_UbergraphFunction: continue
            yield ci, c - 1, st


def replaced(pkg, ci, fi, st):
    """(the function F replaces, how): its SuperStruct's function ('super'), or when it has none the interface function
    of a class it lists that it implements ('interface'). (None, None) for a new function; (None, 'unknown') when the
    super cannot be read. A static whose super is a static replaces nothing: it only hides that one, as C++ does, and
    every call to either is bound to the one it names (EX_CallMath / EX_FinalFunction), so neither its flags nor its
    parameters reach a caller of the other. Its super is still what FindFunctionByName finds (func_super_link)."""
    if st.super:
        f = fn_at(pkg, st.super)
        if f and f.flags & FUNC_Static and st.function_flags & FUNC_Static: return None, None
        return (f, 'super') if f else (None, 'unknown')
    f = own_interface_function(pkg, ci, pkg.exports[fi]['name'])
    if f == 'unknown': return None, 'unknown'
    return (f, 'interface') if f else (None, None)


# ---- parameters

def parms(f):
    """The leading CPF_Parm properties of a Blueprint Fn, or a native Fn's parameter rows (name, C++ type, offset,
    size, CPF bits) off Dumper-7's params struct - none when it has no struct, a function without parameters."""
    if f.native(): return native()['params'].get(f.path.lower(), [])
    return [p for p in f.st.props if p.flags & CPF_Parm]


def bp_sig(pkg, p):
    """What a caller's layout depends on of a Blueprint parameter, as IsSignatureCompatibleWith compares it
    (Class.cpp:5882-5923, FStructUtils::ArePropertiesTheSame): FProperty class, ArrayDim, the struct / class / enum it
    names, the same of an inner property, and SIG_FLAGS. Not the serialized ElementSize: the loader recomputes it from
    the type at link (the editor that cooked the game wrote FName as 12 bytes, FHitResult as 144)."""
    ref = lambda idx: pkg.path(idx).lower() if idx else None
    return (p.type, p.dim, p.flags & SIG_FLAGS, ref(p.ref), ref(p.meta), ref(p.enum),
            tuple(bp_sig(pkg, q)[:1] + bp_sig(pkg, q)[3:] for q in p.subs))


SIMPLE_KINDS = {'float': 'FloatProperty', 'double': 'DoubleProperty', 'int32': 'IntProperty', 'int64': 'Int64Property',
                'int8': 'Int8Property', 'int16': 'Int16Property', 'uint8': 'ByteProperty', 'uint16': 'UInt16Property',
                'uint32': 'UInt32Property', 'uint64': 'UInt64Property', 'bool': 'BoolProperty',
                'class FName': 'NameProperty', 'class FString': 'StrProperty', 'class FText': 'TextProperty'}
WRAPPERS = {'TSubclassOf': 'ClassProperty', 'TArray': 'ArrayProperty', 'TSet': 'SetProperty', 'TMap': 'MapProperty',
            'TScriptInterface': 'InterfaceProperty', 'TWeakObjectPtr': 'WeakObjectProperty',
            'TLazyObjectPtr': 'LazyObjectProperty', 'TSoftObjectPtr': 'SoftObjectProperty',
            'TSoftClassPtr': 'SoftClassProperty', 'TDelegate': 'DelegateProperty', 'TFieldPath': 'FieldPathProperty',
            'TMulticastInlineDelegate': 'MulticastInlineDelegateProperty',
            'TMulticastSparseDelegate': 'MulticastSparseDelegateProperty'}


def native_kind(ctype):
    """The FProperty classes a Dumper-7 C++ parameter type can be, and the object name of the struct or class it
    names (None where the spelling does not say)."""
    t = ctype.replace('const ', '').strip().rstrip('&').strip()
    if t in SIMPLE_KINDS: return {SIMPLE_KINDS[t]}, None
    m = re.match(r'class (\w+)\*$', t)
    if m: return ({'ClassProperty', 'ObjectProperty'} if m.group(1) == 'UClass' else {'ObjectProperty'}), m.group(1)[1:]
    m = re.match(r'struct (\w+)$', t)
    if m: return {'StructProperty'}, m.group(1)[1:]
    m = re.match(r'(\w+)<', t)
    if m and m.group(1) in WRAPPERS: return {WRAPPERS[m.group(1)]}, None
    if re.match(r'(enum class )?E\w+$', t): return {'ByteProperty', 'EnumProperty'}, None
    return None, None


def sig_mismatch(f, parent):
    """Why Blueprint function f's parameter block differs from `parent`'s, or None when they match."""
    mine = parms(f)
    theirs = parms(parent)
    if len(mine) != len(theirs):
        return '%d parameters, %s has %d' % (len(mine), parent.where, len(theirs))
    for k, (a, b) in enumerate(zip(mine, theirs)):
        if not parent.native():
            if bp_sig(f.pkg, a) != bp_sig(parent.pkg, b):
                return 'parameter %d %s %s is %s %s there' % (k, a.type, a.name, b.type, b.name)
            continue
        name, ctype, _off, size, bits = b
        kinds, obj = native_kind(ctype)
        mask = SIG_FLAGS
        if a.dim != 1 or (kinds and a.type not in kinds) or (a.flags & mask) != (bits & mask):
            return 'parameter %d %s %s flags %#x; %s has %s %s flags %#x' % (
                k, a.type, a.name, a.flags & mask, parent.where, ctype, name, bits & mask)
        if obj and a.ref and a.type in ('StructProperty', 'ObjectProperty') and f.pkg.obj(a.ref)['name'].lower() != obj.lower():
            return 'parameter %d %s names %s; %s has %s' % (k, a.name, f.pkg.path(a.ref), parent.where, ctype)
    return None


# ---- locals the frame must construct

def enum_values(pkg, idx):
    """name (lower case) -> value of the UEnum an FPackageIndex names, off its export: after the tagged properties and
    the object-guid flag, UEnum::Serialize writes TArray<TPair<FName, int64>> (Enum.cpp). None for an enum this cannot
    reach (a native one)."""
    r = pkg.resolve(idx) if idx else None
    if not r: return None
    p, k = r
    b, o = p.blob(k), p.tags(k).end
    o += 4 + (16 if _st.unpack_from('<i', b, o)[0] else 0)
    out = {}
    for _ in range(_st.unpack_from('<i', b, o)[0]):
        ni, nn, v = _st.unpack_from('<iiq', b, o + 4)
        out[(p.names[ni] + ('_%d' % (nn - 1) if nn else '')).lower()] = v
        o += 16
    return out


def tag_zero(pkg, i, t, prop=None):
    """Whether a default-instance tag's value is all zero as the struct's memory would hold it (a UDS's default
    instance is written with every member, UScriptStruct::SerializeItem with no defaults, UserDefinedStruct.cpp:87), or
    None when this cannot tell. `prop` is the member the tag is for, which names an enum's UEnum."""
    ty, v = t['type'], t['value']
    if ty == 'BoolProperty': return not t['bool']
    name = lambda at=0: pkg.names[_st.unpack_from('<i', v, at)[0]] + (
        '_%d' % (_st.unpack_from('<i', v, at + 4)[0] - 1) if _st.unpack_from('<i', v, at + 4)[0] else '')
    if ty == 'NameProperty': return name() == 'None'
    if (ty == 'ByteProperty' and t.get('enum') not in (None, 'None')) or ty == 'EnumProperty':
        values = enum_values(pkg, prop.ref or prop.enum) if prop is not None else None
        return None if values is None or name().lower() not in values else values[name().lower()] == 0
    if ty == 'StrProperty': return _st.unpack_from('<i', v)[0] == 0
    if ty in ('ArrayProperty', 'SetProperty'): return _st.unpack_from('<i', v)[0] == 0
    if ty == 'MapProperty': return _st.unpack_from('<i', v, 4)[0] == 0
    if ty in ('SoftObjectProperty', 'SoftClassProperty'): return name() == 'None' and _st.unpack_from('<i', v, 8)[0] == 0
    if ty == 'TextProperty': return False
    if ty == 'StructProperty' and any(v):
        try: inner = pkg.tags(i, t['at'])
        except Exception: return False                              # a native binary struct with a non-zero byte
        zeros = [tag_zero(pkg, i, u) for u in inner]
        return None if None in zeros else all(zeros)
    return not any(v)


def struct_state(pkg, idx, seen=()):
    """(zero-constructible, needs construction) of the struct an FPackageIndex names. Zero-constructible is
    STRUCT_ZeroConstructor: for a native struct Property.cpp's traits (ZERO_NATIVE_STRUCTS); for a UserDefinedStruct
    what UUserDefinedStruct::UpdateStructFlags computes at load (UserDefinedStruct.cpp:442-473): an all-zero default
    instance and only CPF_ZeroConstructor members. Needs construction: its zeroed memory is not its constructed value.
    None when the struct cannot be read."""
    o = pkg.obj(idx)
    if o is None: return None
    if pkg.path(idx).startswith('/Script/'):
        z = o['name'] in ZERO_NATIVE_STRUCTS
        return z, not z
    r = pkg.resolve(idx)
    if not r or pkg.path(idx) in seen: return None
    p, k = r
    st = p.struct(k)
    if st is None or st.kind != 'UserDefinedStruct': return None
    members = {q.name: q for q in st.props}
    zeros = [tag_zero(p, k, t, members.get(t['name'])) for t in getattr(st, 'defaults', [])]
    if None in zeros: return None
    zero = all(zeros)
    members_zero, members_need = True, False
    for q in st.props:
        s = prop_state(p, q, seen + (pkg.path(idx),))
        if s is None: return None
        members_zero &= s[0]; members_need |= s[1]
    return zero and members_zero, (not zero) or members_need


def prop_state(pkg, p, seen=()):
    """(CPF_ZeroConstructor, needs construction) of a property of this type; None when it cannot be told."""
    if p.type == 'TextProperty': return False, True                    # FText's TextData: a null TSharedRef zeroed
    if p.type in ZERO_STATE_TYPES: return False, False
    if p.type == 'StructProperty': return struct_state(pkg, p.ref, seen)
    return True, False


@rule
def func_locals_constructed(pkg):
    """A function other than the ubergraph with a local that lacks CPF_ZeroConstructor has FUNC_HasDefaults
    (0x800000): an FText, a native struct without STRUCT_ZeroConstructor (not in ZERO_NATIVE_STRUCTS), a
    UserDefinedStruct with a non-zero default or such a member. The Kismet compiler sets the flag for the first such
    local (KismetCompiler.cpp:2328-2335) and every game function keeps it. The engine needs it: the frame is memzeroed
    (ScriptCore.cpp:825-826, 1952-1954) and only FirstPropertyToInit onward is InitializeValue'd (909-916, 2005-2011),
    and UFunction::InitializeDerivedMembers sets FirstPropertyToInit only under FUNC_HasDefaults (Class.cpp:5653-5660).
    So without it an FText local reads through a null TextData, an FTransform or FQuat local is all-zero instead of
    identity, an FHitResult has Time 0 instead of 1 (EngineTypes.h:2074-2078), a UDS local misses its defaults. For some
    other native structs zeroed memory is what their constructor makes (FTimerHandle, FGameplayTag): there the rule is
    the compiler's, stricter than the engine's need. TArray / TMap / TSet / soft pointer locals are left out: zero is
    their constructed state. The ubergraph is left out: its persistent frame is initialised whole by the class."""
    for i, st in functions(pkg):
        if st.function_flags & (FUNC_UbergraphFunction | FUNC_HasDefaults): continue
        for p in st.props:
            if p.flags & CPF_Parm: continue
            s = prop_state(pkg, p)
            if s and s[1]:
                yield i, 'local %s %s%s is not zero-constructible, and FunctionFlags %#x lack FUNC_HasDefaults' % (
                    p.type, p.name, ' (%s)' % pkg.path(p.ref) if p.ref else '', st.function_flags)


@rule
def func_not_native(pkg):
    """No function of a cooked Blueprint has FUNC_Native (0x400): UFunction::Bind looks a native up in the class's
    NativeFunctionLookupTable, which a Blueprint class's is empty, and leaves Func null, so the first call - ProcessEvent,
    CallFunction or ProcessLocalFunction, all through UFunction::Invoke - jumps through a null pointer (Class.cpp:
    5756-5783; ScriptCore.cpp:975-1023, 1143-1147). The Kismet compiler never sets it (KismetCompiler.cpp:1857)."""
    for i, st in functions(pkg):
        if st.function_flags & FUNC_Native: yield i, 'FunctionFlags %#x have FUNC_Native' % st.function_flags


@rule
def func_parm_flags(pkg):
    """A function's parameter flags are ones the VM can use (ObjectMacros.h:373-393): CPF_ReturnParm (0x400) comes with
    CPF_Parm | CPF_OutParm, CPF_OutParm only on a CPF_Parm, a CPF_Parm with CPF_ReferenceParm (0x8000000) is an
    CPF_OutParm, and at most one property is the return value. ProcessEvent lists out parameters by CPF_OutParm
    (ScriptCore.cpp:1968-2003), and UFunction::InitializeDerivedMembers takes ReturnValueOffset from the last
    CPF_ReturnParm while GetReturnProperty takes the first (Class.cpp:5644-5651, 5746-5752): a return value without
    OutParm is never listed, so EX_Return's EX_LocalOutVariable walks off OutParms; two disagree on where the result
    is. A local with OutParm is never destroyed (ScriptCore.cpp:926-933). No runtime path reads CPF_ReferenceParm (only
    C++ export, Property.cpp:719, 811): it marks a parameter declared by reference (ObjectMacros.h:393, 'CPF_OutParam
    and CPF_Param should also be set'), and without CPF_OutParm the VM passes it by copy (ScriptCore.cpp:865-900), so
    the callee's writes never reach the caller. A local may carry CPF_ReferenceParm - the Kismet compiler puts it on
    every array (KismetCompiler.cpp:979-988)."""
    for i, st in functions(pkg):
        rets = [p for p in st.props if p.flags & CPF_ReturnParm]
        if len(rets) > 1: yield i, 'return parameters %s' % [p.name for p in rets]
        for p in st.props:
            if p.flags & CPF_ReturnParm and p.flags & 0x180 != 0x180:
                yield i, 'return value %s flags %#x lack Parm | OutParm' % (p.name, p.flags)
            if p.flags & CPF_OutParm and not p.flags & CPF_Parm:
                yield i, '%s flags %#x: OutParm on a local' % (p.name, p.flags)
            if p.flags & CPF_Parm and p.flags & CPF_ReferenceParm and not p.flags & CPF_OutParm:
                yield i, 'parameter %s flags %#x: ReferenceParm without OutParm, a copy' % (p.name, p.flags)


@rule
def func_has_out_parms(pkg):
    """A function with a CPF_Parm | CPF_OutParm parameter - its return value included - has FUNC_HasOutParms
    (0x400000): ProcessEvent builds Stack.OutParms only under it (ScriptCore.cpp:1968-2003), and every
    EX_LocalOutVariable walks that list with no null check (ScriptCore.cpp:2170-2185), so a native event, a delegate
    broadcast, an interface call or an RPC into it crashes. The Kismet compiler sets it from the properties
    (KismetCompiler.cpp:2318-2321). The flag without an out parameter is harmless and not checked."""
    for i, st in functions(pkg):
        outs = [p.name for p in st.props if p.flags & 0x180 == 0x180]
        if outs and not st.function_flags & FUNC_HasOutParms:
            yield i, 'out parameters %s, FunctionFlags %#x lack FUNC_HasOutParms' % (outs, st.function_flags)


@rule
def func_net_flags_wellformed(pkg):
    """FUNC_Net (0x40) comes with exactly one of FUNC_NetServer / FUNC_NetClient / FUNC_NetMulticast, and without
    FUNC_Net none of those nor FUNC_NetReliable is set. AActor::GetFunctionCallspace routes an RPC by them
    (Actor.cpp:4264-4305, reached from ProcessInternal and CallFunction, ScriptCore.cpp:975-1034, 1158-1188): FUNC_Net
    with no direction runs locally on server and client alike, an RPC never sent; with two it is sent both ways. The
    direction bits without FUNC_Net are ignored there (the 'Not Net' return, 4255-4260), so that half is the editor's
    (K2Node_CustomEvent::GetNetFlags drops them, K2Node_CustomEvent.cpp:251-275; KismetCompiler.cpp:1987) - kept because
    a function given a direction but not FUNC_Net is an RPC that silently runs locally."""
    for i, st in functions(pkg):
        ff = st.function_flags
        dirs = bin(ff & (FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast)).count('1')
        if ff & FUNC_Net and dirs != 1:
            yield i, 'FunctionFlags %#x: FUNC_Net with %d directions' % (ff, dirs)
        if not ff & FUNC_Net and ff & (FUNC_NetReliable | FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast):
            yield i, 'FunctionFlags %#x: net bits without FUNC_Net' % ff


@rule
def func_no_event_graph_call(pkg):
    """A function's serialized EventGraphFunction is null and EventGraphCallOffset 0 (UFunction::Serialize,
    Class.cpp:5694-5714). When set, ProcessEvent skips the function and runs EventGraphFunction at that offset with only
    the int parameter (ScriptCore.cpp:1923-1938): the function's own parameters and body never run. The Kismet
    compiler writes one only for a stub whose ubergraph has no FirstPropertyToInit and no PostConstructLink
    (KismetCompilerVMBackend.cpp:1186-1199), and a Blueprint ubergraph always has a PostConstructLink - every property
    a non-native class owns is linked there (Class.cpp:971-976) and it has its EntryPoint parameter - so no cooked
    Blueprint carries one and the game has none. The rule is that editor fact; it guards a function whose parameters
    or body would be skipped."""
    for i, st in functions(pkg):
        if st.event_graph or st.event_graph_offset:
            yield i, 'EventGraphFunction %s offset %d' % (pkg.path(st.event_graph), st.event_graph_offset)


@rule
def func_super_link(pkg):
    """A function's SuperStruct is what ParentClass->FindFunctionByName(Name) returns: the nearest function of that name
    up the class chain - each class's FuncMap, then its Interfaces, then its super (Class.cpp:5281-5323) - and null
    when there is none. One that implements an interface of this class's own list and overrides nothing has none.
    This is what the Kismet compiler writes (KismetCompiler.cpp:1733-1734, 1774, 1842-1852) and every game function
    keeps. The runtime reads the link for RPCs only: an RPC is routed, sent and received with its topmost function's
    flags and parameter layout (Actor.cpp:4264-4268, NetDriver.cpp:2021-2025, DataReplication.cpp:1222-1226), and is a
    NetField of its own only without one (Class.cpp:4190-4194) - a link to the wrong function sends with that one's
    layout. For any other function the link is invisible at run time; the rule keeps it because the override rules
    below find the function whose flags and parameters must be kept through it."""
    for ci, fi, st in class_functions(pkg):
        name = pkg.exports[fi]['name']
        want = find_function(class_level(pkg, pkg.struct(ci).super), name)
        if want == 'unknown': continue
        if st.super:
            got = pkg.path(st.super)
            if got.rsplit(':', 1)[-1].rsplit('.', 1)[-1].lower() != name.lower():
                yield fi, 'super %s is not a function named %s' % (got, name)
            elif want is not None and got.lower() != want.where.lower():
                yield fi, 'super %s, but the parent chain finds %s' % (got, want.where)
            elif want is None and not got.startswith('/Script/'):   # a native class's own interface is not in the table
                yield fi, 'super %s, but no parent class has %s' % (got, name)
        elif want is not None:
            yield fi, 'no super, but the parent chain has %s' % want.where


@rule
def func_override_flags(pkg):
    """An override or an interface implementation keeps the replaced function's contract: it carries its
    FUNC_FuncInherit | access | BlueprintPure flags (Script.h:155), agrees with it on Exec, Final and Static and on the
    access specifier, and an override (SuperStruct set) has its FUNC_NetFuncFlags exactly. The Kismet compiler copies
    the first and refuses the rest (KismetCompiler.cpp:1855-1868, 2019-2029), and the game keeps all of it. What the
    runtime reads of that: the callspace bits of the function found by name - BlueprintAuthorityOnly / BlueprintCosmetic
    (Actor.cpp:4202, 4249; UEngine::GetGlobalFunctionCallspace) - so dropping one runs it where the parent's contract
    forbade; the net flags, checked against the super at class link (Class.cpp:4190) and routed by the topmost one;
    Exec for console calls. Const, Event, BlueprintCallable and the access specifier are the editor's. What it replaces
    through the parent chain is a FUNC_BlueprintEvent (VerifyValidOverrideFunction, KismetCompiler.cpp:3312) that is
    neither Final nor Static: every caller of a Final or Static function binds it by object (EX_FinalFunction /
    EX_CallMath, KismetCompilerVMBackend.cpp:1222-1224; C++ calls it directly), so an override of one - or of a native
    non-event - splits the behaviour: those callers keep the parent's, by-name callers get this one."""
    for ci, fi, st in class_functions(pkg):
        p, how = replaced(pkg, ci, fi, st)
        if p is None: continue
        f, q = st.function_flags, p.flags
        inherit = q & (FUNC_FuncInherit | FUNC_AccessSpecifiers | FUNC_BlueprintPure)
        if inherit & ~f: yield fi, 'FunctionFlags %#x lack %#x of %s (%#x)' % (f, inherit & ~f, p.where, q)
        if (f ^ q) & (FUNC_Exec | FUNC_Final | FUNC_Static | FUNC_AccessSpecifiers):
            yield fi, 'FunctionFlags %#x and %s %#x differ in Exec / Final / Static / access' % (f, p.where, q)
        if how == 'super' and (f ^ q) & FUNC_NetFuncFlags:
            yield fi, 'net flags %#x, %s has %#x' % (f & FUNC_NetFuncFlags, p.where, q & FUNC_NetFuncFlags)
        if how == 'super' and (not q & FUNC_BlueprintEvent or q & (FUNC_Final | FUNC_Static)):
            yield fi, 'overrides %s (%#x), not an overridable BlueprintEvent' % (p.where, q)


@rule
def func_override_params(pkg):
    """An override or an interface implementation has the replaced function's parameter block: the same CPF_Parm
    properties in the same order, each the same FProperty class and ArrayDim, naming the same struct / class / enum,
    each a parameter, an out parameter and the return value alike (SIG_FLAGS) - IsSignatureCompatibleWith less the
    bits no runtime path reads (KismetCompiler.cpp:1956, 1993-2011; Class.cpp:5882-5923; Class.h:1914-1921). A native caller - ProcessEvent for a BlueprintImplementableEvent, an
    interface's Execute_, a replicated RPC (DataReplication.cpp:1218-1235) - lays out the parameters for the parent
    and ProcessEvent copies ParmsSize bytes into this function's frame at this function's offsets (ScriptCore.cpp:1958,
    1971-1996, 2014-2016): a different block reads garbage and writes outputs to the wrong places."""
    for ci, fi, st in class_functions(pkg):
        p, how = replaced(pkg, ci, fi, st)
        if p is None: continue
        why = sig_mismatch(Fn(pkg, fi), p)
        if why: yield fi, '%s of %s: %s' % ('overriding' if how == 'super' else 'implementing', p.where, why)


INTERFACE_STUB = FUNC_BlueprintEvent | FUNC_BlueprintCallable | FUNC_Public
EFFECTS = {0x0F, 0x14, 0x5F, 0x60, 0x43, 0x44, 0x5C, 0x5D, 0x62, 0x61, 0x63, 0x64, 0x31, 0x39, 0x3B,
           0x1B, 0x1C, 0x45, 0x46, 0x68, 0x06, 0x07, 0x4C, 0x4D, 0x4E, 0x4F}


@rule
def interface_class_stubs(pkg):
    """A Blueprint interface class (CLASS_Interface 0x4000) holds no properties and only stub functions:
    BlueprintEvent | BlueprintCallable | Public (0x0C020000), no SuperStruct, and a body with no effect - no store, no
    call, no jump. That is what the editor compiles for an interface Blueprint (KismetCompiler.cpp:1820-1828,
    3854-3858, 4098-4101) and all of the game's 14 interface functions keep, the 4 with outputs included (their bodies
    are EX_Return and EX_EndOfScript). What the runtime makes of it: an implementer without a function of its own
    reaches the interface's through FindFunctionByName (Class.cpp:5302-5309) and ProcessEvent runs it on the
    implementer, so a body there would run on it; the implementer's own function inherits these flags (KismetCompiler.cpp:1857).
    Every class a class's Interfaces list names is an interface class: UClass::ImplementsInterface walks that list
    (Class.cpp:4649-4668) and FindFunctionByName looks functions up in each class of it."""
    for ci, cst in classes(pkg):
        for c, _o, _k2 in cst.interfaces:
            lv = class_level(pkg, c)
            if lv and lv[0] == 'bp' and not lv[1].struct(lv[2]).class_flags & 0x4000:
                yield ci, 'Interfaces lists %s, not an interface class' % pkg.path(c)
            t = native()
            if lv and lv[0] == 'native' and t and lv[1] in t['supers'] and lv[1] not in t['interfaces']:
                yield ci, 'Interfaces lists %s, not an interface class' % pkg.path(c)
        if not cst.class_flags & 0x4000: continue
        if cst.props: yield ci, 'an interface class with properties %s' % [p.name for p in cst.props]
        for c in cst.children:
            if c <= 0 or pkg.class_of(c) not in Package.FUNCTION_CLASSES: continue
            st = pkg.struct(c - 1)
            if st.function_flags & INTERFACE_STUB != INTERFACE_STUB:
                yield c - 1, 'interface function FunctionFlags %#x lack %#x' % (st.function_flags, INTERFACE_STUB & ~st.function_flags)
            if st.super: yield c - 1, 'interface function with super %s' % pkg.path(st.super)
            ops = sorted({n.op for n in statements(pkg, c - 1)[0]} & EFFECTS)
            if ops: yield c - 1, 'interface stub body does something: ops %s' % ['%02x' % o for o in ops]


def call_target(pkg, i, n, mine):
    """The Fn a call node reaches, where this package and the native table say: by object for EX_FinalFunction /
    EX_LocalFinalFunction / EX_CallMath, by name up the calling function's class chain for a call on self. None where
    it cannot be told (a call by name on another object)."""
    kind, v = n.ops[0][:2]
    if n.op in (0x1C, 0x46, 0x68) and kind == 'obj':
        return fn_at(pkg, v)
    if n.op in (0x1B, 0x45) and kind == 'name' and mine:
        owner = pkg.exports[i]['outer']
        if owner <= 0: return None
        r = find_function(('bp', pkg, owner - 1), v)
        return r if isinstance(r, Fn) else None
    return None


@rule
def call_local_unrouted(pkg):
    """EX_LocalVirtualFunction / EX_LocalFinalFunction (0x45 / 0x46) never reach a function with routing - any of
    FUNC_NetFuncFlags, BlueprintAuthorityOnly, BlueprintCosmetic, NetRequest, NetResponse - and EX_CallMath (0x68) only
    a Static | Final | Native one without those. Those opcodes skip GetFunctionCallspace: the Local ones run
    ProcessLocalFunction (ScriptCore.cpp:1140-1156, 3012-3024), EX_CallMath calls the native thunk on the class default
    object and only checkSlow's the flags ('ProcessContext is the arbiter of net callspace', ScriptCore.cpp:937-958).
    So an RPC reached so runs here and is never sent, an authority-only function runs on a client, a cosmetic one on a
    dedicated server - an engine static included: EX_FinalFunction on a function library's default object would ask
    UEngine::GetGlobalFunctionCallspace, which absorbs it there (BlueprintFunctionLibrary.cpp:20-23,
    UnrealEngine.cpp:14837-14890; CallFunction, ScriptCore.cpp:975-1034; ProcessInternal, 1158-1188). And EX_CallMath
    runs the function pointer with the caller's frame on the default object, which only a static native can take. The
    editor picks the opcodes so (KismetCompilerVMBackend.cpp:1222-1234); a Local call to a non-routed native, which it
    never emits either, is harmless (ProcessLocalFunction invokes it) and not flagged."""
    for i, st in functions(pkg):
        for n, mine in (x for t in pkg.script(i) for x in t.on_self()):
            if n.op not in (0x45, 0x46, 0x68): continue
            f = call_target(pkg, i, n, mine)
            if f is None: continue
            if n.op == 0x68 and (f.flags & 0x2401 != 0x2401 or f.flags & FUNC_Routed):
                yield i, 'EX_CallMath at mem %d to %s, FunctionFlags %#x' % (n.mem, f.where, f.flags)
            if n.op != 0x68 and f.flags & FUNC_Routed:
                yield i, 'op %02x at mem %d to %s, FunctionFlags %#x: not routed' % (n.op, n.mem, f.where, f.flags)


FUNC_RULES = ['func_locals_constructed', 'func_not_native', 'func_parm_flags', 'func_has_out_parms',
              'func_net_flags_wellformed', 'func_no_event_graph_call', 'func_super_link', 'func_override_flags',
              'func_override_params', 'interface_class_stubs', 'call_local_unrouted']
