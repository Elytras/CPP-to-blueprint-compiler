import sys
from invariants import *
"""DELEG: delegates and event dispatchers - signature functions, the multicast opcodes' operands, broadcasts and the
functions bound by name. Each rule cites the UE 4.27 source that makes it one; each was calibrated on the game's own
cooked Blueprints (its FSD/Content) before it was run over the suite.

A delegate's signature is known three ways: on disk (a UFunction of this package, or of a /Game package that
Package.resolve finds), from UeApi (a native dispatcher member or a native function's TDelegate parameter, spelled as
C++ - parameter kinds and counts only, since UeApi drops by-reference-ness), or not at all (a /Script signature
function, whose layout is not on disk). Checks that need what is not known are skipped, never guessed."""
import collections, glob, os, re

STATS = collections.Counter()      # what the rules checked, for the calibration report

FUNC_Delegate = 0x00100000
MULTICAST = ('MulticastDelegateProperty', 'MulticastInlineDelegateProperty', 'MulticastSparseDelegateProperty')
# UFunction::GetDefaultIgnoredSignatureCompatibilityFlags (Class.h 1914-1921) with ObjectMacros.h 362-443's values:
# PersistentInstance, ExportObject, InstancedReference, ContainsInstancedReference, ComputedFlags (IsPlainOldData,
# NoDestructor, ZeroConstructor, HasGetValueTypeHash), ConstParm, UObjectWrapper, NativeAccessSpecifiers,
# AdvancedDisplay, BlueprintVisible, BlueprintReadOnly.
IGNORED_PARM_FLAGS = (0x0002000000000000 | 0x8 | 0x80000 | 0x0000008000000000
                      | 0x40000000 | 0x0000001000000000 | 0x200 | 0x0008000000000000
                      | 0x2 | 0x0004000000000000 | 0x0070000000000000 | 0x0000040000000000 | 0x4 | 0x10)
VARS = (0x00, 0x01, 0x48)                   # EX_LocalVariable, EX_InstanceVariable, EX_LocalOutVariable
CONTEXTS = (0x19, 0x1A, 0x12)               # EX_Context, EX_Context_FailSilent, EX_ClassContext
ADDRESSABLE = VARS + (0x42, 0x6B)           # what leaves Stack.MostRecentPropertyAddress (runscript.ADDRESSABLE)
CALLS = (0x1B, 0x1C, 0x45, 0x46, 0x68)
UEAPI = UEAPI_DIR


# ---- UeApi: the native classes' delegate members and functions, by /Script path

_API = []


def _split(s):
    """A C++ parameter list split at its top-level commas."""
    out, depth, cur = [], 0, ''
    for ch in s:
        depth += ch in '<(' and 1 or ch in '>)' and -1 or 0
        if ch == ',' and depth == 0: out.append(cur.strip()); cur = ''
        else: cur += ch
    return out + [cur.strip()] if cur.strip() else out


def _type_of(param):
    """'class AActor* DestroyedActor' -> 'class AActor*': a UeApi parameter is its type and then its name."""
    m = re.match(r'^(.*?)\s*\b(\w+)$', param.strip(), re.S)
    return m.group(1).strip() if m and m.group(1).strip() else param.strip()


def api():
    """({'/script/pkg.name': class}, {cpp name: key}) off UeApi's headers: each class's UE_CLASS path, C++ super,
    data members (name -> type text) and functions (name -> [parameter type lists], one per overload)."""
    if not _API:
        classes, by_cpp = {}, {}
        for h in glob.glob(os.path.join(UEAPI, '*.h')):
            text = open(h, encoding='utf-8', errors='replace').read()
            for m in re.finditer(r'^class (\w+)(?: : public (\w+))?[^\n;{]*\n\{\n(.*?)^\};', text, re.M | re.S):
                cpp, sup, body = m.groups()
                u = re.search(r'UE_CLASS\("([^"]+)", "([^"]+)"\)', body)
                if not u: continue
                c = dict(cpp=cpp, super=sup, members={}, methods={})
                for line in body.split('\n'):
                    line = line.strip()
                    if not line.endswith(';') or line.startswith(('UE_', '//', '/*', 'public', 'private', 'protected')): continue
                    depth, k = 0, None
                    for j, ch in enumerate(line):
                        if ch == '<': depth += 1
                        elif ch == '>': depth -= 1
                        elif ch == '(' and depth == 0: k = j; break
                    if k is None:
                        mm = re.match(r'^(.*\S)\s+(\w+);$', line)
                        if mm: c['members'][mm.group(2).lower()] = mm.group(1)
                        continue
                    name = re.search(r'(\w+)\s*$', line[:k])
                    d, e = 0, None
                    for j in range(k, len(line)):
                        d += line[j] == '(' and 1 or line[j] == ')' and -1 or 0
                        if d == 0: e = j; break
                    if name and e is not None:
                        c['methods'].setdefault(name.group(1).lower(), []).append(
                            [_type_of(p) for p in _split(line[k + 1:e])])
                key = (u.group(1) + '.' + u.group(2)).lower()
                classes[key] = c
                by_cpp[cpp] = key
        _API[:] = [classes, by_cpp]
    return _API


def api_chain(path):
    """The UeApi classes from the native class at `path` up through its supers ([] if UeApi does not know it)."""
    classes, by_cpp = api()
    c, out = classes.get((path or '').lower()), []
    while c and len(out) < 64:
        out.append(c)
        c = classes.get(by_cpp.get(c['super'])) if c['super'] else None
    return out


def api_delegate_params(t):
    """The parameter types of a UeApi TDelegate / TMulticast*Delegate<R(...)> type text, or None."""
    if not t or '<' not in t or not re.match(r'^(TDelegate|TMulticast\w*Delegate)\b', t): return None
    inner = t[t.index('<') + 1:t.rindex('>')]
    if '(' not in inner: return None
    return [_type_of(p) for p in _split(inner[inner.index('(') + 1:inner.rindex(')')])]


SCALARS = {'bool': 'BoolProperty', 'int': 'IntProperty', 'int32': 'IntProperty', 'float': 'FloatProperty',
           'double': 'DoubleProperty', 'uint8': 'ByteProperty', 'int8': 'Int8Property', 'int16': 'Int16Property',
           'uint16': 'UInt16Property', 'uint32': 'UInt32Property', 'int64': 'Int64Property', 'uint64': 'UInt64Property',
           'FName': 'NameProperty', 'FString': 'StrProperty', 'FText': 'TextProperty'}
TEMPLATES = {'TSubclassOf': 'ClassProperty', 'TSoftObjectPtr': 'SoftObjectProperty', 'TSoftClassPtr': 'SoftClassProperty',
             'TWeakObjectPtr': 'WeakObjectProperty', 'TLazyObjectPtr': 'LazyObjectProperty',
             'TScriptInterface': 'InterfaceProperty', 'TArray': 'ArrayProperty', 'TSet': 'SetProperty',
             'TMap': 'MapProperty', 'TDelegate': 'DelegateProperty', 'TEnumAsByte': 'ByteProperty',
             'TMulticastInlineDelegate': 'MulticastInlineDelegateProperty',
             'TMulticastSparseDelegate': 'MulticastSparseDelegateProperty'}


def api_kind(t):
    """(FProperty classes a UeApi parameter type can be, the name of the object it references or None), or None when
    the spelling is not one this knows. By-reference-ness is dropped: UeApi does not keep it for delegates."""
    t = re.sub(r'^const\s+', '', t.strip()).rstrip('&').strip()
    if t.endswith('*'):
        base = re.sub(r'^(class|struct)\s+', '', t[:-1].strip())
        if base == 'UClass': return {'ClassProperty', 'ObjectProperty'}, None
        return {'ObjectProperty'}, base[1:] if base[:1] in 'AU' else base
    m = re.match(r'^(\w+)\s*<', t)
    if m: return ({TEMPLATES[m.group(1)]}, None) if m.group(1) in TEMPLATES else None
    if t in SCALARS: return {SCALARS[t]}, None
    if re.match(r'^E[A-Z]\w*$', t): return {'ByteProperty', 'EnumProperty'}, t
    if re.match(r'^F[A-Z]\w*$', t): return {'StructProperty'}, t[1:]
    return None


# ---- what is on disk

def _path(P, idx):
    return P.path(idx).lower() if idx else None


def prop_named(pkg, owner, name):
    """The FProperty a field path names: (Package, Prop) when its owner struct is readable - this package, or a /Game
    package Package.resolve finds - ('native', owner path, name) for a /Script owner, None otherwise."""
    if owner < 0:
        r = pkg.resolve(owner)
        if not r:
            path = pkg.path(owner)
            return ('native', path, name) if path and path.startswith('/Script/') else None
        P, k = r
    elif owner > 0: P, k = pkg, owner - 1
    else: return None
    st = P.struct(k)
    p = next((p for p in st.props if p.name.lower() == name.lower()), None) if st else None
    return (P, p) if p else None


def var_of(pkg, n):
    """What a variable expression, or a context around one, names (as prop_named); None for any other expression."""
    if n.op in CONTEXTS: return var_of(pkg, n.kids[1])
    if n.op in VARS:
        _, segs, owner = n.ops[0]
        return prop_named(pkg, owner, segs[-1])
    return None


def is_var(n):
    return n.op in VARS or n.op in CONTEXTS and n.kids[1].op in VARS


def prop_type(where):
    """The FProperty class a prop_named result has: from disk, or from UeApi for a native one; None if unknown."""
    if where is None: return None
    if where[0] != 'native': return where[1].type
    for c in api_chain(where[1]):
        t = c['members'].get(where[2].lower())
        if t is not None:
            k = api_kind(t)
            return sorted(k[0])[0] if k and len(k[0]) == 1 else '?' + t
    return None


def signature(where):
    """The signature a delegate-typed property carries: ('disk', Package, export index), ('api', [parameter types])
    for a native member UeApi spells, or None when it is not known."""
    if where is None: return None
    if where[0] == 'native':
        for c in api_chain(where[1]):
            t = c['members'].get(where[2].lower())
            if t is not None:
                ps = api_delegate_params(t)
                return ('api', ps) if ps is not None else None
        return None
    P, p = where
    if p.ref > 0: return ('disk', P, p.ref - 1)
    r = P.resolve(p.ref) if p.ref < 0 else None
    return ('disk',) + tuple(r) if r else None


def parm_list(P, k):
    """A function's leading run of CPF_Parm properties: what IsSignatureCompatibleWith walks (Class.cpp 5894)."""
    out = []
    for p in P.struct(k).props:
        if not p.flags & CPF_Parm: break
        out.append(p)
    return out


def differ(P, a, Q, b, flags=True):
    """Why FStructUtils::ArePropertiesTheSame(a, b) or the flag test of IsSignatureCompatibleWith fails (Class.cpp
    5799-5832, 5905-5906): class, size, the referenced struct / class / enum / signature (SameType, Property*.cpp), the
    inner properties, flags outside the ignored mask. None when they match."""
    if a.type != b.type: return '%s / %s' % (a.type, b.type)
    if (a.dim, a.elem_size) != (b.dim, b.elem_size): return 'size %d*%d / %d*%d' % (a.dim, a.elem_size, b.dim, b.elem_size)
    for f in ('ref', 'meta', 'enum'):
        if _path(P, getattr(a, f)) != _path(Q, getattr(b, f)):
            return '%s %s / %s' % (f, _path(P, getattr(a, f)), _path(Q, getattr(b, f)))
    if len(a.subs) != len(b.subs): return 'inner properties'
    for x, y in zip(a.subs, b.subs):
        why = differ(P, x, Q, y, False)
        if why: return 'inner ' + why
    if flags and (a.flags ^ b.flags) & ~IGNORED_PARM_FLAGS: return 'flags %#x / %#x' % (a.flags, b.flags)
    return None


def _ref_name(P, p):
    idx = p.enum if p.type == 'EnumProperty' else p.ref
    return P.obj(idx)['name'] if idx else None


def mismatch(fn, sig):
    """Why the function (Package, export index) is not IsSignatureCompatibleWith the signature, or None. Against a
    UeApi signature only the count and each parameter's kind and referenced object's name can be compared."""
    P, k = fn
    got = parm_list(P, k)
    if sig[0] == 'disk':
        want = parm_list(sig[1], sig[2])
        if len(got) != len(want): return '%d parameters, the signature %d' % (len(got), len(want))
        for a, b in zip(got, want):
            why = differ(P, a, sig[1], b)
            if why: return 'parameter %s: %s' % (a.name, why)
        return None
    got = [p for p in got if not p.flags & CPF_ReturnParm]
    if len(got) != len(sig[1]): return '%d parameters, the signature %d (%s)' % (len(got), len(sig[1]), ', '.join(sig[1]))
    for a, t in zip(got, sig[1]):
        kind = api_kind(t)
        if not kind: continue
        if a.type not in kind[0]: return 'parameter %s is a %s, the signature\'s a %s' % (a.name, a.type, t)
        name = _ref_name(P, a)
        if kind[1] and name and name.lower() != kind[1].lower() and a.type != 'ByteProperty':
            return 'parameter %s is a %s of %s, the signature\'s a %s' % (a.name, a.type, name, t)
    return None


def sig_name(sig):
    return sig[1].exports[sig[2]]['name'] if sig[0] == 'disk' else 'void(%s)' % ', '.join(sig[1])


# ---- where FindFunction lands

def find_function(pkg, ci, name, depth=0):
    """What UObject::FindFunction(name) finds on an object of the class export ci: FuncMap, then the interfaces, then
    the super class (UClass::FindFunctionByName, Class.cpp 5281-5320; FName compares ignore case). ('disk', Package,
    export index), ('native', path) for a function UeApi lists, ('missing', path of the class searched last) when the
    whole chain is known and has none, None when part of it cannot be read."""
    P, k, last, unknown = pkg, ci, None, False
    while depth < 32:
        depth += 1
        st = P.struct(k)
        if st is None or not hasattr(st, 'func_map'): return None
        last = P.path(k + 1)
        hit = next((v for n, v in st.func_map if n.lower() == name.lower()), None)
        if hit: return ('disk', P, hit - 1) if hit > 0 else None
        for iface, _, _ in st.interfaces:
            r = _class_at(P, iface)
            if r is None: return None
            got = find_function(r[0], r[1], name, depth) if r[0] != 'native' else _native_function(r[1], name)
            if got is None: unknown = True                  # an interface UeApi does not declare: it may have the name
            elif got[0] != 'missing': return got
        sup = st.super
        if sup == 0: return None if unknown else ('missing', last)
        r = _class_at(P, sup)
        if r is None: return None
        if r[0] == 'native':
            got = _native_function(r[1], name)
            return None if unknown and got and got[0] == 'missing' else got
        P, k = r
    return None


def lookup(where, name):
    """find_function on a class given as (Package, export index) or ('native', /Script path)."""
    return _native_function(where[1], name) if where[0] == 'native' else find_function(where[0], where[1], name)


def static_class(pkg, n):
    """The class an object expression is declared to hold - a variable (or a context's r-value) of an ObjectProperty
    whose PropertyClass is readable: (Package, export index) or ('native', path). None for anything else."""
    w = var_of(pkg, n)
    if not w or w[0] == 'native' or w[1].type != 'ObjectProperty' or not w[1].ref: return None
    return _class_at(w[0], w[1].ref)


def _class_at(P, idx):
    """(Package, export index) of a class an FPackageIndex names, ('native', path) for a /Script one, None else."""
    if idx > 0: return P, idx - 1
    r = P.resolve(idx)
    if r: return r
    path = P.path(idx)
    return ('native', path) if path and path.startswith('/Script/') else None


def _native_function(path, name):
    chain = api_chain(path)
    if not chain: return None
    if any(name.lower() in c['methods'] for c in chain): return ('native', path)
    if chain[-1]['super']: return None                      # the chain leaves what UeApi declares
    return ('missing', path)


def class_of_function(pkg, i):
    """The class export (0-based) a function export is a member of, or None."""
    o = pkg.exports[i]['outer']
    return o - 1 if o > 0 and hasattr(pkg.struct(o - 1) or object(), 'func_map') else None


def tree(n, parent=None, j=None, mine=True):
    """(node, parent, index in parent's kids, runs on this object) for n and everything under it. The r-value of a
    context runs on the context object (ProcessContextOpcode steps it with that object as `this`), where EX_Self and
    EX_InstanceDelegate mean that object; a call's arguments are stepped on the frame's own object again
    (Stack.Step(Stack.Object, ...) in UObject::CallFunction and execCallMulticastDelegate, ScriptCore.cpp 3035-3060)."""
    yield n, parent, j, mine
    for k, kid in enumerate(n.kids):
        yield from tree(kid, n, k, False if n.op in CONTEXTS and k == 1 else True if n.op in CALLS + (0x63,) else mine)


# ---- the rules

@rule
def dispatcher_signature_export(pkg):
    """An event dispatcher of a Blueprint class is a MulticastInlineDelegateProperty whose SignatureFunction is the
    class's own function <Var>__DelegateSignature with FUNC_Delegate.
    Engine requirements: the base MulticastDelegateProperty is a constructible field class (PropertyMulticastDelegate.cpp
    374) whose delegate accessors are PURE_VIRTUAL (UnrealType.h 5051-5062), so the first bind or broadcast through it is
    a fatal error; a sparse one must name a USparseDelegateFunction (CastChecked in UClass::CreateDefaultObject, Class.cpp
    3781, and in every sparse accessor, PropertyMulticastDelegate.cpp 449-593).
    Editor guarantee the game keeps (all its dispatchers): the signature is the class's own export by that name, found
    by name when the class compiles (KismetCompiler.cpp 760-767) and flagged FUNC_Delegate (2034-2046). The VM never
    reads the property's pointer - a broadcast carries its own signature operand - but what reads a dispatcher by
    reflection does: SameType (PropertyMulticastDelegate.cpp 343-346), delegate ImportText (300), an editor child
    Blueprint's Bind / Call nodes. A cooked game has no by-name fix-up (the one for signatures, UObjectGlobals.cpp 213,
    is skipped on cooked platforms), so the serialized pointer (315-318) is the only link."""
    for ci, st in classes(pkg):
        for p in st.props:
            if p.type not in MULTICAST: continue
            STATS['dispatcher props'] += 1
            if p.type == 'MulticastDelegateProperty':
                yield ci, '%s is the abstract MulticastDelegateProperty' % p.name
            if p.ref == 0:
                yield ci, 'dispatcher %s has no SignatureFunction' % p.name; continue
            kind = pkg.class_of(p.ref)
            if p.type == 'MulticastSparseDelegateProperty' and kind != 'SparseDelegateFunction':
                yield ci, 'sparse dispatcher %s names a %s, not a SparseDelegateFunction' % (p.name, kind)
            if p.ref < 0 or kind not in Package.FUNCTION_CLASSES:
                yield ci, 'dispatcher %s names %s (%s), not a function of its class' % (p.name, pkg.path(p.ref), kind)
                continue
            e = pkg.exports[p.ref - 1]
            if e['outer'] != ci + 1 or e['name'].lower() != (p.name + '__DelegateSignature').lower():
                yield ci, 'dispatcher %s names %s' % (p.name, pkg.path(p.ref))
            sig = pkg.struct(p.ref - 1)
            if not sig.function_flags & FUNC_Delegate:
                yield ci, 'dispatcher %s\'s signature %s has FunctionFlags %#x, no FUNC_Delegate' % (
                    p.name, e['name'], sig.function_flags)


def _delegate_props(pkg):
    """(export, Prop) of every delegate-typed property: class variables, parameters and locals, container inners."""
    def inner(p):
        yield p
        for s in p.subs: yield from inner(s)
    for i in range(len(pkg.exports)):
        st = pkg.struct(i)
        if not st: continue
        for p in st.props:
            for q in inner(p):
                if q.type == 'DelegateProperty' or q.type in MULTICAST: yield i, q


@rule
def delegate_property_signature(pkg):
    """Every (Multicast)DelegateProperty - a variable, a parameter, a local, a container's inner - names its
    SignatureFunction: a Function / DelegateFunction / SparseDelegateFunction called *__DelegateSignature with
    FUNC_Delegate (PropertyDelegate.cpp 161-175 and PropertyMulticastDelegate.cpp 315-318 serialize it unchecked).
    Engine requirements, wherever the property sits: a MulticastSparseDelegateProperty's is a SparseDelegateFunction,
    CastChecked by every sparse accessor (PropertyMulticastDelegate.cpp 449-593); the abstract
    MulticastDelegateProperty's accessors are PURE_VIRTUAL, fatal on first use (UnrealType.h 5051-5062).
    Editor / UHT guarantee the game keeps (all its delegate-typed properties): the signature exists, carries the
    __DelegateSignature suffix and FUNC_Delegate (KismetCompiler.cpp 764, 2036). What reads it is reflection, not the
    VM: SameType compares delegate properties by it (PropertyDelegate.cpp 188-191, so a function taking this delegate
    only matches a signature naming the same one), ImportText resolves a bound name against it; a cooked game never
    finds it by name (UObjectGlobals.cpp 213)."""
    for i, p in _delegate_props(pkg):
        STATS['delegate-typed props'] += 1
        if p.type == 'MulticastDelegateProperty':
            yield i, '%s is the abstract MulticastDelegateProperty' % p.name
        if p.type == 'MulticastSparseDelegateProperty' and p.ref and pkg.class_of(p.ref) != 'SparseDelegateFunction':
            yield i, 'sparse %s names a %s, not a SparseDelegateFunction' % (p.name, pkg.class_of(p.ref))
        if p.ref == 0:
            yield i, '%s %s has no SignatureFunction' % (p.type, p.name); continue
        o, kind = pkg.obj(p.ref), pkg.class_of(p.ref)
        if kind not in Package.FUNCTION_CLASSES or not o['name'].endswith('__DelegateSignature'):
            yield i, '%s %s names %s (%s)' % (p.type, p.name, pkg.path(p.ref), kind); continue
        r = pkg.resolve(p.ref)
        if r:
            st = r[0].struct(r[1])
            STATS['delegate prop signatures read'] += 1
            if st and not st.function_flags & FUNC_Delegate:
                yield i, '%s %s: %s has FunctionFlags %#x, no FUNC_Delegate' % (p.type, p.name, o['name'], st.function_flags)


def _function_flags(pkg, i, n):
    """(name, FunctionFlags or None) of what a call node or an EX_CallMulticastDelegate names."""
    kind, v = n.ops[0][:2]
    if kind == 'name':
        f = callee(pkg, i, n)
        return v, f.function_flags if f else None
    if not v: return None, None
    r = pkg.resolve(v)
    st = r[0].struct(r[1]) if r else None
    return pkg.obj(v)['name'], st.function_flags if st and hasattr(st, 'function_flags') else None


@rule
def delegate_signature_not_called(pkg):
    """A delegate signature function is only the operand of EX_CallMulticastDelegate, and that operand is one.
    Editor guarantee the game keeps, not a VM check: the backend asserts FUNC_Delegate on a broadcast's operand
    (KismetCompilerVMBackend.cpp 1334) and asserts false for any call to a FUNC_Delegate function (1248-1252). It still
    guards something real: called directly, a signature's stub body just returns, so the call silently broadcasts
    nothing; and execCallMulticastDelegate sizes and steps the broadcast's parameters by its operand, which the bound
    handlers then read as their own (ScriptCore.cpp 3032-3063), so the operand must be a delegate signature."""
    for i, st in functions(pkg):
        for n in statements(pkg, i)[0]:
            if n.op in CALLS:
                name, flags = _function_flags(pkg, i, n)
                STATS['calls checked' if flags is not None else 'calls by name only'] += 1
                if (flags is not None and flags & FUNC_Delegate) or (name or '').endswith('__DelegateSignature'):
                    yield i, 'op %02x at mem %d calls the delegate signature %s' % (n.op, n.mem, name)
            elif n.op == 0x63:
                name, flags = _function_flags(pkg, i, n)
                kind = pkg.class_of(n.ops[0][1]) if n.ops[0][1] else None
                if kind not in Package.FUNCTION_CLASSES or not (name or '').endswith('__DelegateSignature') \
                        or (flags is not None and not flags & FUNC_Delegate):
                    yield i, 'EX_CallMulticastDelegate at mem %d names %s (%s, flags %s)' % (
                        n.mem, name, kind, None if flags is None else hex(flags))


@rule
def multicast_operand_is_dispatcher(pkg):
    """The delegate operand of EX_AddMulticastDelegate, EX_RemoveMulticastDelegate, EX_ClearMulticastDelegate,
    EX_LetMulticastDelegate (its left side) and EX_CallMulticastDelegate is a variable - itself or as a context's
    r-value - of a multicast delegate property: the VM steps it for Stack.MostRecentProperty / Address and casts that
    to FMulticastDelegateProperty unchecked in a shipping build (ScriptCore.cpp 3035-3037, 3090-3100, 3110-3120,
    3130-3137; Field.h 880-890 CastFieldCheckedNullAllowed). Any other expression leaves no property, so the operation
    silently does nothing; another property type is called through the wrong vtable. The delegate Add / Remove take is
    stepped into a 16-byte FScriptDelegate (3095-3096, 3115-3116): a variable of another property type copies its own
    size there - an engine requirement. That it is EX_InstanceDelegate or a delegate variable, never another
    expression, is what the editor emits (KismetCompilerVMBackend.cpp 1589-1607; the game keeps it)."""
    for i, st in functions(pkg):
        for n in statements(pkg, i)[0]:
            if n.op not in (0x5C, 0x62, 0x5D, 0x43, 0x63): continue
            d = n.kids[0]
            STATS['multicast ops'] += 1
            if not is_var(d):
                yield i, 'op %02x at mem %d: its dispatcher is op %02x, not a variable' % (n.op, n.mem, d.op); continue
            t = prop_type(var_of(pkg, d))
            STATS['multicast operand typed' if t else 'multicast operand untyped'] += 1
            if t is not None and t not in MULTICAST:
                yield i, 'op %02x at mem %d: its dispatcher %s is a %s' % (n.op, n.mem, d.kids[1].ops[0][1][-1]
                                                                          if d.op in CONTEXTS else d.ops[0][1][-1], t)
            if n.op in (0x5C, 0x62):
                v = n.kids[1]
                if v.op == 0x4B: continue
                if not is_var(v):
                    yield i, 'op %02x at mem %d adds / removes op %02x, not a delegate' % (n.op, n.mem, v.op); continue
                t = prop_type(var_of(pkg, v))
                if t is not None and t != 'DelegateProperty':
                    yield i, 'op %02x at mem %d adds / removes a %s' % (n.op, n.mem, t)


@rule
def broadcast_matches_signature(pkg):
    """EX_CallMulticastDelegate <signature> <dispatcher> <arguments> EX_EndFunctionParms: the signature is the
    dispatcher property's own SignatureFunction, and there is one argument per parameter of it, a parameter passed by
    reference getting a variable. execCallMulticastDelegate allocates the signature's ParmsSize, then walks its
    parameter chain once per argument until EX_EndFunctionParms, unchecked (ScriptCore.cpp 3040-3063): one argument
    too many walks off the chain into a null property; a missing one leaves its parameter zeroed, never initialized.
    For a CPF_OutParm it steps the argument with no result buffer and copies from Stack.MostRecentPropertyAddress
    (3045-3053): a literal there writes through a null result pointer, and an expression that leaves no address loses
    the argument. The
    bound handlers then read the buffer as their own parameters (ScriptDelegates.h 479-502, 231-248). That the operand
    is the property's own signature, not merely a layout-identical one, is what the editor emits (one term per
    parameter, KismetCompilerVMBackend.cpp 1339-1356); the game keeps it."""
    for i, st in functions(pkg):
        for n in statements(pkg, i)[0]:
            if n.op != 0x63: continue
            op_sig = n.ops[0][1]
            r = pkg.resolve(op_sig) if op_sig else None
            where = var_of(pkg, n.kids[0])
            prop_sig = signature(where)
            if where and where[0] != 'native' and where[1].ref and r and prop_sig and prop_sig[0] == 'disk' \
                    and _path(prop_sig[1], prop_sig[2] + 1) != _path(pkg, op_sig):
                yield i, 'EX_CallMulticastDelegate at mem %d names %s, its dispatcher\'s signature is %s' % (
                    n.mem, pkg.path(op_sig), prop_sig[1].path(prop_sig[2] + 1))
            args = [k for k in n.kids[1:] if k.op != 0x16]
            sig = ('disk',) + tuple(r) if r else prop_sig
            STATS['broadcasts, signature ' + (sig[0] if sig else 'unknown')] += 1
            if sig is None: continue
            if sig[0] == 'disk':
                want = [p for p in parm_list(sig[1], sig[2]) if not p.flags & CPF_ReturnParm]
                if len(args) != len(want):
                    yield i, 'EX_CallMulticastDelegate at mem %d passes %d arguments to %s\'s %d parameters' % (
                        n.mem, len(args), sig_name(sig), len(want))
                for p, a in zip(want, args):
                    if p.flags & CPF_OutParm and a.op not in ADDRESSABLE and not (a.op in CONTEXTS and a.kids[1].op in ADDRESSABLE):
                        yield i, 'EX_CallMulticastDelegate at mem %d: reference parameter %s gets op %02x, not a variable' % (
                            n.mem, p.name, a.op)
            elif len(args) != len(sig[1]):
                yield i, 'EX_CallMulticastDelegate at mem %d passes %d arguments to %s' % (n.mem, len(args), sig_name(sig))


def _callee_param_sig(pkg, i, n, j):
    """The signature of parameter j of the function a call node names, when that parameter is a delegate."""
    kind, v = n.ops[0][:2]
    if kind == 'name':
        f = callee(pkg, i, n)
        if f is None: return None
        ps = [p for p in f.props if p.flags & CPF_Parm and not p.flags & CPF_ReturnParm]
        return signature((pkg, ps[j])) if j < len(ps) and ps[j].type == 'DelegateProperty' else None
    if not v: return None
    r = pkg.resolve(v)
    if r:
        f = r[0].struct(r[1])
        ps = [p for p in f.props if p.flags & CPF_Parm and not p.flags & CPF_ReturnParm] if f else []
        return signature((r[0], ps[j])) if j < len(ps) and ps[j].type == 'DelegateProperty' else None
    path = pkg.path(v)
    if not path or not path.startswith('/Script/') or ':' not in path: return None
    cls, name = path.rsplit(':', 1)
    nargs = sum(1 for k in n.kids if k.op != 0x16)
    for c in api_chain(cls):
        for params in c['methods'].get(name.lower(), []):
            if len(params) == nargs and j < len(params):
                ps = api_delegate_params(params[j])
                return ('api', ps) if ps is not None else None
    return None


def _sinks(pkg, i, top):
    """Where a delegate value meets a signature in function i: (value node, signature or None, what, bind node).
    EX_BindDelegate binds its name into its variable; EX_LetDelegate / EX_Let store a value into a variable;
    Add / Remove hand one to a dispatcher; a call passes one as a TDelegate argument."""
    for t in top:
        for n, parent, j, mine in tree(t):
            if n.op == 0x61:
                yield n, signature(var_of(pkg, n.kids[0])), 'BindDelegate into its variable', mine
            elif n.op in (0x44, 0x0F) and len(n.kids) == 2 and (n.kids[1].op == 0x4B or is_var(n.kids[1])):
                w = var_of(pkg, n.kids[0])
                if prop_type(w) == 'DelegateProperty': yield n.kids[1], signature(w), 'a delegate variable', mine
            elif n.op in (0x5C, 0x62):
                yield n.kids[1], signature(var_of(pkg, n.kids[0])), 'the dispatcher', mine
            elif n.op in CALLS:
                args = [k for k in n.kids if k.op != 0x16]
                for a_i, a in enumerate(args):
                    if a.op == 0x4B or (is_var(a) and prop_type(var_of(pkg, a)) == 'DelegateProperty'):
                        yield a, _callee_param_sig(pkg, i, n, a_i), 'parameter %d of the call' % a_i, mine


@rule
def delegate_bind_matches_signature(pkg):
    """A function bound by name - EX_InstanceDelegate (the object running it), EX_BindDelegate (its object operand) -
    is found by FindFunction on that object's class chain, and its parameter chain is IsSignatureCompatibleWith the
    signature of the delegate it ends up in (the delegate variable, the dispatcher it is added to, the TDelegate
    parameter it is passed to). Neither opcode checks the name (ScriptCore.cpp 3295-3321); a broadcast skips a name
    FindFunction misses, silently (ScriptDelegates.h 38-49, 479-502), and runs a found one by ProcessEvent over the
    signature's parameter buffer (231-248; ScriptCore.cpp 3040, 1958), so a different layout reads and writes the wrong
    bytes. The layout is the engine requirement; the flag comparison on top of it (IsSignatureCompatibleWith, Class.cpp
    5882-5923, with the ignored flags of Class.h 1914-1921) is the editor's test (K2Node_CreateDelegate.cpp 137-164),
    which the game keeps. An EX_BindDelegate object other than Self is checked on the class its variable declares: a
    function found there has that signature in any subclass too, but a name missing there may be a subclass's, so
    only Self's chain can prove a name missing."""
    for i, st in functions(pkg):
        ci = class_of_function(pkg, i)
        if ci is None: continue
        me = (pkg, ci)

        def target(obj, mine):
            """(class FindFunction runs on, whether that is the whole chain) for a BindDelegate's object, or None."""
            if obj.op == 0x17: return (me, True) if mine else None
            c = static_class(pkg, obj)
            STATS['BindDelegate object ' + ('Self' if obj.op == 0x17 else 'typed variable' if c else 'op %02x' % obj.op)] += 1
            return (c, False) if c else None
        top = pkg.script(i)
        bound = {}                                         # a delegate variable -> the (name, class, exact) bound into it here
        for t in top:
            for n, _, _, mine in tree(t):
                if n.op == 0x61 and is_var(n.kids[0]):
                    on = target(n.kids[1], mine)
                    if on: bound.setdefault(repr(var_of(pkg, n.kids[0])), []).append((n.ops[0][1],) + on)
                if n.op in (0x44, 0x0F) and len(n.kids) == 2 and n.kids[1].op == 0x4B and mine and is_var(n.kids[0]):
                    bound.setdefault(repr(var_of(pkg, n.kids[0])), []).append((n.kids[1].ops[0][1], me, True))
        seen = set()
        for v, sig, what, mine in _sinks(pkg, i, top):
            if v.op == 0x61:
                on = target(v.kids[1], mine)
                names = [(v.ops[0][1],) + on] if on else []
            elif v.op == 0x4B:
                names = [(v.ops[0][1], me, True)] if mine else []
            else:
                w = var_of(pkg, v)
                names = bound.get(repr(w), [])
                own = signature(w)
                if sig and own and sig[0] == own[0] == 'disk' and (own[1], own[2]) != (sig[1], sig[2]):
                    STATS['delegate value vs sink signature compared'] += 1
                    why = mismatch((own[1], own[2]), sig)
                    if why: yield i, 'a %s value goes to %s %s: %s' % (sig_name(own), what, sig_name(sig), why)
            for name, where, exact in names:
                found = lookup(where, name)
                STATS['bound names found ' + (found[0] if found else 'unknown') + ('' if exact else ' (declared class)')] += 1
                key = (name, what, repr(sig), repr(where))
                if key in seen: continue
                seen.add(key)
                if found is None: continue
                if found[0] == 'missing':
                    if exact:
                        yield i, 'binds %s, which FindFunction does not find on %s or its supers' % (name, pkg.path(ci + 1))
                    continue
                if found[0] != 'disk' or sig is None: continue
                STATS['bound functions compared to a %s signature' % sig[0]] += 1
                why = mismatch((found[1], found[2]), sig)
                if why: yield i, 'binds %s to %s %s: %s' % (name, what, sig_name(sig), why)


TIMER_BY_NAME = 'K2_SetTimer'


def _string_const(pkg, i, n):
    """The value of an EX_StringConst / EX_UnicodeStringConst node, read off the export's bytes; None for another op."""
    b, o = pkg.blob(i), n.disk + 1
    if n.op == 0x1F: return b[o:b.index(b'\0', o)].decode('latin-1')
    if n.op == 0x34:
        e = o
        while b[e] or b[e + 1]: e += 2
        return b[o:e].decode('utf-16-le')
    return None


@rule
def timer_by_name_resolves(pkg):
    """K2_SetTimer(Self, "<Name>", ...) with a constant name: FindFunction finds <Name> on the object's class chain and
    it takes no parameters. K2_SetTimer looks it up and refuses one with ParmsSize > 0 (KismetSystemLibrary.cpp
    449-463); K2_SetTimerDelegate sets no timer for a name IsBound cannot find (474, 492-497). Either way it only logs
    a warning and returns an invalid handle: the timer silently never fires."""
    for i, st in functions(pkg):
        ci = class_of_function(pkg, i)
        if ci is None: continue
        for t in pkg.script(i):
            for n, _, _, mine in tree(t):
                if n.op not in (0x1C, 0x46, 0x68) or not n.ops[0][1] or n.ops[0][0] != 'obj': continue
                if pkg.obj(n.ops[0][1])['name'] != TIMER_BY_NAME: continue
                args = [k for k in n.kids if k.op != 0x16]
                if len(args) < 2 or args[0].op != 0x17: continue
                name = _string_const(pkg, i, args[1])
                if name is None: continue
                found = find_function(pkg, ci, name)
                STATS['K2_SetTimer names found ' + (found[0] if found else 'unknown')] += 1
                if found and found[0] == 'missing':
                    yield i, 'K2_SetTimer names %s, which FindFunction does not find on %s' % (name, pkg.path(ci + 1))
                elif found and found[0] == 'disk' and parm_list(found[1], found[2]):
                    yield i, 'K2_SetTimer names %s, which takes parameters' % name
