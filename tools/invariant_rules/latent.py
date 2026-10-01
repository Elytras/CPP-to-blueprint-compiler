import sys
from invariants import *
import invariants

# ---- LATENT: async-action proxies (K2Node_BaseAsyncTask's expansion) and latent calls' world context.
#
# An async-action proxy, as these rules find one: a local of a function, assigned from a call to a native function
# (the factory), one of whose multicast dispatchers the function binds (EX_AddMulticastDelegate with the local as the
# context) and which it activates (a native call named Activate with no argument, on the local) -
# UBlueprintAsyncActionBase::Activate; a component's Activate(bool bReset) takes one. A callback proxy is also any
# local assigned from one of FRAME_FACTORIES, the editor's async-task factories whose proxy has no Activate. Narrowed to native factories and a native Activate by calibration (see
# latent_native_call): the game's only finding before was a widget Blueprint's own Activate function.

CALLS = (0x1B, 0x1C, 0x45, 0x46, 0x68)
UBERGRAPH = 0x8000                      # FUNC_UbergraphFunction

# Callback-proxy factories whose proxy carries RF_StrongRefOnFrame and that nothing else roots: NewObject in the
# transient package, engine delegates bound with BindUObject / AddDynamic (weak). Only the persistent frame keeps it.
FRAME_ONLY = {
    'CreateProxyObjectForPlayMontage',              # PlayMontageCallbackProxy.cpp 15-23, 56-63
    'CreatePlayAnimationProxyObject',               # Animation/WidgetAnimationPlayCallbackProxy.cpp 10-13, 43
    'CreatePlayAnimationTimeRangeProxyObject',      # Animation/WidgetAnimationPlayCallbackProxy.cpp 18-21, 61
}
# The callback-proxy factories of the editor's async-task nodes that have no Activate. CreateMoveToProxyObject's proxy
# is frame-flagged too, but the AI system also holds it (AIBlueprintHelperLibrary.cpp 36-40, AISystem.h 112, 177),
# so it is a proxy for latent_async_proxy_validated and not for latent_proxy_frame_held.
FRAME_FACTORIES = FRAME_ONLY | {'CreateMoveToProxyObject'}

# Operands that cannot be an object: a latent call's world context given one of these finds no world.
NOT_OBJECTS = {0x1D: 'an int', 0x1E: 'a float', 0x1F: 'a string', 0x21: 'a name', 0x22: 'a rotator', 0x23: 'a vector',
               0x24: 'a byte', 0x25: 'an int', 0x26: 'an int', 0x27: 'a bool', 0x28: 'a bool', 0x2A: 'None',
               0x2B: 'a transform', 0x2C: 'an int', 0x2F: 'a struct', 0x34: 'a string', 0x35: 'an int64',
               0x36: 'a uint64', 0x29: 'a text'}


def latent_call_name(pkg, n):
    """The name of the function a call node calls: the name itself for a virtual call, the import's or export's
    object name otherwise. None for anything but a call."""
    if n.op not in CALLS or not n.ops: return None
    kind, v = n.ops[0][:2]
    if kind == 'name': return v
    return pkg.obj(v)['name'] if kind == 'obj' and v else None


def latent_args(n):
    return [k for k in n.kids if k.op != 0x16]


def latent_native_call(pkg, n):
    """A call to a native function: EX_CallMath, EX_VirtualFunction by name (a script function is called with
    EX_LocalVirtualFunction / EX_LocalFinalFunction), or EX_FinalFunction of a /Script import. Calibration: the game
    calls a widget Blueprint's own zero-parameter 'Activate' (HUD_DamageClass_Indicator, EX_LocalVirtualFunction on a
    local its own CreateIcon returned) and binds one of its dispatchers - no async action."""
    if n.op in (0x68, 0x1B): return True
    return n.op == 0x1C and n.ops[0][0] == 'obj' and n.ops[0][1] < 0 and (pkg.path(n.ops[0][1]) or '').startswith('/Script/')


def latent_local(n):
    """An EX_LocalVariable's (property path, owner) - the variable's identity - else None."""
    if n.op != 0x00 or not n.ops: return None
    _, segs, owner = n.ops[0]
    return '.'.join(segs), owner


def latent_uncontext(n):
    """(object operand, the expression it runs) of an EX_Context / EX_Context_FailSilent, else (None, n)."""
    if n.op in (0x19, 0x1A) and len(n.kids) > 1: return n.kids[0], n.kids[1]
    return None, n


def latent_int(pkg, i, n):
    """The value of an int literal operand of export i (IntConst, IntZero, IntOne, IntConstByte, SkipOffsetConst)."""
    if n.op in (0x1D, 0x5B): return n.ops[0][1]
    if n.op in (0x25, 0x26): return n.op - 0x25
    if n.op == 0x2C: return pkg.blob(i)[n.disk + 1]
    return None


def latent_events(n, out):
    """The proxy events of one statement's expression, in the order the VM runs them: ('assign', X, factory name),
    ('bind', X, handler name), ('activate', X), ('test', X) for a validity test of X. A Let's value runs before its
    destination is written (execLet steps the destination first, but only to find it)."""
    if n.op in (0x0F, 0x5F, 0x14) and len(n.kids) == 2:
        latent_events(n.kids[1], out)
        x = latent_local(n.kids[0])
        obj, val = latent_uncontext(n.kids[1])
        if x and val.op in CALLS: out.append(('assign', x, val))
        return out
    if n.op == 0x5C and len(n.kids) == 2:
        obj, _ = latent_uncontext(n.kids[0])
        x = latent_local(obj) if obj is not None else None
        handler = n.kids[1].ops[0][1] if n.kids[1].op == 0x4B else None
        for k in n.kids: latent_events(k, out)
        if x: out.append(('bind', x, handler))
        return out
    for k in n.kids: latent_events(k, out)
    obj, val = latent_uncontext(n)
    if obj is not None and val.op in CALLS and not latent_args(val) and latent_local(obj):
        out.append(('activate', latent_local(obj), val))
    if n.op in CALLS or n.op == 0x38:
        for a in (latent_args(n) if n.op in CALLS else n.kids):
            if latent_local(a): out.append(('test', latent_local(a), n))
    return out


def latent_proxies(pkg, i):
    """The async-action proxies of function export i: {X: dict(assign=[...], bind=[...], activate=[...], test=[...],
    factories={names})}, statement indices per event; X is in only if the function assigns it from a call and binds it,
    and activates it or assigns it from a FRAME_FACTORIES call."""
    seen = {}
    for k, t in enumerate(pkg.script(i)):
        for ev in latent_events(t, []):
            kind, x = ev[0], ev[1]
            if x[1] != i + 1: continue                  # a local of this function only
            d = seen.setdefault(x, dict(assign=[], bind=[], activate=[], test=[], factories=set(), handlers=set()))
            if kind == 'activate':
                if latent_call_name(pkg, ev[2]) != 'Activate' or not latent_native_call(pkg, ev[2]): continue
            elif kind == 'assign':
                if not latent_native_call(pkg, ev[2]): continue
            elif kind == 'test':
                name = latent_call_name(pkg, ev[2]) if ev[2].op in CALLS else 'cast'
                if name not in ('IsValid', 'NotEqual_ObjectObject', 'EqualEqual_ObjectObject', 'cast'): continue
            d[kind].append(k)
            if kind == 'assign': d['factories'].add(latent_call_name(pkg, ev[2]))
            if kind == 'bind' and ev[2]: d['handlers'].add(ev[2])
    return {x: d for x, d in seen.items()
            if d['assign'] and d['bind'] and (d['activate'] or d['factories'] & FRAME_FACTORIES)}


def latent_flags(pkg, i):
    st = pkg.struct(i)
    return getattr(st, 'function_flags', 0)


@rule
def latent_async_proxy_validated(pkg):
    """An async-task proxy bound in the ubergraph is tested before its dispatchers are bound. This is the editor's
    expansion, not an engine requirement: K2Node_BaseAsyncTask calls the factory, runs UKismetSystemLibrary::IsValid
    on the result and binds the delegates and calls Activate only on its true branch (K2Node_BaseAsyncTask.cpp
    393-408, 440-448), and every async-task proxy of the game keeps the test. Without it a None proxy (from
    CreateMoveToProxyObject with no pawn or no AI controller, AIBlueprintHelperLibrary.cpp 107-135) costs an
    'Accessed None' script warning per bind and per Activate (ScriptCore.cpp 2904-2937) and binds nothing
    (execAddMulticastDelegate, 3085-3101); the waiting method stays parked either way. Only the ubergraph is checked: the editor places async-task nodes there
    alone (K2Node_BaseAsyncTask.cpp 54-64) and AssetGen's UE_AWAIT lowers there, while a bind the mod writes itself in
    a plain function is the author's own unchecked pointer. Here: a validity test of the proxy (IsValid, an object
    comparison, or an object-to-bool cast) in a statement before its first bind."""
    names = set(pkg.names)
    if 'Activate' not in names and not names & FRAME_FACTORIES: return
    for i, st in functions(pkg):
        if not latent_flags(pkg, i) & UBERGRAPH: continue
        for x, d in latent_proxies(pkg, i).items():
            first = min(d['bind'])
            if not any(k < first for k in d['test']):
                yield i, 'proxy %s (from %s) is bound at statement %d with no validity test before it' % (
                    x[0], '/'.join(sorted(d['factories'])), first)


def latent_ubergraph_entries(pkg, i):
    """Where ubergraph export i is entered, by the function that enters it: {function name: {offsets}} - the int
    literal a call to it passes (an event stub, a handler, AssetGen's method stub), or a stub's EventGraphCallOffset
    (the fast path, ScriptCore.cpp 1923-1937)."""
    out = {}
    uname = pkg.exports[i]['name']
    for j, st in functions(pkg):
        if pkg.exports[j]['outer'] != pkg.exports[i]['outer']: continue
        if getattr(st, 'event_graph', 0) == i + 1:
            out.setdefault(pkg.exports[j]['name'], set()).add(st.event_graph_offset)
        if j == i: continue
        for n in statements(pkg, j)[0]:
            if n.op not in CALLS or not latent_args(n): continue
            kind, v = n.ops[0][:2]
            if (kind == 'obj' and v == i + 1) or (kind == 'name' and v == uname):
                e = latent_int(pkg, j, latent_args(n)[0])
                if e is not None: out.setdefault(pkg.exports[j]['name'], set()).add(e)
    return out


def latent_links(pkg, i, t):
    """{statement index: [resume offsets]} of the latent calls that resume (a LatentActionInfo literal whose Linkage
    is an int other than -1): the thread ends there and FLatentActionManager runs the ubergraph at the Linkage later."""
    out = {}
    for k, s in enumerate(t):
        for n in s.walk():
            if n.op == 0x2F and n.ops and n.ops[0][0] == 'obj' and (pkg.path(n.ops[0][1]) or '').endswith('LatentActionInfo'):
                m = [c for c in n.kids if c.op != 0x30]
                link = latent_int(pkg, i, m[0]) if m else None
                if link is not None and link != -1: out.setdefault(k, []).append(link)
    return out


NONE, MADE, BOUND, ACTIVE = 'none', 'made', 'bound', 'active'


@rule
def latent_async_activated_once(pkg):
    """An async-action proxy is activated once, after its dispatchers are bound, on every path that waits on it.
    Activate is the action's 'the delegates are bound, start' call (BlueprintAsyncActionBase.h 26-28) and may
    broadcast before it returns (AsyncActionLoadPrimaryAsset.cpp 6-35: a load already done completes at once), so a
    bind after it misses that broadcast; an action whose work starts in Activate never broadcasts without it; and a
    second Activate starts the work again (the same function reloads and broadcasts again). The editor's node
    guarantees all three by construction - factory, binds, Activate on one exec chain, a new proxy per run
    (K2Node_BaseAsyncTask.cpp 410-438) - and the game keeps it. Followed statement by statement from each entry of the
    function (the ubergraph's: every offset a stub or handler enters it at), a latent call continuing at its Linkage,
    and a thread that ends with the proxy active continuing at the entries of the handlers bound on it (the await's
    resume). The walk is path-insensitive: a lowering that guarded Activate with a run-time flag would read here as a
    second Activate. Findings: Activate with no bind since the proxy was assigned; Activate on a proxy already active
    and not assigned again; a thread that binds the proxy and ends without activating it."""
    if 'Activate' not in set(pkg.names): return
    for i, st in functions(pkg):
        for x, msgs, seen in latent_proxy_flow(pkg, i):
            for m in sorted(msgs): yield i, m


def latent_proxy_flow(pkg, i):
    """latent_async_activated_once's walk of function export i: (proxy, findings, {(Activate's statement index, the
    proxy's state as it ran)}) per activated proxy - the last so a calibration can show the walk reached them."""
    proxies = {x: d for x, d in latent_proxies(pkg, i).items() if d['activate']}
    if proxies:
        t = pkg.script(i)
        index = {n.mem: k for k, n in enumerate(t)}
        uber = latent_flags(pkg, i) & UBERGRAPH
        entries = latent_ubergraph_entries(pkg, i) if uber else {}
        prologue = ()
        for s in t:
            if s.op == 0x4C and s.ops[0][1] in index: prologue += (index[s.ops[0][1]],)
            elif s.op == 0x4E: break
            elif s.op not in (0x5E, 0x6A, 0x0B): prologue = (); break
        roots = sorted({index[e] for es in entries.values() for e in es if e in index}) if uber else [0]
        links = {k: [index[l] for l in ls if l in index] for k, ls in latent_links(pkg, i, t).items()}
        events = [latent_events(s, []) for s in t]
        for x, d in proxies.items():
            resume = sorted({index[e] for h in d['handlers'] for e in entries.get(h, ()) if e in index})
            msgs, ran = set(), set()
            todo = [(r, prologue, NONE) for r in roots]
            visited = set()
            while todo:
                state = todo.pop()
                if state in visited or state[0] >= len(t) or len(state[1]) > 64: continue
                visited.add(state)
                k, stack, xs = state
                s = t[k]
                for ev in events[k]:
                    if ev[1] != x: continue
                    if ev[0] == 'assign': xs = MADE
                    elif ev[0] == 'bind' and xs == MADE: xs = BOUND
                    elif ev[0] == 'activate' and latent_call_name(pkg, ev[2]) == 'Activate' and latent_native_call(pkg, ev[2]):
                        ran.add((k, xs))
                        if xs == MADE: msgs.add('Activate at mem %d runs before any dispatcher of %s is bound' % (s.mem, x[0]))
                        elif xs == ACTIVE: msgs.add('Activate at mem %d runs on %s a second time, without assigning it again' % (s.mem, x[0]))
                        if xs != NONE: xs = ACTIVE
                ends, nxt = False, []
                o = s.op
                if o == 0x06: nxt = [(index.get(s.ops[0][1]), stack)]
                elif o == 0x07: nxt = [(k + 1, stack), (index.get(s.ops[0][1]), stack)]
                elif o == 0x4C: nxt = [(k + 1, stack + (index.get(s.ops[0][1]),))]
                elif o == 0x4D:
                    if stack: nxt = [(stack[-1], stack[:-1])]
                    else: ends = True
                elif o == 0x4F:
                    nxt = [(k + 1, stack)]
                    if stack: nxt.append((stack[-1], stack[:-1]))
                    else: ends = True
                elif o in (0x04, 0x53, 0x4E): ends = True
                else: nxt = [(k + 1, stack)]
                if k in links:                                  # the call's thread ends; its continuation runs later
                    ends = ends or not nxt
                    todo += [(l, prologue, xs) for l in links[k]]
                if ends:
                    if xs == BOUND: msgs.add('a thread binds %s and ends at mem %d without activating it' % (x[0], s.mem))
                    if xs == ACTIVE: todo += [(r, prologue, ACTIVE) for r in resume]
                todo += [(j, stk, xs) for j, stk in nxt if j is not None]
            yield x, msgs, ran


@rule
def latent_proxy_frame_held(pkg):
    """A callback proxy only the persistent frame keeps alive is held in a member or in a local of the ubergraph, never
    only in a local of a function that does not wait. Its factory (FRAME_ONLY) makes it in the transient package with
    RF_StrongRefOnFrame and roots it nowhere else, the engine's delegates reaching it weakly (PlayMontageCallbackProxy.cpp
    15-23, 56-63; Animation/WidgetAnimationPlayCallbackProxy.cpp 10-13, 18-21, 43, 61); the GC sees the ubergraph frame through
    the class (BlueprintGeneratedClass.cpp 1683-1713) and treats an RF_StrongRefOnFrame object there as a strong
    reference (UObjectGlobals.cpp 3460-3483), while a plain function's locals die with its call - after it returns the
    proxy is collectable and its dispatchers never fire. Async actions are left out: whether one roots itself is up to
    its factory (AsyncTaskDownloadImage.cpp 56-63 AddToRoot, BlueprintAsyncActionBase.cpp 23-46
    RegisterWithGameInstance), which the package does not show; so is CreateMoveToProxyObject's, which the AI system
    holds. Findings: such a proxy assigned to a local of a function without FUNC_UbergraphFunction, and copied from
    there into no member and no frame slot."""
    names = set(pkg.names)
    if not names & FRAME_ONLY: return
    for i, st in functions(pkg):
        if latent_flags(pkg, i) & UBERGRAPH: continue
        proxies = {x: d for x, d in latent_proxies(pkg, i).items() if d['factories'] & FRAME_ONLY}
        if not proxies: continue
        kept = set()
        for n in statements(pkg, i)[0]:
            if n.op in (0x0F, 0x5F) and len(n.kids) == 2 and n.kids[0].op == 0x01 and latent_local(n.kids[1]):
                kept.add(latent_local(n.kids[1]))
            if n.op == 0x64 and n.kids and latent_local(n.kids[0]):
                kept.add(latent_local(n.kids[0]))
        for x, d in proxies.items():
            if x not in kept:
                yield i, 'proxy %s (from %s) is kept only in a local of this function, which does not wait: nothing ' \
                         'references it once the function returns' % (x[0], '/'.join(sorted(d['factories'] & FRAME_ONLY)))


LATENT_LIBRARIES = ('/Script/Engine.KismetSystemLibrary:', '/Script/Engine.GameplayStatics:')


@rule
def latent_world_context_object(pkg):
    """A latent library call's first argument - the world context (Delay, RetriggerableDelay, LoadAsset,
    LoadAssetClass, LoadStreamLevel, ...) or MoveComponentTo's component - is an object expression, never None nor a
    literal of another type: the call finds its world through it (KismetSystemLibrary.cpp 2166, 2179, 2197, 2658,
    2687; GameplayStatics.cpp 701, 720 for Load/UnloadStreamLevel) and GetWorldFromContextObject answers None for a null object
    (UnrealEngine.cpp 11320-11346), so the action is never added and the waiting method never resumes; a literal of
    another type is read as a pointer. Here: a call to a KismetSystemLibrary / GameplayStatics function that passes a
    LatentActionInfo literal - every one of them takes the world context (or MoveComponentTo's component) first."""
    if 'LatentActionInfo' not in set(pkg.names): return
    for i, st in functions(pkg):
        for n in statements(pkg, i)[0]:
            if n.op not in (0x1C, 0x68) or n.ops[0][0] != 'obj': continue
            path = pkg.path(n.ops[0][1]) or ''
            if not path.startswith(LATENT_LIBRARIES): continue
            a = latent_args(n)
            if not any(k.op == 0x2F and k.ops and k.ops[0][0] == 'obj'
                       and (pkg.path(k.ops[0][1]) or '').endswith('LatentActionInfo') for k in a): continue
            if a and a[0].op in NOT_OBJECTS:
                yield i, '%s at mem %d gets %s as its world context' % (path.split(':')[-1], n.mem, NOT_OBJECTS[a[0].op])


MINE = [n for n, f in invariants.RULES.items() if f.__module__ == __name__]
