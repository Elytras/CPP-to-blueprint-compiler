import sys
from invariants import *
import invariants

"""The UBER work package's rules: the ubergraph and its persistent frame, latent-action literals and where they resume,
the flow stack, ubergraph names along a class chain, and the operands of deferred spawns and component adds. Each rule
is a generator over one Package yielding (export index, message), registered with invariants.RULES by @rule."""


# ---- helpers shared by the rules below

def _i32(b):
    return struct.unpack_from('<i', b)[0]


def uber_tag(pkg, ci):
    """The FPackageIndex class export ci's UberGraphFunction tag holds, 0 when it has none (the tag is left out of a
    class without an event graph, as a null ObjectProperty is)."""
    t = pkg.tag(ci, 'UberGraphFunction')
    return _i32(t['value']) if t and t['type'] == 'ObjectProperty' and len(t['value']) == 4 else 0


def outer_class(pkg, i):
    """The class export (0-based) function export i is outered to, or None."""
    o = pkg.exports[i]['outer']
    if o <= 0: return None
    st = pkg.struct(o - 1)
    return o - 1 if st is not None and hasattr(st, 'class_flags') else None


def ancestors(pkg, ci, limit=64):
    """(Package, class export index) of each Blueprint ancestor of class export ci, nearest first, as far as their
    packages resolve (Package.resolve: this Content folder, then GAME_CONTENT). A /Script parent ends the walk: a
    native class has no ubergraph and no Blueprint functions."""
    p, i = pkg, ci
    for _ in range(limit):
        r = p.resolve(p.struct(i).super)
        if not r: return
        p, i = r
        st = p.struct(i)
        if st is None or not hasattr(st, 'class_flags'): return
        yield p, i


def chain_complete(pkg, ci):
    """Whether class ci's super chain resolves all the way to a /Script class, so a lookup that misses every level
    really misses."""
    p, i = pkg, ci
    for _ in range(64):
        sup = p.struct(i).super
        if sup == 0: return True
        if sup < 0:
            outermost = sup
            while p.obj(outermost)['outer']: outermost = p.obj(outermost)['outer']
            if p.obj(outermost)['name'].startswith('/Script/'): return True
        r = p.resolve(sup)
        if not r: return False
        p, i = r
        if p.struct(i) is None or not hasattr(p.struct(i), 'class_flags'): return False
    return False


def same_package(a, b):
    return os.path.normcase(os.path.abspath(a.base)) == os.path.normcase(os.path.abspath(b.base))


def top(pkg, i):
    return pkg.script(i)


def entry_offsets(pkg, u):
    """The offsets an ubergraph (export u) may be entered at: the in-memory starts of its top-level statements after
    the EX_ComputedJump that dispatches on EntryPoint, the EX_EndOfScript left out. Offset 0 would re-run the dispatch
    itself; None when the script has no dispatch."""
    t = top(pkg, u)
    j = next((k for k, s in enumerate(t) if s.op == 0x4E), None)
    if j is None: return None
    return {s.mem for s in t[j + 1:] if s.op != 0x53}


INT_LITERALS = (0x1D, 0x25, 0x26, 0x2C, 0x5B)


def int_literal(pkg, i, n):
    """The value of an int literal node of export i's script (IntConst, IntZero, IntOne, IntConstByte,
    SkipOffsetConst), else None."""
    if n.op == 0x25: return 0
    if n.op == 0x26: return 1
    if n.op == 0x2C: return pkg.blob(i)[n.disk + 1]
    if n.op in (0x1D, 0x5B): return n.ops[0][1]
    return None


def members(n):
    """An EX_StructConst's member expressions, the EX_EndStructConst left out."""
    return n.kids[:-1] if n.kids and n.kids[-1].op == 0x30 else list(n.kids)


def latent_infos(pkg, i):
    """(top-level statement index, EX_StructConst node) of every /Script/Engine.LatentActionInfo literal in export i."""
    for k, t in enumerate(top(pkg, i)):
        for n in t.walk():
            if n.op == 0x2F and n.ops and n.ops[0][0] == 'obj' \
                    and (pkg.path(n.ops[0][1]) or '').lower() == '/script/engine.latentactioninfo':
                yield k, n


def linkage_of(pkg, i, n):
    """A LatentActionInfo literal's Linkage when it is an int literal, else None."""
    m = members(n)
    return int_literal(pkg, i, m[0]) if m else None


CALLS = (0x1B, 0x1C, 0x45, 0x46, 0x68)


def call_name(pkg, n):
    """The name of the function a call node reaches: its import / export name, or the name a virtual call looks up."""
    if n.op not in CALLS or not n.ops: return None
    kind, v = n.ops[0][:2]
    if kind == 'name': return v
    if kind == 'obj' and v: return pkg.obj(v)['name']
    return None


def args(n):
    return [k for k in n.kids if k.op != 0x16]


def norm(o):
    return o[:2] if o[0] == 'int' else o


def same_tree(a, b):
    """Two expressions read alike: the same opcodes, operands (disk offsets aside) and sub-expressions."""
    return a.op == b.op and [norm(o) for o in a.ops] == [norm(o) for o in b.ops] and len(a.kids) == len(b.kids) \
        and all(same_tree(x, y) for x, y in zip(a.kids, b.kids))


CASTS = (0x2E, 0x13, 0x55, 0x52, 0x54)


def uncast(n):
    while n.op in CASTS and n.kids: n = n.kids[-1]
    return n


def calls_in(n, actor=None):
    """(call node, the actor expression it runs on - None for self) for every call under n: a call that is an
    EX_Context's guarded expression runs on the context object; its arguments still run on self."""
    if n.op in CALLS: yield n, actor
    for j, k in enumerate(n.kids):
        yield from calls_in(k, n.kids[0] if n.op in (0x19, 0x1A) and j == 1 and k.op in CALLS else None)


# -- the flow stack, statement by statement

def flow_index(t):
    return {s.mem: k for k, s in enumerate(t)}


def successors(t, k, stack, entries, index, latent=None):
    """The (statement index, flow stack) pairs statement k passes control to, as the VM runs it (ScriptCore.cpp
    execJump 2374, execJumpIfNot 2398, execComputedJump 2384, execPushExecutionFlow 2445, execPopExecutionFlow 2453,
    execPopExecutionFlowIfNot 2472; EX_Return ends the invocation). None stands for a pop that finds the stack empty.
    `latent` maps a statement to where its latent action resumes, to follow a thread across the wait."""
    n, nxt = t[k], [(k + 1, stack)]
    if latent and k in latent: return latent[k]
    if n.op == 0x06:
        j = index.get(n.ops[0][1])
        return [(j, stack)] if j is not None else []
    if n.op == 0x07:
        j = index.get(n.ops[0][1])
        return nxt + ([(j, stack)] if j is not None else [])
    if n.op == 0x4C:
        j = index.get(n.ops[0][1])
        return [(k + 1, stack + (j,))] if j is not None else []
    if n.op == 0x4D: return [(stack[-1], stack[:-1])] if stack else [None]
    if n.op == 0x4F: return nxt + ([(stack[-1], stack[:-1])] if stack else [None])
    if n.op == 0x4E: return [(e, stack) for e in entries]
    if n.op in (0x04, 0x53): return []
    return nxt


def ubergraph_entries(pkg):
    """export index -> the statement offsets this package enters it at: an int literal passed to a call of it, a
    latent Linkage literal naming it, an EventGraphCallOffset into it."""
    if hasattr(pkg, '_uber_entries'): return pkg._uber_entries
    out = {}
    names = {}
    for i, st in functions(pkg):
        names.setdefault(pkg.exports[i]['name'].lower(), i)
        if st.event_graph > 0: out.setdefault(st.event_graph - 1, set()).add(st.event_graph_offset)
    for i, st in functions(pkg):
        for n in statements(pkg, i)[0]:
            if n.op in (0x1C, 0x46, 0x68) and n.ops and n.ops[0][0] == 'obj' and n.ops[0][1] > 0:
                a = args(n)
                if len(a) == 1 and int_literal(pkg, i, a[0]) is not None:
                    out.setdefault(n.ops[0][1] - 1, set()).add(int_literal(pkg, i, a[0]))
        for k, n in latent_infos(pkg, i):
            m, link = members(n), linkage_of(pkg, i, n)
            if link is not None and link != -1 and len(m) > 2 and m[2].op == 0x21 and m[2].ops[0][1].lower() in names:
                out.setdefault(names[m[2].ops[0][1].lower()], set()).add(link)
    pkg._uber_entries = out
    return out


def reach(pkg, i, limit=200000):
    """Every (statement index, flow stack) export i's script reaches from its entry with an empty flow stack (each
    call starts a new FFrame, whose FlowStack is empty), and the statements where a pop finds it empty."""
    key = '_reach_%d' % i
    if hasattr(pkg, key): return getattr(pkg, key)
    t = top(pkg, i)
    index = flow_index(t)
    entries = sorted(index[o] for o in ubergraph_entries(pkg).get(i, ()) if o in index)
    seen, todo, under = set(), [(0, ())], set()
    while todo and len(seen) < limit:
        st = todo.pop()
        if st in seen or st[0] >= len(t): continue
        seen.add(st)
        for s2 in successors(t, st[0], st[1], entries, index):
            if s2 is None: under.add(st[0])
            elif len(s2[1]) <= 64: todo.append(s2)
    setattr(pkg, key, (seen, under))
    return seen, under


# ---- the rules

@rule
def uber_tag_own_function(pkg):
    """A class's UberGraphFunction names a Function export outered to that class, flagged FUNC_UbergraphFunction
    (0x8000), and in the class's FuncMap under its own name. GetPersistentUberGraphFrame hands out the frame only when
    UberGraphFunction == the called function (BlueprintGeneratedClass.cpp 1312-1327); ProcessEvent asks for it only
    when the function has FUNC_UbergraphFunction (ScriptCore.cpp 1943-1949), else runs it on a fresh zeroed alloca
    frame; and a latent resume finds the ubergraph by name, FindFunction over FuncMap (LatentActionManager.cpp
    214-216, Class.cpp 5281-5284), so one missing there never resumes."""
    for ci, st in classes(pkg):
        k = uber_tag(pkg, ci)
        if not k: continue
        if k < 0:
            yield ci, 'UberGraphFunction is the import %s, not a function of this class' % pkg.path(k); continue
        if pkg.class_of(k) not in Package.FUNCTION_CLASSES or pkg.exports[k - 1]['outer'] != ci + 1:
            yield ci, 'UberGraphFunction %s is not a function outered to the class' % pkg.path(k); continue
        name = pkg.exports[k - 1]['name']
        if not pkg.struct(k - 1).function_flags & 0x8000:
            yield k - 1, 'the class\'s UberGraphFunction lacks FUNC_UbergraphFunction: flags %#x' % pkg.struct(k - 1).function_flags
        if dict((n.lower(), v) for n, v in st.func_map).get(name.lower()) != k:
            yield ci, 'FuncMap does not map %s to its UberGraphFunction export' % name


@rule
def uber_frame_property(pkg):
    """A class with an UberGraphFunction declares, itself (ExcludeSuper), an FStructProperty named UberGraphFrame whose
    struct is /Script/Engine.PointerToUberGraphFrame: Link looks it up by that name among the class's own struct
    properties only (BlueprintGeneratedClass.cpp 1631-1646) and every frame access reinterprets its memory as an
    FPointerToUberGraphFrame (1318, 1342). Without it no persistent frame is ever made (the ensure at 1339 is the only
    signal) and EX_LetValueOnPersistentFrame writes through null + offset (ScriptCore.cpp 2507-2512, checkSlow only)."""
    for ci, st in classes(pkg):
        if not uber_tag(pkg, ci): continue
        frame = next((p for p in st.props if p.type == 'StructProperty' and p.name.lower() == 'ubergraphframe'), None)
        if frame is None:
            yield ci, 'has an UberGraphFunction but no StructProperty UberGraphFrame of its own'
        elif (pkg.path(frame.ref) or '').lower() != '/script/engine.pointertoubergraphframe':
            yield ci, 'UberGraphFrame is a %s, not a PointerToUberGraphFrame' % pkg.path(frame.ref)


@rule
def uber_entry_parameter(pkg):
    """An ubergraph's parameters are exactly one int32, first among its properties, neither out nor return: a latent
    resume is ProcessEvent(Ubergraph, &LinkID) with LinkID an int32 (LatentActionManager.cpp 216), and ProcessEvent
    copies ParmsSize bytes from there into the frame's start (ScriptCore.cpp 1958); the event fast path passes the
    same &int32 (1923-1937). A larger parameter block reads past LinkID; another first property misplaces it."""
    for ci, st in classes(pkg):
        k = uber_tag(pkg, ci)
        if k <= 0 or pkg.class_of(k) not in Package.FUNCTION_CLASSES: continue
        props = pkg.struct(k - 1).props
        parms = [p for p in props if p.flags & CPF_Parm]
        if len(parms) != 1 or parms[0].type != 'IntProperty' or parms[0].flags & (CPF_OutParm | CPF_ReturnParm) \
                or props[0] is not parms[0]:
            yield k - 1, 'ubergraph parameters %s, want one int32 first' % [(p.type, p.name, hex(p.flags)) for p in parms]


PRE_DISPATCH = (0x4C, 0x5E, 0x5A, 0x0B, 0x6A, 0x50)     # PushExecutionFlow and the no-op trace / nothing opcodes


@rule
def uber_entry_dispatch(pkg):
    """An ubergraph dispatches on its entry parameter before anything with an effect runs: the first statement other
    than an EX_PushExecutionFlow or a no-op trace is EX_ComputedJump(EX_LocalVariable <that parameter>).
    The editor's shape, which the game keeps (2,906 ubergraphs): every ubergraph opens with KCST_ComputedGoto on
    EntryPoint (K2Node_FunctionEntry.cpp 151-160), after the return-address push when it uses a flow stack
    (KismetCompilerVMBackend.cpp 2197-2200). The engine itself needs less - only that each entry runs the code of its
    own offset: every event stub and latent resume starts the ubergraph at Script[0] (ProcessEvent / ProcessScriptFunction),
    so whatever precedes the dispatch runs on every entry, and execComputedJump goes where its operand says, unchecked
    in shipping (ScriptCore.cpp 2384-2395). Another dispatch (a chain of compares) would be legal; the rule keeps the
    editor's because the entry rules below (uber_entry_calls, uber_event_graph_fast_call, uber_latent_resume_target, the
    flow exploration) read an ubergraph's entries as the statements after this jump."""
    for ci, st in classes(pkg):
        k = uber_tag(pkg, ci)
        if k <= 0 or pkg.class_of(k) not in Package.FUNCTION_CLASSES: continue
        parms = [p.name for p in pkg.struct(k - 1).props if p.flags & CPF_Parm]
        first = next((s for s in top(pkg, k - 1) if s.op not in PRE_DISPATCH), None)
        ok = first is not None and first.op == 0x4E and first.kids and first.kids[0].op == 0x00 \
            and first.kids[0].ops[0][2] == k and first.kids[0].ops[0][1][-1:] == parms[:1]
        if not ok:
            yield k - 1, 'first statement %s is not a computed jump on the entry parameter %s' % (
                '%02x' % first.op if first else None, parms[:1])


@rule
def uber_frame_write_owner(pkg):
    """EX_LetValueOnPersistentFrame names a property of the UberGraphFunction of the running function's class or of a
    Blueprint ancestor: the VM takes the frame from the property's owner (CastChecked<UFunction>, unchecked in
    shipping) through GetPersistentUberGraphFrame, which answers only for a class's own UberGraphFunction up the
    chain, and null otherwise - then the write lands at null + offset (ScriptCore.cpp 2498-2516, the IsChildOf and
    frame checks are checkSlow; BlueprintGeneratedClass.cpp 1312-1327)."""
    for i, st in functions(pkg):
        owners = None
        for n in statements(pkg, i)[0]:
            if n.op != 0x64: continue
            _, segs, owner = n.ops[0]
            if owners is None: owners = chain_ubers(pkg, outer_class(pkg, i))
            r = pkg.resolve(owner)
            if r is None:                                  # null, or /Script: no Blueprint ubergraph; a /Game package
                outermost = owner                          # not found here: nothing to check it against
                while outermost < 0 and pkg.obj(outermost)['outer']: outermost = pkg.obj(outermost)['outer']
                if owner == 0 or not pkg.obj(outermost)['name'].startswith('/Game/'):
                    yield i, 'LetValueOnPersistentFrame at mem %d writes %s of %s, no Blueprint ubergraph' % (
                        n.mem, '.'.join(segs), pkg.path(owner))
                continue
            p, u = r
            if not any(same_package(p, q) and u == v for q, v in owners):
                yield i, 'LetValueOnPersistentFrame at mem %d writes %s of %s, not an ubergraph of the class chain' % (
                    n.mem, '.'.join(segs), p.path(u + 1))
            elif segs[-1].lower() not in {q.name.lower() for q in p.struct(u).props}:
                yield i, 'LetValueOnPersistentFrame at mem %d writes %s, not a property of %s' % (
                    n.mem, segs[-1], p.exports[u]['name'])


def chain_ubers(pkg, c):
    """(Package, export index) of the UberGraphFunction of class export c and of each Blueprint ancestor that has one."""
    if c is None: return []
    out = [(pkg, uber_tag(pkg, c) - 1)] if uber_tag(pkg, c) > 0 else []
    return out + [(p, uber_tag(p, a) - 1) for p, a in ancestors(pkg, c) if uber_tag(p, a) > 0]


def is_uber(p, u):
    """Whether export u of package p is the UberGraphFunction of the class it is outered to."""
    c = outer_class(p, u)
    return c is not None and uber_tag(p, c) == u + 1


@rule
def uber_entry_calls(pkg):
    """A call into an ubergraph passes one argument, and a literal one is an offset the ubergraph is entered at - a
    top-level statement after its EX_ComputedJump: execComputedJump sets Code = &Script[offset] with only a check()
    compiled out of shipping (ScriptCore.cpp 2384-2395). Out of range reads past the script; a mid-statement offset
    runs operand bytes as opcodes; 0 re-runs the dispatch; no argument leaves EntryPoint 0. Called on self, it is the
    ubergraph of the calling function's class or of a Blueprint ancestor: the frame is looked up through the
    ubergraph's own class at that class's UberGraphFrame offset in self (ScriptCore.cpp 819-827;
    BlueprintGeneratedClass.cpp 1312-1321), which an object of an unrelated class does not have."""
    for i, st in functions(pkg):
        owners = None
        for t in top(pkg, i):
            for n, mine in t.on_self():
                if n.op not in (0x1C, 0x46) or not n.ops or n.ops[0][0] != 'obj' or not n.ops[0][1]: continue
                r = pkg.resolve(n.ops[0][1])
                if not r or not is_uber(*r): continue
                p, u = r
                if owners is None: owners = chain_ubers(pkg, outer_class(pkg, i))
                if mine and not any(same_package(p, q) and u == v for q, v in owners):
                    yield i, 'call at mem %d runs %s on self, not an ubergraph of the class chain' % (n.mem, p.path(u + 1))
                a = args(n)
                if len(a) != 1:
                    yield i, 'call at mem %d into %s passes %d arguments' % (n.mem, p.exports[u]['name'], len(a)); continue
                v, ok = int_literal(pkg, i, a[0]), entry_offsets(p, u)
                if v is not None and ok is not None and v not in ok:
                    yield i, 'call at mem %d enters %s at %d, not a statement after its dispatch' % (n.mem, p.exports[u]['name'], v)


@rule
def uber_event_graph_fast_call(pkg):
    """A function's EventGraphFunction, when set, is an ubergraph of its class chain, the function has no parameters,
    and EventGraphCallOffset is an entry of that ubergraph: ProcessEvent then skips the stub and calls the ubergraph
    with &EventGraphCallOffset as its parameters (ScriptCore.cpp 1923-1937), so a stub's own parameters are never
    copied into the frame and the offset goes straight to execComputedJump (2384-2395). The editor sets it only for
    a parameterless stub (KismetCompilerVMBackend.cpp 1176-1200)."""
    for i, st in functions(pkg):
        if not st.event_graph: continue
        r = pkg.resolve(st.event_graph)
        if r is None or not any(same_package(r[0], q) and r[1] == v for q, v in chain_ubers(pkg, outer_class(pkg, i))):
            yield i, 'EventGraphFunction %s is not an ubergraph of the class chain' % pkg.path(st.event_graph); continue
        if any(p.flags & CPF_Parm for p in st.props):
            yield i, 'has parameters, which the EventGraphFunction fast path never copies'
        ok = entry_offsets(r[0], r[1])
        if ok is not None and st.event_graph_offset not in ok:
            yield i, 'EventGraphCallOffset %d is not an entry of %s' % (st.event_graph_offset, r[0].exports[r[1]]['name'])


@rule
def uber_latent_info_members(pkg):
    """A LatentActionInfo literal has one member per property - Linkage, UUID, ExecutionFunction, CallbackTarget, none
    transient (LatentActionManager.h 14-49) - as execStructConst steps exactly that many and then skips one byte for
    the EX_EndStructConst (ScriptCore.cpp 3376-3395): an engine requirement.
    CallbackTarget is EX_Self. The engine resumes whatever object it names - the action is filed under it
    (KismetSystemLibrary.cpp 2166-2171) and the link runs on it (LatentActionManager.cpp 205-229), and one filed under
    None is dropped - but only self holds the waiting thread's frame, so any other object resumes someone else's
    thread; the editor always writes Self (KismetCompilerVMBackend.cpp 1123-1129) and the game keeps it.
    Where the call resumes (Linkage not -1), Linkage is an int literal and ExecutionFunction an EX_NameConst: the
    editor writes a SkipOffsetConst and a NameConst (KismetCompilerVMBackend.cpp 1116-1122, 1130-1139). The engine
    checks neither, so a computed one could not be shown to resume anywhere valid; this is what lets the resume rules
    below check every site."""
    for i, st in functions(pkg):
        for k, n in latent_infos(pkg, i):
            m = members(n)
            if len(m) != 4:
                yield i, 'LatentActionInfo at mem %d has %d members, not 4' % (n.mem, len(m)); continue
            if m[3].op != 0x17:
                yield i, 'LatentActionInfo at mem %d: CallbackTarget is op %02x, not Self' % (n.mem, m[3].op)
            link = int_literal(pkg, i, m[0])
            if link == -1: continue
            if link is None: yield i, 'LatentActionInfo at mem %d: Linkage is op %02x, not an int literal' % (n.mem, m[0].op)
            if m[2].op != 0x21: yield i, 'LatentActionInfo at mem %d: ExecutionFunction is op %02x, not a name' % (n.mem, m[2].op)


def find_function(pkg, ci, name):
    """UClass::FindFunctionByName for a class export and its Blueprint ancestors (Class.cpp 5281-5323): the first
    FuncMap along the chain holding the name, FName-compared (case-insensitive). (Package, export index), or None, or
    'unknown' when the chain does not resolve far enough to say."""
    want = name.lower()
    for p, c in [(pkg, ci)] + list(ancestors(pkg, ci)):
        hit = next((v for n, v in p.struct(c).func_map if n.lower() == want), None)
        if hit is not None: return p, hit - 1
    return None if chain_complete(pkg, ci) else 'unknown'


@rule
def uber_latent_resume_target(pkg):
    """A latent call that resumes (Linkage not -1) names, as ExecutionFunction, a function that FindFunction finds
    from the calling class - its FuncMap first, then its Blueprint ancestors' (Class.cpp 5279-5314) - and Linkage is an
    entry offset of that function: on completion the manager runs ProcessEvent(FindFunction(ExecutionFunction),
    &Linkage) on the object (LatentActionManager.cpp 205-229), which keeps the waiting thread's locals only in a class's
    UberGraphFunction (ScriptCore.cpp 1943-1955) and jumps to Linkage unchecked (2384-2395). A name found nowhere
    never resumes (a warning, 218-221). A descendant shadowing the name is uber_name_unshadowed's.
    Stricter than the engine, as the editor has it and the game keeps it (2,218 resuming sites): the call sits in its
    class's own ubergraph and the name finds that same function - the editor emits latent info only in the
    ubergraph (KismetCompilerVMBackend.cpp 1297) and names it ExecuteUbergraph_<Blueprint> (CallFunctionHandler.cpp
    124-125) - since a continuation offset only means something in the function it was taken from."""
    for i, st in functions(pkg):
        infos = [(k, n) for k, n in latent_infos(pkg, i)]
        if not infos: continue
        c = outer_class(pkg, i)
        for k, n in infos:
            m, link = members(n), linkage_of(pkg, i, n)
            if link in (None, -1) or len(m) != 4 or m[2].op != 0x21: continue
            name = m[2].ops[0][1]
            if c is None or uber_tag(pkg, c) != i + 1:
                yield i, 'latent call at mem %d resumes, from a function that is not its class\'s ubergraph' % n.mem; continue
            f = find_function(pkg, c, name)
            if f == 'unknown': continue
            if f is None:
                yield i, 'latent call at mem %d resumes %s, which the class chain does not have' % (n.mem, name); continue
            if not (same_package(f[0], pkg) and f[1] == i):
                yield i, 'latent call at mem %d resumes %s, which resolves to %s, not the ubergraph making the call' % (
                    n.mem, name, f[0].path(f[1] + 1))
                continue
            ok = entry_offsets(pkg, i)
            if ok is not None and link not in ok:
                yield i, 'latent call at mem %d resumes at %d, not an entry of %s' % (n.mem, link, name)


def class_uuids(pkg, ci):
    """(UUID, where) of every resuming LatentActionInfo literal (Linkage a literal other than -1) with a literal UUID
    in the functions of class export ci."""
    out = []
    for i, st in functions(pkg):
        if outer_class(pkg, i) != ci: continue
        for k, n in latent_infos(pkg, i):
            m = members(n)
            if len(m) > 1 and int_literal(pkg, i, m[1]) is not None and linkage_of(pkg, i, n) not in (None, -1):
                out.append((int_literal(pkg, i, m[1]), '%s:%s@%d' % (pkg.package_name(), pkg.exports[i]['name'], n.mem)))
    return out


@rule
def uber_latent_uuid_chain(pkg):
    """Two latent call sites that resume (Linkage not -1) and can be pending on one object - in its class or a
    Blueprint ancestor - have distinct UUIDs: the manager keys actions by (CallbackTarget, UUID), Delay drops a request
    while one with the same key is pending (KismetSystemLibrary.cpp 2166-2171), so the second site's continuation never
    runs, and FindExistingAction casts whatever it finds to the requested action type unchecked (LatentActionManager.h
    104-139). The dedup is meant for one site called again; the editor gives each latent node a UUID of its own
    (CalculateStableIdentifierForLatentActionManager, CallFunctionHandler.cpp 122). Narrowed to resuming sites by
    calibration: the game's own cooked Blueprints repeat one UUID over several LoadAsset calls with Linkage -1
    (8 groups in 7 of 51,579 packages, e.g. _MENU_MinersManual, EWC_Base); LoadAsset always adds a new action
    (KismetSystemLibrary.cpp 2662, 2691) and Linkage -1 never resumes (LatentActionManager.cpp 208)."""
    for ci, st in classes(pkg):
        own = class_uuids(pkg, ci)
        if not own: continue
        seen = {}
        for u, where in own: seen.setdefault(u, []).append(where)
        for p, a in ancestors(pkg, ci):
            for u, where in class_uuids(p, a):
                if u in seen: seen[u].append(where)
        for u, wheres in seen.items():
            if len(wheres) > 1: yield ci, 'UUID %d at %s' % (u, ', '.join(wheres))


@rule
def uber_name_unshadowed(pkg):
    """No class declares a function named like a Blueprint ancestor's ubergraph, nor has its own ubergraph named like
    an ancestor's function: a latent resume looks its ExecutionFunction up by name on the object, the most-derived
    FuncMap first (LatentActionManager.cpp 214-216; Class.cpp 5281-5284; FName compares case-insensitively), so the
    descendant's function would run the ancestor's resume at the ancestor's offset, on the wrong frame."""
    for ci, st in classes(pkg):
        mine = {n.lower() for n, v in st.func_map}
        own_uber = uber_tag(pkg, ci)
        own_name = pkg.exports[own_uber - 1]['name'].lower() if own_uber > 0 else None
        for p, a in ancestors(pkg, ci):
            theirs = {n.lower() for n, v in p.struct(a).func_map}
            u = uber_tag(p, a)
            if u > 0 and p.exports[u - 1]['name'].lower() in mine:
                yield ci, 'declares %s, the ubergraph of its ancestor %s' % (p.exports[u - 1]['name'], p.exports[a]['name'])
            if own_name and own_name in theirs:
                yield ci, 'its ubergraph %s is also a function of its ancestor %s' % (pkg.exports[own_uber - 1]['name'], p.exports[a]['name'])


@rule
def uber_flow_stack_underflow(pkg):
    """No path pops an empty flow stack: each invocation starts a new FFrame with an empty FlowStack, and
    EX_PopExecutionFlow / EX_PopExecutionFlowIfNot on an empty one only logs 'Tried to pop from an empty flow stack'
    and carries on into the next bytes (ScriptCore.cpp 2453-2496) - whatever statement follows, often another
    event's code. Explored per function from its entry, a computed jump going to every offset the package enters it
    at (KB-28 / NODE-12: the editor pushes the final return first whenever it pops)."""
    for i, st in functions(pkg):
        if not any(n.op in (0x4D, 0x4F) for n in top(pkg, i)): continue
        seen, under = reach(pkg, i)
        t = top(pkg, i)
        for k in sorted(under):
            yield i, 'op %02x at mem %d pops an empty flow stack' % (t[k].op, t[k].mem)


@rule
def uber_latent_no_fallthrough(pkg):
    """The code after a latent call does not run the call's continuation in the same invocation: the manager runs the
    link later, as a new ProcessEvent at Linkage (LatentActionManager.cpp 171-229; LatentActions.h 52-66), so a
    continuation also reached by falling through the call runs twice.
    Not an engine requirement, and not one the editor's own output always keeps: the editor ends the thread after a
    latent call (KCST_EndOfThread, CallFunctionHandler.cpp 467-473), which is EX_PopExecutionFlow under a flow stack,
    and a Sequence pin wired into the latent node's Completed chain would reach the continuation that way - none of
    the game's 2,218 resuming sites does. For AssetGen it is an invariant of the lowering: a latent call suspends the
    C++ statements it sits in until the resume, so the continuation runs once."""
    for i, st in functions(pkg):
        infos = [(k, linkage_of(pkg, i, n)) for k, n in latent_infos(pkg, i)]
        infos = [(k, l) for k, l in infos if l not in (None, -1)]
        if not infos: continue
        t = top(pkg, i)
        index = flow_index(t)
        seen, _ = reach(pkg, i)
        for k, link in infos:
            if link not in index: continue
            stacks = {s for kk, s in seen if kk == k} or {()}
            todo, visited = [(k + 1, s) for s in stacks], set()
            while todo:
                state = todo.pop()
                if state in visited or state[0] >= len(t): continue
                visited.add(state)
                if state[0] == index[link]:
                    yield i, 'the continuation at %d of the latent call in statement %d is reached by falling through it' % (link, t[k].mem)
                    break
                if t[state[0]].op == 0x4E: continue
                todo += [s2 for s2 in successors(t, state[0], state[1], [], index) if s2 is not None and len(s2[1]) <= 64]


def _stored_sites(pkg, i):
    """Spawned actors and added components export i stores in a variable: (statement index, kind, variable node,
    finishes already made). A BeginDeferredActorSpawnFromClass, or an add with a literal True bDeferredFinish, comes
    back unfinished; a FinishSpawningActor(BeginDeferred...) - Objects.h's SpawnActor - or an add with a literal False
    comes back finished (AddComponent / AddComponentByClass run FinishAddComponent themselves, ActorConstruction.cpp
    1132-1135, 1157-1160)."""
    for k, t in enumerate(top(pkg, i)):
        if t.op not in (0x0F, 0x5F) or len(t.kids) < 2: continue
        val = uncast(t.kids[1])
        if val.op in (0x19, 0x1A) and len(val.kids) > 1: val = uncast(val.kids[1])
        name = call_name(pkg, val)
        a = args(val) if name else []
        if name == 'BeginDeferredActorSpawnFromClass':
            yield k, 'spawn', t.kids[0], 0
        elif name == 'FinishSpawningActor' and a and call_name(pkg, uncast(a[0])) == 'BeginDeferredActorSpawnFromClass':
            yield k, 'spawn', t.kids[0], 1
        elif name in ('AddComponentByClass', 'AddComponent'):
            flag = a[3] if name == 'AddComponentByClass' and len(a) >= 4 else a[4] if name == 'AddComponent' and len(a) >= 5 else None
            if flag is not None and flag.op in (0x27, 0x28):
                yield k, 'component', t.kids[0], 0 if flag.op == 0x27 else 1


FINISH = {'spawn': 'FinishSpawningActor', 'component': 'FinishAddComponent'}


def _finishes(pkg, stmt, kind, var):
    """How many finishes of `var` statement `stmt` makes."""
    return sum(1 for n, on in calls_in(stmt) if call_name(pkg, n) == FINISH[kind] and args(n)
               and same_tree(uncast(args(n)[0]), var))


def _writes(stmt, var):
    return stmt.op in (0x0F, 0x5F) and stmt.kids and same_tree(stmt.kids[0], var)


@rule
def uber_deferred_finish(pkg):
    """A spawned actor or added component is finished at most once: FinishSpawning runs the construction script and
    BeginPlay once and only ensures against a second call (Actor.cpp 3206), and a second FinishAddComponent runs
    OnComponentCreated again past its ensure (ActorComponent.cpp 1359) and re-attaches and re-registers the component
    (ActorConstruction.cpp 1165-1199). Followed statement by statement from where the result is stored until the
    variable is assigned again, a latent call's thread continuing at its Linkage; explored without values, so two
    finishes under mutually exclusive conditions would read as one path through both (no game or suite package has
    that shape).
    Not checked, as the engine accepts it (see the report's skipped list): a spawn never finished, or finished in
    another function (the actor just never runs construction or BeginPlay), a finish at another transform than the
    begin's (recomposed, Actor.cpp 3210-3232), a component finished by another actor expression (one actor can be
    named two ways). The editor's nodes always pair them one to one (K2Node_SpawnActorFromClass.cpp 412-435,
    K2Node_AddComponent.cpp 463-507); LATENT's pending DeferredLeft asks the compiler to warn about the rest."""
    for i, st in functions(pkg):
        sites = list(_stored_sites(pkg, i))
        if not sites: continue
        t = top(pkg, i)
        index = flow_index(t)
        seen, _ = reach(pkg, i)
        prologue = ()
        for s in t:
            if s.op == 0x4C and s.ops[0][1] in index: prologue += (index[s.ops[0][1]],)
            elif s.op not in PRE_DISPATCH: break
        latent = {}
        for k, n in latent_infos(pkg, i):
            link = linkage_of(pkg, i, n)
            if link is not None and link != -1 and link in index: latent[k] = [(index[link], prologue)]
        for k, kind, var, done in sites:
            stacks = {s for kk, s in seen if kk == k} or {()}
            todo, visited, again = [(k + 1, s, done) for s in stacks], set(), None
            while todo and again is None:
                state = todo.pop()
                j, stack, count = state
                if state in visited or j >= len(t) or j == k: continue    # the site again, round a loop: a new one
                visited.add(state)
                s = t[j]
                count += _finishes(pkg, s, kind, var)
                if count > 1: again = s.mem; break
                if _writes(s, var): continue                                  # assigned again: this one ends
                todo += [(s2[0], s2[1], count) for s2 in successors(t, j, stack, [], index, latent)
                         if s2 is not None and len(s2[1]) <= 64]
            if again is not None:
                yield i, '%s stored at mem %d is finished again at mem %d on one path' % (
                    'the actor' if kind == 'spawn' else 'the component', t[k].mem, again)


def class_flags_of(pkg, idx):
    """ClassFlags of the class an FPackageIndex names, read off its defining package; None for a /Script class."""
    r = pkg.resolve(idx)
    if not r: return None
    st = r[0].struct(r[1])
    return st.class_flags if st is not None and hasattr(st, 'class_flags') else None


SPAWNS = {'SpawnObject': 0, 'AddComponentByClass': 0}


def object_literal(pkg, i, n):
    """The FPackageIndex an operand names when it is an EX_ObjectConst, or a local of export i that one EX_Let /
    EX_LetObj of an EX_ObjectConst - and nothing else - assigns (an inline helper's parameter); else None."""
    if n.op == 0x20: return n.ops[0][1]
    if n.op != 0x00: return None
    lets = [s for s in top(pkg, i) if s.op in (0x0F, 0x5F) and s.kids and same_tree(s.kids[0], n)]
    return lets[0].kids[1].ops[0][1] if len(lets) == 1 and len(lets[0].kids) > 1 and lets[0].kids[1].op == 0x20 else None


def worldless(pkg, idx):
    """Whether the object an FPackageIndex names is a class default object or a package's top-level object (an asset,
    a class): no actor in a world. A level's actor (outered to its PersistentLevel) is not."""
    o = pkg.obj(idx)
    if o is None: return False
    if o['name'].startswith('Default__'): return True
    if idx > 0: return o['outer'] == 0
    return o['outer'] < 0 and pkg.obj(o['outer'])['class_name'] == 'Package'


@rule
def uber_spawn_class_operands(pkg):
    """A literal class constructed by SpawnObject or added by AddComponentByClass - an EX_ObjectConst, or a local only
    ever assigned one (an inline helper's parameter, as Objects.h's AddComponentByType) - is not CLASS_Abstract (0x1):
    both end in NewObject, and StaticAllocateObject checks against it - 'it is illegal to create an abstract class,
    except the CDO' (UObjectGlobals.cpp 2362; GameplayStatics.cpp 626; ActorConstruction.cpp 1154) - which a Shipping
    build compiles out and so creates one. AddComponent and AddComponentByClass never run under an EX_Context on an
    EX_ObjectConst of a class default object or an asset: both read GetWorld()->bIsTearingDown unchecked
    (ActorConstruction.cpp 1093-1094, 1147-1148), and neither is an actor in a world.
    Not checked, as the engine handles it (see the report's skipped list): an abstract or deprecated class spawned as
    an actor (SpawnActor warns and returns None, LevelActor.cpp 333-342), a deprecated one constructed (nothing
    checks it), SpawnObject's null Outer (a warning and None, GameplayStatics.cpp 614-618); LATENT's pending
    SpawnAbstract asks the compiler to warn about those. /Script classes are skipped: import rows carry no flags."""
    for i, st in functions(pkg):
        for s in top(pkg, i):
            for n in s.walk():
                if n.op in (0x19, 0x1A) and len(n.kids) > 1 and n.kids[1].op in CALLS and n.kids[0].op == 0x20 \
                        and call_name(pkg, n.kids[1]) in ('AddComponentByClass', 'AddComponent') \
                        and worldless(pkg, n.kids[0].ops[0][1]):
                    yield i, '%s at mem %d runs on the object constant %s' % (call_name(pkg, n.kids[1]), n.mem,
                                                                          pkg.path(n.kids[0].ops[0][1]))
                name = call_name(pkg, n)
                if name not in SPAWNS: continue
                a = args(n)
                if len(a) <= SPAWNS[name]: continue
                cls = object_literal(pkg, i, a[SPAWNS[name]])
                flags = class_flags_of(pkg, cls) if cls else None
                if flags is not None and flags & 0x1:
                    yield i, '%s at mem %d: %s is abstract' % (name, n.mem, pkg.path(cls))


MINE = [n for n, f in invariants.RULES.items() if f.__module__ == __name__]
