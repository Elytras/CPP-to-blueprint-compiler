import sys
from invariants import *
"""VMSEM: what the VM does at run time with a value the bytecode hands it - a None context's r-value, a statement's
discarded result, a native's out array, a struct constant's members, a function's constructed locals, an interface
value's slot. Each rule states the invariant and cites the UE 4.27 source that makes it one.

Native (/Script) structs, functions and computed property flags are not in the cooked packages; _vm_sdk.py reads them
off the game's Dumper-7 dump. Without the dump the rules skip native operands and still check the package's own."""
import os as _os
from invariant_rules import _vm_sdk as sdkinfo

CALLS = (0x1B, 0x1C, 0x45, 0x46, 0x68)                 # Virtual, Final, LocalVirtual, LocalFinal, CallMath
CONTEXTS = (0x12, 0x19, 0x1A)                          # ClassContext, Context, Context_FailSilent
LETS = (0x0F, 0x14, 0x43, 0x44, 0x5F, 0x60)            # Let, LetBool, LetMulticastDelegate, LetDelegate, LetObj, LetWeakObjPtr
CPF_Transient, CPF_EditorOnly = 0x2000, 0x800000000
FUNC_HasDefaults = 0x800000


# ---- a property's type, from a cooked FProperty or a Dumper-7 spelling

class Ty:
    """cls: the FProperty class ('IntProperty', 'StructProperty', ...); size: ElementSize * ArrayDim; struct: the
    struct's path for a StructProperty; where: (Package, Prop) for a cooked property, else None."""
    def __init__(s, cls, size, struct=None, where=None): s.cls, s.size, s.struct, s.where = cls, size, struct, where
    def __repr__(s): return '%s%s(%d)' % (s.cls, '<%s>' % s.struct.split('.')[-1] if s.struct else '', s.size)
    def family(s): return 'ByteProperty' if s.cls == 'EnumProperty' else s.cls


def cooked_ty(pkg, p):
    return Ty(p.type, p.elem_size * p.dim, pkg.path(p.ref) if p.type == 'StructProperty' and p.ref else None, (pkg, p))


_SPELL = [('TArray<', 'ArrayProperty'), ('TSet<', 'SetProperty'), ('TMap<', 'MapProperty'), ('TSubclassOf<', 'ClassProperty'),
          ('TScriptInterface<', 'InterfaceProperty'), ('TWeakObjectPtr<', 'WeakObjectProperty'),
          ('TLazyObjectPtr<', 'LazyObjectProperty'), ('TSoftObjectPtr<', 'SoftObjectProperty'),
          ('TSoftClassPtr<', 'SoftClassProperty'), ('TDelegate<', 'DelegateProperty'), ('TFieldPath<', 'FieldPathProperty'),
          ('TMulticastInlineDelegate<', 'MulticastInlineDelegateProperty'),
          ('TMulticastSparseDelegate<', 'MulticastSparseDelegateProperty'), ('TEnumAsByte<', 'ByteProperty')]
_SCALAR = {'int32': 'IntProperty', 'int64': 'Int64Property', 'uint64': 'UInt64Property', 'uint32': 'UInt32Property',
           'int16': 'Int16Property', 'uint16': 'UInt16Property', 'int8': 'Int8Property', 'uint8': 'ByteProperty',
           'float': 'FloatProperty', 'double': 'DoubleProperty', 'bool': 'BoolProperty', 'class FName': 'NameProperty',
           'class FString': 'StrProperty', 'class FText': 'TextProperty'}


def sdk_ty(spelling, size):
    t = ' '.join(spelling.replace('const ', '').replace('&', '').split())
    if t in _SCALAR: return Ty(_SCALAR[t], size)
    for pre, cls in _SPELL:
        if t.startswith(pre): return Ty(cls, size)
    if t.endswith('*'): return Ty('ObjectProperty', size)
    if t.startswith('struct '):
        tab = sdkinfo.table()
        path = _cpp_struct_path().get(t[7:]) if tab else None
        return Ty('StructProperty', size, path or t[7:])
    if t.startswith('E'): return Ty('ByteProperty' if size == 1 else 'EnumProperty', size)
    return Ty('?' + t, size)


_CPP = [None]


def _cpp_struct_path():
    if _CPP[0] is None: _CPP[0] = {v['cpp']: k for k, v in sdkinfo.table()['structs'].items()}
    return _CPP[0]


# What UE computes from the C++ type and a cooked package never carries (FProperty::Serialize masks CPF_ComputedFlags):
# CPF_NoDestructor and CPF_ZeroConstructor per property class, as every dumped property of that kind has them.
_DESTRUCTED = {'StrProperty', 'TextProperty', 'ArrayProperty', 'SetProperty', 'MapProperty', 'SoftObjectProperty',
               'SoftClassProperty', 'FieldPathProperty', 'MulticastInlineDelegateProperty'}
_NOT_ZERO = {'TextProperty', 'SetProperty', 'MapProperty', 'SoftObjectProperty', 'SoftClassProperty',
             'LazyObjectProperty', 'FieldPathProperty'}


def struct_members(pkg, idx, all_props=False):
    """(name, Ty) per element of a struct's PropertyLink, the way UStruct::Link builds it (Class.cpp 944-982:
    TFieldIterator(this) walks the struct's own properties, then each super's); Transient and EditorOnly ones left out
    unless all_props. idx is an FPackageIndex of pkg, or a /Script path. None when neither the packages nor the dump
    have the struct."""
    def keep(flags): return all_props or not (flags & (CPF_Transient | CPF_EditorOnly))
    if isinstance(idx, int) and idx != 0:
        r = pkg.resolve(idx)
        if r:
            other, k = r
            st = other.struct(k)
            if st is None or not hasattr(st, 'struct_flags'): return None
            out = [(p.name, cooked_ty(other, p)) for p in st.props if keep(p.flags) for _ in range(p.dim)]
            return out + (struct_members(other, st.super, all_props) or []) if st.super else out
        idx = pkg.path(idx)
    if not isinstance(idx, str) or not idx.startswith('/Script/'): return None
    link = sdkinfo.struct_link(idx)
    if link is None: return None
    out = []
    for t, name, dim, flags in link:
        if all_props or not ('Transient' in flags or 'EditorOnly' in flags):
            out += [(name, sdk_ty(t, 0))] * dim
    return out


def no_destructor(ty, depth=0):
    """Whether a value of the type is bytes alone, which nothing needs to destroy (CPF_NoDestructor, from
    TIsTriviallyDestructible), or None when that cannot be told. A Blueprint struct is when every member is."""
    if ty.cls in _DESTRUCTED: return False
    if ty.cls.startswith('?'): return None
    if ty.cls != 'StructProperty': return True
    if ty.struct and ty.struct.startswith('/Script/'):
        st = sdkinfo.table() and sdkinfo.table()['structs'].get(ty.struct)
        f = sdkinfo.type_flags('struct ' + st['cpp']) if st else None
        return f[2] if f else None
    if ty.where and depth < 8:
        pkg, p = ty.where
        members = struct_members(pkg, p.ref, all_props=True)
        if members is None: return None
        vals = [no_destructor(t, depth + 1) for _, t in members]
        return False if False in vals else None if None in vals else True
    return None


def zero_constructible(ty, depth=0):
    """CPF_ZeroConstructor of the type (UnrealType.h 1021-1027: TIsZeroConstructType; PropertyStruct.cpp 114-116:
    STRUCT_ZeroConstructor), or None when that cannot be told. A Blueprint struct is zero-constructible only if every
    member is and its default instance is all zero bytes (UserDefinedStruct.cpp 442-473); the default instance is
    not read here, so such a struct only ever answers False (a member is not) or None."""
    if ty.cls in _NOT_ZERO: return False
    if ty.cls.startswith('?'): return None
    if ty.cls != 'StructProperty': return True
    if ty.struct and ty.struct.startswith('/Script/'):
        st = sdkinfo.table() and sdkinfo.table()['structs'].get(ty.struct)
        f = sdkinfo.type_flags('struct ' + st['cpp']) if st else None
        return f[0] if f else None
    if ty.where and depth < 8:
        pkg, p = ty.where
        members = struct_members(pkg, p.ref, all_props=True)
        if members is None: return None
        return False if False in [zero_constructible(t, depth + 1) for _, t in members] else None
    return None


# ---- what a call reaches

class Callee:
    """A called function: its path, flags, and parameters in order (Ty, name, flag words) with the return value last
    or absent. native: a /Script function read off the dump."""
    def __init__(s, path, native, params, ret, flags):
        s.path, s.native, s.params, s.ret, s.flags = path, native, params, ret, flags


def _cooked_callee(pkg, k):
    st = pkg.struct(k)
    if st is None or not hasattr(st, 'function_flags'): return None
    params = [(cooked_ty(pkg, p), p.name, p.flags) for p in st.props if p.flags & CPF_Parm and not p.flags & CPF_ReturnParm]
    ret = next((cooked_ty(pkg, p) for p in st.props if p.flags & CPF_ReturnParm), None)
    return Callee(pkg.path(k + 1), False, params, ret, st.function_flags)


def _native_callee(path, info):
    if info is None: return None
    params, ret = [], None
    for t, name, size, dim, flags in info['params']:
        if 'ReturnParm' in flags: ret = sdk_ty(t, size * dim); ret.no_dtor = 'NoDestructor' in flags
        elif 'Parm' in flags: params.append((sdk_ty(t, size * dim), name, flags))
    return Callee(path, 'Native' in info['flags'], params, ret, info['flags'])


def find_member_function(pkg, cls, name, depth=0):
    """Function `name` of class `cls` (an FPackageIndex of pkg) or of its supers, as a Callee; None if not found."""
    if depth > 16 or not cls: return None
    if cls > 0:
        for k, e in enumerate(pkg.exports):
            if e['name'] == name and e['outer'] == cls and pkg.class_of(k + 1) in Package.FUNCTION_CLASSES:
                return _cooked_callee(pkg, k)
        st = pkg.struct(cls - 1)
        return find_member_function(pkg, getattr(st, 'super', 0), name, depth + 1) if st else None
    r = pkg.resolve(cls)
    if r: return find_member_function(r[0], r[1] + 1, name, depth + 1)
    path = pkg.path(cls)
    if path.startswith('/Script/'):
        p, info = sdkinfo.find_function(path, name)
        return _native_callee(p, info)
    return None


def callee_of(pkg, i, n, on_context=False):
    """The function a call node reaches, or None when it cannot be told here (a virtual call on another object)."""
    kind, v = n.ops[0][:2]
    if n.op in (0x1C, 0x46, 0x68) and kind == 'obj' and v:
        if v > 0: return _cooked_callee(pkg, v - 1)
        r = pkg.resolve(v)
        if r: return _cooked_callee(*r)
        path = pkg.path(v)
        return _native_callee(path, sdkinfo.function(path)) if path.startswith('/Script/') else None
    if n.op in (0x1B, 0x45) and kind == 'name' and not on_context:
        return find_member_function(pkg, pkg.exports[i]['outer'], v)
    return None


def with_parents(nodes, parent=None, ctx=False):
    """(node, parent, index in the parent's kids, whether it runs on a context's object) of every node; a statement's
    parent is None. The member an EX_Context guards (its second operand) runs on the context object (Node.on_self)."""
    for j, n in enumerate(nodes):
        yield n, parent, j, ctx
        yield from _kids_with_parents(n, ctx)


def _kids_with_parents(n, ctx):
    for j, k in enumerate(n.kids):
        yield k, n, j, ctx or (n.op in CONTEXTS and j == 1)
        yield from _kids_with_parents(k, ctx or (n.op in CONTEXTS and j == 1))


def prop_of_path(pkg, segs, owner, depth=0):
    """The Ty of the property a FieldPath (segments, owner FPackageIndex) names, looked up on the owner and its
    supers; None when neither the packages nor the dump have it."""
    if not segs or not owner or depth > 16: return None
    r = pkg.resolve(owner)
    if r:
        other, k = r
        st = other.struct(k)
        p = next((p for p in (st.props if st else []) if p.name == segs[-1]), None)
        if p: return cooked_ty(other, p)
        return prop_of_path(other, segs, st.super, depth + 1) if st and getattr(st, 'super', 0) else None
    path = pkg.path(owner)
    if not path or not path.startswith('/Script/'): return None
    f = sdkinfo.class_field(path, segs[-1])
    if f: return sdk_ty(*f)
    tab = sdkinfo.table()
    while tab and path in tab['structs']:
        for t, name, off, size, dim, flags in tab['structs'][path]['fields']:
            if name == segs[-1]: return sdk_ty(t, size * dim)
        path = tab['structs'][path]['super']
    return None


# ---- the rules

_LITERALS = {'IntProperty': {0x1D, 0x25, 0x26, 0x2C}, 'Int64Property': {0x35, 0x36}, 'UInt64Property': {0x35, 0x36},
             'FloatProperty': {0x1E}, 'ByteProperty': {0x24}, 'EnumProperty': {0x24}, 'BoolProperty': {0x27, 0x28},
             'NameProperty': {0x21}, 'StrProperty': {0x1F, 0x34}, 'TextProperty': {0x29}, 'ObjectProperty': {0x20, 0x2A},
             'ClassProperty': {0x20, 0x2A}, 'InterfaceProperty': {0x2D}, 'SoftObjectProperty': {0x67},
             'SoftClassProperty': {0x67}, 'ArrayProperty': {0x65}, 'SetProperty': {0x3D}, 'MapProperty': {0x3F}}
_ANY_LITERAL = set().union(*_LITERALS.values()) | {0x22, 0x23, 0x2B, 0x2F}
_OWN_CONST = {'/Script/CoreUObject.Vector': 0x23, '/Script/CoreUObject.Rotator': 0x22, '/Script/CoreUObject.Transform': 0x2B}


def _literal_fits(ty, k, pkg):
    """Whether literal node k can be stepped into a member of type ty (None: not a literal, or a type not judged)."""
    if k.op not in _ANY_LITERAL: return None
    if ty.cls == 'StructProperty':
        if not ty.struct: return None
        if k.op == 0x2F: return pkg.path(k.ops[0][1]) == ty.struct or pkg.path(k.ops[0][1]).split('.')[-1] == ty.struct.split('.')[-1]
        return _OWN_CONST.get(ty.struct) == k.op
    ok = _LITERALS.get(ty.cls)
    return None if ok is None else k.op in ok


@rule
def struct_const_members(pkg):
    """EX_StructConst holds exactly one expression per element of every property on the struct's PropertyLink that is
    neither CPF_Transient nor CPF_EditorOnly, in PropertyLink order - the struct's own properties first, then each super
    struct's (UStruct::Link, Class.cpp 944-982, fills PropertyLink from TFieldIterator(this), which walks the struct and
    then GetInheritanceSuper, UnrealType.h 5625-5681). execStructConst steps one expression into each such element in
    that order and then P_FINISH skips exactly one byte for EX_EndStructConst (ScriptCore.cpp 3376-3405); the editor
    writes the same walk (KismetCompilerVMBackend.cpp 866-896). One too many or too few desyncs the stream; one in
    another order lands a literal of one type in a member of another. A literal child is checked against its member's
    type; a non-literal child (a variable, a call) only counts."""
    for i, st in functions(pkg):
        for n in statements(pkg, i)[0]:
            if n.op != 0x2F: continue
            members = struct_members(pkg, n.ops[0][1])
            if members is None: continue
            kids = [k for k in n.kids if k.op != 0x30]
            name = (pkg.path(n.ops[0][1]) or '?').split('.')[-1]
            if len(kids) != len(members):
                yield i, 'StructConst %s at mem %d has %d members, its PropertyLink %d: %s' % (
                    name, n.mem, len(kids), len(members), [m for m, _ in members])
                continue
            for k, (m, ty) in zip(kids, members):
                if _literal_fits(ty, k, pkg) is False:
                    yield i, 'StructConst %s at mem %d: op %02x lands in %s, a %r' % (name, n.mem, k.op, m, ty)
                    break


def discarded_type(pkg, i, t):
    """(callee, its return Ty) when top-level statement t is a call whose result the statement throws away."""
    n, ctx = t, False
    if t.op in CONTEXTS: n, ctx = t.kids[1], True
    if n.op not in CALLS: return None
    c = callee_of(pkg, i, n, ctx)
    return (c, c.ret) if c and c.ret else None


@rule
def discarded_result_fits_scratch(pkg):
    """A call that is a statement of its own - bare, or the member of a statement-level EX_Context - has its return
    value stepped into ProcessLocalScriptFunction's `MS_ALIGN(16) uint8 Buffer[MAX_SIMPLE_RETURN_VALUE_SIZE]`
    (ScriptCore.cpp 1058, 1120; Script.h 40: 64 bytes), raw stack memory nothing constructs or destroys ("No POD struct
    can ever be stored in this buffer"). So the callee's return value must be bytes alone (CPF_NoDestructor: nothing to
    free, and a native thunk's `*(T*)Z_Param__Result = ...` or a script return's copy reads no old value) and at most
    64 bytes: an FString / TArray / FText is assigned over garbage (freeing a garbage pointer) and leaks, a larger
    struct overruns the stack. The editor gives every return pin a term (KismetCompilerMisc.cpp 1841-1871), so a call
    with a return value is always a Let's value there."""
    for i, st in functions(pkg):
        for t in pkg.script(i):
            d = discarded_type(pkg, i, t)
            if not d: continue
            c, ret = d
            bytes_only = ret.no_dtor if hasattr(ret, 'no_dtor') else no_destructor(ret)
            if bytes_only is False or ret.size > 64:
                yield i, 'statement at mem %d discards %s\'s %r return value into the 64-byte scratch buffer' % (
                    t.mem, c.path.split(':')[-1], ret)


def _rvalue(n):
    return next(((segs, owner) for kind, segs, owner in (o for o in n.ops if o[0] == 'prop')), ([], 0))


@rule
def context_rvalue(pkg):
    """An EX_Context / EX_Context_FailSilent / EX_ClassContext carries after its skip count the r-value property that
    ProcessContextOpcode clears when the object is None or pending kill: `if (RESULT_PARAM && RValueProperty)
    RValueProperty->ClearValue(RESULT_PARAM)` (ScriptCore.cpp 2878-2953). Three things follow.
    (1) A statement-level context names none: its RESULT_PARAM is the uninitialised 64-byte statement buffer
    (1058, 1120), and clearing a non-trivial value there frees garbage. The editor passes Statement.LHS's property, null
    for a call with no destination (KismetCompilerVMBackend.cpp 1241-1244).
    (2) A context that is the value of a Let or of EX_Return names one: execLet steps the value straight into the
    destination (ScriptCore.cpp 2647-2679) and EX_Return into the caller's (1123-1128), so with no r-value a None object
    leaves the destination holding its previous value where Blueprint zeroes it. Elsewhere - an argument, a condition,
    another context's object - the slot starts zeroed (a callee's memzeroed frame, a thunk's default-constructed
    local, execJumpIfNot's `bool Value=0`), so an empty r-value is harmless there and not checked.
    (3) A named r-value has the type of what the member yields - the member variable itself, or the call's return
    value - since ClearValue runs that property's clear on RESULT_PARAM (the property class, and the struct for a
    StructProperty; a cooked ElementSize is the editor's, an FName 12 bytes there and 8 in the game, so no size)."""
    for i, st in functions(pkg):
        for c, parent, j, _ in with_parents(pkg.script(i)):
            if c.op not in CONTEXTS: continue
            segs, owner = _rvalue(c)
            if parent is None:
                if segs: yield i, 'statement-level context at mem %d names r-value %s' % (c.mem, '.'.join(segs))
                continue
            if not segs:
                if (parent.op in LETS and j == 1) or parent.op in (0x04, 0x64):
                    yield i, 'context at mem %d is the value of op %02x with no r-value: a None object leaves the ' \
                             'destination stale' % (c.mem, parent.op)
                continue
            have = prop_of_path(pkg, segs, owner)
            inner = c.kids[1]
            if inner.op in (0x01, 0x00, 0x48, 0x6C):
                want = prop_of_path(pkg, inner.ops[0][1], inner.ops[0][2])
            elif inner.op in CALLS:
                callee = callee_of(pkg, i, inner, True)
                want = callee.ret if callee else None
            else:
                want = None
            if have and want and (have.family() != want.family() or have.cls == 'StructProperty' and have.struct and want.struct
                                  and have.struct.split('.')[-1] != want.struct.split('.')[-1]):
                yield i, 'context at mem %d clears r-value %s, a %r, over a %r' % (c.mem, '.'.join(segs), have, want)


def _same(a, b):
    return a.op == b.op and [o[:2] if o[0] == 'int' else o for o in a.ops] == [o[:2] if o[0] == 'int' else o for o in b.ops] \
        and len(a.kids) == len(b.kids) and all(_same(x, y) for x, y in zip(a.kids, b.kids))


def _empties(pkg, i, t):
    """The array expression statement t empties - `EX_SetArray <var> EX_EndArray` with no elements, or a call of
    KismetArrayLibrary's Array_Clear on it - or None when t is neither."""
    if t.op == 0x31: return t.kids[0] if len(t.kids) == 2 and t.kids[1].op == 0x32 else None
    if t.op in (0x1C, 0x68) and t.ops and t.ops[0][0] == 'obj' and t.ops[0][1] < 0 \
            and pkg.path(t.ops[0][1]) == '/Script/Engine.KismetArrayLibrary:Array_Clear':
        args = [k for k in t.kids if k.op != 0x16]
        return args[0] if len(args) == 1 else None
    return None


@rule
def native_out_arrays_emptied(pkg):
    """Before a call to a FUNC_Native function, every TArray parameter that is CPF_Parm | CPF_OutParm and none of
    CPF_ReferenceParm / CPF_ConstParm / CPF_ReturnParm gets its argument emptied by a statement of its own,
    `EX_SetArray <argument> EX_EndArray`, written just ahead of the call's statement (KismetCompilerVMBackend.cpp
    1152-1174: "Array output parameters are cleared, in case the native function doesn't clear them before filling").
    A native written against that contract appends to what the caller's variable already holds - GenericSet_ToArray
    does (BlueprintSetLibrary.cpp 53-70: GenericArray_Add per element, no EmptyValues). An Array_Clear of the same
    variable in that place empties it just as well, so it counts too; what matters is that the array is empty when the
    native runs, not which statement empties it."""
    for i, st in functions(pkg):
        top = pkg.script(i)
        for s, t in enumerate(top):
            for n, on_ctx in t.on_self():
                if n.op not in CALLS: continue
                c = callee_of(pkg, i, n, not on_ctx)
                if not c or not c.native: continue
                args = [k for k in n.kids if k.op != 0x16]
                for (ty, name, flags), arg in zip(c.params, args):
                    if ty.cls != 'ArrayProperty' or 'OutParm' not in flags: continue
                    if any(f in flags for f in ('ReferenceParm', 'ConstParm', 'ReturnParm')): continue
                    q, emptied = s - 1, False
                    while q >= 0 and _empties(pkg, i, top[q]) is not None:
                        if _same(_empties(pkg, i, top[q]), arg): emptied = True
                        q -= 1
                    if not emptied:
                        yield i, 'call to native %s at mem %d: out array %s is not emptied first' % (
                            c.path.split(':')[-1], n.mem, name)


@rule
def locals_constructed_have_defaults(pkg):
    """A function one of whose locals (a property without CPF_Parm) is not CPF_ZeroConstructor has FUNC_HasDefaults: the
    editor sets it exactly then (KismetCompiler.cpp 2328-2335), UFunction::Link finds FirstPropertyToInit only under
    it (Class.cpp 5653-5660), and the VM constructs locals from FirstPropertyToInit on (ScriptCore.cpp 909-916, 2005-2011);
    without it such a local stays memzeroed - an FText with a null TextData, a TMap / TSet whose free list says slot 0,
    an FHitResult with Time 0, an FTransform with a zero quaternion. CPF_ZeroConstructor is computed from the C++ type
    and never saved, so it is judged here from the property's type."""
    for i, st in functions(pkg):
        if st.function_flags & FUNC_HasDefaults: continue
        bad = [p.name for p in st.props if not p.flags & CPF_Parm and zero_constructible(cooked_ty(pkg, p)) is False]
        if bad: yield i, 'FunctionFlags %#x lack HasDefaults, and locals %s are not zero-constructible' % (st.function_flags, bad)


def expr_ty(pkg, e):
    """The Ty of a variable expression (a local, a member, a struct member, a member through a context), or None."""
    if e.op in (0x00, 0x01, 0x48, 0x6C, 0x42): return prop_of_path(pkg, e.ops[0][1], e.ops[0][2])
    if e.op in CONTEXTS: return expr_ty(pkg, e.kids[1])
    return None


def let_dest_ty(pkg, n):
    """The Ty an EX_Let writes: its property operand, or - when that is null, as the editor writes it for an
    object-to-interface cast (KismetCompilerVMBackend.cpp 1486-1489) - the destination expression's variable."""
    segs, owner = (n.ops[0][1], n.ops[0][2]) if n.ops and n.ops[0][0] == 'prop' else ([], 0)
    return prop_of_path(pkg, segs, owner) if segs else expr_ty(pkg, n.kids[0])


def _is_interface_class(pkg, idx):
    if not idx: return False
    r = pkg.resolve(idx)
    if r:
        st = r[0].struct(r[1])
        return bool(st and hasattr(st, 'class_flags') and st.class_flags & 0x4000)
    return sdkinfo.is_interface(pkg.path(idx))


@rule
def interface_value_slot(pkg):
    """An interface value - EX_DynamicCast to a CLASS_Interface class, EX_ObjToInterfaceCast, EX_CrossInterfaceCast -
    is a 16-byte FScriptInterface written through RESULT_PARAM (ScriptCore.cpp 3605-3645: SetObject, then
    SetInterface at 3640; 3593, 3693-3695). So it must be evaluated into a 16-byte interface slot: the value of an EX_Let
    whose property is an InterfaceProperty, an InterfaceProperty parameter, an interface-returning function's EX_Return,
    or the operand of an interface op (EX_InterfaceContext 2195-2198, the interface casts). Into an 8-byte object slot -
    EX_LetObj, an object parameter, a context's object, a JumpIfNot's bool - the second half lands past it."""
    parents = {}
    for i, st in functions(pkg):
        for n, parent, j, ctx in with_parents(pkg.script(i)):
            parents[id(n)] = ctx
            if n.op not in (0x2E, 0x52, 0x54): continue
            if n.op == 0x2E and not _is_interface_class(pkg, n.ops[0][1]): continue
            if parent is None: continue
            where = None
            if parent.op == 0x0F and j == 1:
                ty = let_dest_ty(pkg, parent)
                if ty and ty.cls != 'InterfaceProperty': where = 'a Let into %r' % ty
            elif parent.op == 0x64:
                ty = prop_of_path(pkg, parent.ops[0][1], parent.ops[0][2])
                if ty and ty.cls != 'InterfaceProperty': where = 'a persistent-frame Let into %r' % ty
            elif parent.op in CALLS:
                c = callee_of(pkg, i, parent, parents.get(id(parent), False))
                ty = c.params[j][0] if c and j < len(c.params) else None
                if ty and ty.cls != 'InterfaceProperty': where = 'parameter %s of %s, a %r' % (c.params[j][1], c.path, ty)
            elif parent.op == 0x04:
                ret = next((cooked_ty(pkg, p) for p in st.props if p.flags & CPF_ReturnParm), None)
                if ret and ret.cls != 'InterfaceProperty': where = 'the return value, a %r' % ret
            elif parent.op in (0x51, 0x54, 0x55):
                pass
            else:
                where = 'op %02x (operand %d)' % (parent.op, j)
            if where: yield i, 'interface value (op %02x) at mem %d lands in %s' % (n.op, n.mem, where)


VMSEM_RULES = ['struct_const_members', 'discarded_result_fits_scratch', 'context_rvalue', 'native_out_arrays_emptied',
               'locals_constructed_have_defaults', 'interface_value_slot']
