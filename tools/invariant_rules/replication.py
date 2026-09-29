import sys
from invariants import *

# Replication and RPC well-formedness: what UE 4.27's replication layer (RepLayout, UClass::SetUpRuntimeReplicationData,
# AActor::GetFunctionCallspace) reads off a Blueprint class's properties and functions. Flag values: Script.h 116-157,
# ObjectMacros.h 371, 397-398 (CPF_Net 0x20, CPF_RepSkip 0x80000000, CPF_RepNotify 0x100000000); conditions:
# CoreNetTypes.h 12-27. Where a rule asks for more than the runtime needs, its docstring says which part is only what
# the editor (or UnrealHeaderTool) always writes: the game's own cooked packages keep those parts too (calibrated), and
# each is kept because breaking it means the compiler put a replication marker where it does nothing.

CPF_Net, CPF_RepSkip, CPF_RepNotify = 0x20, 0x80000000, 0x100000000
FUNC_Net, FUNC_NetReliable, FUNC_Static = 0x40, 0x80, 0x2000
FUNC_NetServer, FUNC_NetClient, FUNC_NetMulticast = 0x200000, 0x1000000, 0x4000
FUNC_NetDirections = FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast
FUNC_NetFuncFlags = FUNC_Net | FUNC_NetReliable | FUNC_NetDirections          # Script.h 157
COND_DEFINED = set(range(14)) | {15}                                            # CoreNetTypes.h 12-26: no 14, Max = 16
UNREPLICABLE = ('MapProperty', 'SetProperty', 'DelegateProperty', 'MulticastDelegateProperty',
                'MulticastInlineDelegateProperty', 'MulticastSparseDelegateProperty', 'InterfaceProperty')


def _i32(tag):
    return struct.unpack_from('<i', tag['value'])[0] if tag and len(tag['value']) >= 4 else 0


def _all_props(props, depth=0):
    """Each property and, under it, its container inners (an array's inner, a map's key and value), which are FProperties
    serialized with the same header (FProperty::Serialize, Property.cpp 579-588)."""
    for p in props:
        yield p, depth
        yield from _all_props(p.subs, depth + 1)


def _in_func_map(pkg, ci, name):
    """('found', package, export index) when class export ci's own FuncMap names `name` (an FName, so case-blind)."""
    st = pkg.struct(ci)
    for n, idx in getattr(st, 'func_map', ()):
        if n.lower() == name.lower():
            if idx > 0: return ('found', pkg, idx - 1)
            r = pkg.resolve(idx)
            return ('found',) + r if r else ('lost', pkg.path(idx))
    return None


def _find_function(pkg, ci, name):
    """How UClass::FindFunctionByName (Class.cpp 5281-5323) finds `name` from class export ci: its FuncMap, then each
    implemented interface's, then its super's, up the Blueprint parents (their packages opened through
    Package.resolve). ('found', package, export index), or ('native', path) when the walk reaches a class no cooked
    package holds (a /Script class), or ('lost', path) when a /Game parent cannot be opened."""
    seen = set()
    while True:
        st = pkg.struct(ci)
        if (pkg.base, ci) in seen or st is None or not hasattr(st, 'func_map'): return ('lost', pkg.path(ci + 1))
        seen.add((pkg.base, ci))
        hit = _in_func_map(pkg, ci, name)
        if hit: return hit
        for iface, _, _ in st.interfaces:
            r = pkg.resolve(iface)
            hit = r and _in_func_map(r[0], r[1], name)
            if hit: return hit
        if st.super == 0: return ('native', pkg.path(ci + 1))
        if st.super > 0:
            ci = st.super - 1
            continue
        r = pkg.resolve(st.super)
        if r is None:
            return ('native' if pkg.path(st.super).startswith('/Script/') else 'lost', pkg.path(st.super))
        pkg, ci = r


def _type_key(pkg, p):
    """A property's type as the engine compares it: its class and, for an object / struct / enum / class property, the
    full path of the object it names."""
    ref = pkg.path(p.ref) if p.ref else None
    return (p.type, ref, pkg.path(p.meta) if p.meta else None, tuple(_type_key(pkg, s) for s in p.subs))


def _unreplicable(pkg, p, seen=()):
    """The path to the first TMap / TSet / delegate / interface in p's type tree, container inners and user-defined
    struct members included (a replicated struct is replicated member by member, skipping CPF_RepSkip members,
    RepLayout.cpp 5332-5340), or None. A native struct's members are not in any cooked package, so they are not
    walked."""
    if p.type in UNREPLICABLE: return '%s %s' % (p.type, p.name)
    for s in p.subs:
        bad = _unreplicable(pkg, s, seen)
        if bad: return '%s/%s' % (p.name, bad)
    if p.type == 'StructProperty' and p.ref:
        r = pkg.resolve(p.ref)
        if r:
            other, k = r
            st = other.struct(k)
            if st and st.kind == 'UserDefinedStruct' and (other.base, k) not in seen:
                for m in st.props:
                    if m.flags & CPF_RepSkip: continue
                    bad = _unreplicable(other, m, seen + ((other.base, k),))
                    if bad: return '%s.%s' % (p.name, bad)
    return None


@rule
def repl_count_matches_net_props(pkg):
    """A Blueprint class's NumReplicatedProperties tag (0 when absent) is the number of CPF_Net properties it declares
    itself. The engine needs at least that many: GetLifetimeBlueprintReplicationList (BlueprintGeneratedClass.cpp
    1786-1799) stops after NumReplicatedProperties CPF_Net properties in ChildProperties order, and one past the count
    gets no lifetime entry, so CompareParentProperty skips it and it never replicates (RepLayout.cpp 1347-1360). A
    higher count is harmless to the engine; exactly the count is what the Kismet compiler writes (KismetCompiler.cpp
    744, 787-790) and every game class keeps, so a higher one still means the compiler counted what it did not add."""
    for i, st in classes(pkg):
        tag = pkg.tag(i, 'NumReplicatedProperties')
        want, got = sum(1 for p in st.props if p.flags & CPF_Net), _i32(tag)
        if got < want:
            yield i, 'NumReplicatedProperties %d, the class declares %d CPF_Net properties: the last %d never ' \
                     'replicate' % (got, want, want - got)
        elif got > want:
            yield i, 'NumReplicatedProperties %d, the class declares %d CPF_Net properties (the editor writes the ' \
                     'count)' % (got, want)


@rule
def repl_condition_defined(pkg):
    """A CPF_Net class variable's BlueprintReplicationCondition (a byte on every FProperty, Property.cpp 588) is a
    defined ELifetimeCondition: 0..13 or COND_Never 15 (CoreNetTypes.h 12-27; 14 is no enumerator, COND_Max is 16). It
    becomes the variable's lifetime condition (BlueprintGeneratedClass.cpp 1797), which indexes the COND_Max
    ConditionMap (RepLayout.cpp 6805-6849; TStaticBitArray's range check is a check(), StaticBitArray.h 164, compiled
    out in Shipping). Only COND_Never skips setup (RepLayout.cpp 5947); bit 14 is never set, so a 14 is set up and
    compared and never sent. The engine reads the condition of no other property: that every other one - a variable
    without CPF_Net, a struct member, a parameter, a container's inner - holds COND_None (0) is what the editor
    writes (the details panel resets it when replication is switched off, BlueprintDetailsCustomization.cpp
    2334-2342; UDS members and parameters never get one) and the game keeps; a condition there is a marker the
    compiler wrote where it does nothing."""
    for i in range(len(pkg.exports)):
        st = pkg.struct(i)
        if not st: continue
        for p, depth in _all_props(st.props):
            if p.cond not in COND_DEFINED:
                yield i, 'property %s has replication condition %d, not an ELifetimeCondition' % (p.name, p.cond)
            elif p.cond and not (not depth and hasattr(st, 'func_map') and p.flags & CPF_Net):
                yield i, 'property %s has condition %d but is no CPF_Net class variable' % (p.name, p.cond)


@rule
def repl_notify_resolves(pkg):
    """A class variable's RepNotifyFunc names a function the class finds by name, own, an implemented interface's or a
    Blueprint parent's, with no return value and either no parameter or one of the variable's own type. RepLayout
    resolves the name at layout time (RepLayout.cpp 5956-5959): an unresolved one leaves RepNotifyNumParams INDEX_NONE
    and the OnRep is never queued, silently. It calls the function with NumParms parameters, the property's shadow
    value as the parms buffer (RepLayout.cpp 4456-4543): a return value counts in NumParms and is copied back into
    that buffer; two parameters trip a check on a plain property, more a checkf. The Kismet compiler keeps only a
    0-parameter, 0-return function (KismetCompiler.cpp 2531-2545).
    The runtime tests only the name, on CPF_Net variables. Beyond it, what the editor writes and the game keeps: a
    variable naming a RepNotify is CPF_Net | CPF_RepNotify (the details panel sets both with a name,
    BlueprintDetailsCustomization.cpp 2384-2388) - without CPF_Net it does not replicate at all; no struct member,
    parameter or inner names one (UDS members are written with None, UserDefinedStructureCompilerUtils.cpp 278); and
    the function is the Blueprint's own (the editor creates OnRep_<Var> in it, BlueprintDetailsCustomization.cpp
    2353-2363), so a name that only a native parent could hold - its functions are in no cooked package - is
    reported too."""
    for i in range(len(pkg.exports)):
        st = pkg.struct(i)
        if not st: continue
        top = st.props if hasattr(st, 'func_map') else []
        for p, depth in _all_props(st.props):
            if p.notify != 'None' and (depth or p not in top):
                yield i, '%s names RepNotify %s but is no class variable' % (p.name, p.notify)
    for i, st in classes(pkg):
        for p in st.props:
            if p.notify == 'None': continue
            if p.flags & (CPF_Net | CPF_RepNotify) != CPF_Net | CPF_RepNotify:
                yield i, '%s names RepNotify %s with flags %#x, not CPF_Net | CPF_RepNotify' % (p.name, p.notify, p.flags)
            found = _find_function(pkg, i, p.notify)
            if found[0] != 'found':
                yield i, '%s: RepNotify %s is no function of the class or its Blueprint parents (%s %s)' % (
                    p.name, p.notify, found[0], found[1])
                continue
            other, k = found[1], found[2]
            fn = other.struct(k)
            parms = [q for q in fn.props if q.flags & CPF_Parm]
            if any(q.flags & CPF_ReturnParm for q in parms):
                yield i, '%s: RepNotify %s returns a value' % (p.name, p.notify)
            elif len(parms) > 1 or (parms and _type_key(other, parms[0]) != _type_key(pkg, p)):
                yield i, '%s: RepNotify %s takes %s' % (p.name, p.notify, [repr(q) for q in parms])


@rule
def repl_map_set_not_net(pkg):
    """A TMap or TSet property is never CPF_Net or CPF_RepNotify and names no RepNotify. Replicated, it sends nothing:
    FMapProperty / FSetProperty::NetSerializeItem only log 'Replicated TMaps are not supported.' each time
    (PropertyMap.cpp 505-509, PropertySet.cpp 451-455), and UnrealHeaderTool refuses one in a replicated struct or an
    RPC (HeaderParser.cpp 6930-6945, 8064-8081). CPF_RepNotify and a RepNotify name without CPF_Net do nothing at run
    time; that a map or set has none of the three is what the editor writes - it clears all three when a variable
    becomes a map or set, and greys replication out for one (BlueprintEditorUtils.cpp 5193-5214,
    BlueprintDetailsCustomization.cpp 1991-2003) - and the game keeps."""
    for i in range(len(pkg.exports)):
        st = pkg.struct(i)
        if not st: continue
        for p in st.props:
            if p.type in ('MapProperty', 'SetProperty') and (p.flags & (CPF_Net | CPF_RepNotify) or p.notify != 'None'):
                yield i, '%s %s flags %#x notify %s' % (p.type, p.name, p.flags, p.notify)


@rule
def repl_types_replicable(pkg):
    """No CPF_Net class variable and no parameter of a FUNC_Net function is, or holds through an array, map, set or a
    user-defined struct's member (CPF_RepSkip ones aside), a TMap, TSet, delegate, multicast delegate or interface:
    their NetSerializeItem writes no bits (PropertyMap.cpp 505-509, PropertySet.cpp 451-455, PropertyDelegate.cpp
    89-93, PropertyMulticastDelegate.cpp 121-125, PropertyInterface.cpp 153-157), and a replicated struct or an RPC's
    parameter is laid out member by member (RepLayout.cpp 5321-5384, InitFromFunction 6112-6161), so the value never
    reaches the remote machine. UnrealHeaderTool refuses the map, set and delegate cases in native code
    (HeaderParser.cpp 6930-6949, 6964-6967, 8042-8097, 8205-8209); the Blueprint editor refuses only a map or set
    variable itself (BlueprintDetailsCustomization.cpp 1991-2003), so the struct-member and interface cases rest on
    NetSerializeItem alone."""
    for i in range(len(pkg.exports)):
        st = pkg.struct(i)
        if not st: continue
        if hasattr(st, 'func_map'): props, what = [p for p in st.props if p.flags & CPF_Net], 'replicated variable'
        elif hasattr(st, 'function_flags') and st.function_flags & FUNC_Net:
            props, what = [p for p in st.props if p.flags & CPF_Parm], 'RPC parameter'
        else: continue
        for p in props:
            bad = _unreplicable(pkg, p)
            if bad: yield i, '%s %s holds %s' % (what, p.name, bad)


@rule
def repl_rpc_one_direction(pkg):
    """A FUNC_Net function has exactly one of FUNC_NetServer, FUNC_NetClient and FUNC_NetMulticast, and no function
    has a direction or FUNC_NetReliable without FUNC_Net. The sender (Actor.cpp 4255-4305) tests Multicast, then
    Client, then Server on the topmost super, so with two directions the one tested later is dead on one side: a
    Multicast | Server function called on a client runs there and never reaches the server; with none the call runs
    locally on every machine. The receiver checks its own flags (DataReplication.cpp 1194-1204). The editor sets one
    direction with FUNC_Net, clearing the others (BlueprintDetailsCustomization.cpp 3868-3895), and drops every net
    flag without FUNC_Net (K2Node_CustomEvent.cpp 251-275); UnrealHeaderTool refuses 'reliable' without a direction
    (HeaderParser.cpp 606-609). An orphan direction is otherwise inert, except that a standalone game absorbs a
    non-authority actor's call to a function with FUNC_NetServer (Actor.cpp 4235-4242)."""
    for i, st in functions(pkg):
        f = st.function_flags
        dirs = bin(f & FUNC_NetDirections).count('1')
        if f & FUNC_Net and dirs != 1:
            yield i, 'FUNC_Net function with %d directions (flags %#x)' % (dirs, f)
        elif not f & FUNC_Net and f & (FUNC_NetDirections | FUNC_NetReliable):
            yield i, 'net direction / reliable flags %#x without FUNC_Net' % (f & (FUNC_NetDirections | FUNC_NetReliable))


@rule
def repl_rpc_not_static(pkg):
    """A FUNC_Net function is never FUNC_Static: AActor::GetFunctionCallspace sends a static function to
    GetGlobalFunctionCallspace (Actor.cpp 4191-4197, ActorComponent.cpp 688-694), which returns only Local or Absorbed
    (UnrealEngine.cpp 14837-14887), so the RPC is never sent. UnrealHeaderTool refuses one (HeaderParser.cpp
    591-594)."""
    for i, st in functions(pkg):
        if st.function_flags & (FUNC_Net | FUNC_Static) == FUNC_Net | FUNC_Static:
            yield i, 'FUNC_Net | FUNC_Static (flags %#x)' % st.function_flags


@rule
def repl_rpc_no_return(pkg):
    """A FUNC_Net function has no CPF_ReturnParm property: FRepLayout::InitFromFunction (RepLayout.cpp 6119) lays out
    parameters only while (flags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm, so the return value and every parameter
    after it are never sent; and a caller of a remote-only call gets its return value zeroed (ScriptCore.cpp
    1172-1187). UnrealHeaderTool refuses one ('Replicated functions can't have return values', HeaderParser.cpp
    7662-7668)."""
    for i, st in functions(pkg):
        if st.function_flags & FUNC_Net and any(p.flags & CPF_ReturnParm for p in st.props):
            yield i, 'FUNC_Net function returns a value'


@rule
def repl_override_keeps_net_flags(pkg):
    """A function whose SuperStruct is another function (an override) has the same FUNC_NetFuncFlags as it: UClass::
    SetUpRuntimeReplicationData checks it (Class.cpp 4189-4190: a check(), live in a cooked Development game, compiled
    out in Shipping) and enters only root functions in NetFields (4191-4194); the sender routes by the topmost super's
    flags (Actor.cpp 4264-4268) while the receiver checks the most-derived function's own (DataReplication.cpp
    1182-1204). The editor gives an override its parent's net flags (K2Node_CustomEvent.cpp 254-264). A native
    super's flags are in no cooked package, so only a Blueprint super is compared."""
    for i, st in functions(pkg):
        if not st.super: continue
        r = pkg.resolve(st.super)
        if not r: continue
        other, k = r
        sup = other.struct(k)
        if sup is None or not hasattr(sup, 'function_flags'): continue
        if st.function_flags & FUNC_NetFuncFlags != sup.function_flags & FUNC_NetFuncFlags:
            yield i, 'net flags %#x, its super %s has %#x' % (st.function_flags & FUNC_NetFuncFlags, other.path(k + 1),
                                                             sup.function_flags & FUNC_NetFuncFlags)


MINE = {'repl_count_matches_net_props', 'repl_condition_defined', 'repl_notify_resolves', 'repl_map_set_not_net',
        'repl_types_replicable', 'repl_rpc_one_direction', 'repl_rpc_not_static', 'repl_rpc_no_return',
        'repl_override_keeps_net_flags'}
