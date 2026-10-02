#!/usr/bin/env python3
"""A class run the way the Blueprint VM runs it, for what runscript.py alone cannot: latent calls and the ubergraph's
persistent frame, delegate binds and broadcasts, calls and sets on other objects. Engine calls are logged and answered
by a test's fakes. Importing this extends runscript's parser with the opcodes those bodies use."""
import copy
import os
import dumpexp
import runscript
from runscript import Node, P

_parse = P.node


def _ref(s):
    """An object reference by bare name: exp[0]:SpawnTest_C -> SpawnTest_C, imp[1]:Class'Actor' -> Actor."""
    p = s.ptr().split(':', 1)[-1]
    return p.split("'")[1] if "'" in p else p


def _node(s):
    """runscript's parser plus what latent / async / spawn bodies use. Nodes runscript already read keep its val."""
    op, mem = s.b[s.o], s.mem
    if op not in (0, 0x48, 0x19, 0x20, 0x2E, 0x2F, 0x30, 0x4B, 0x5B, 0x5C, 0x63, 0x64, 0x44, 0x5D, 0x61, 0x62):
        return _parse(s)                                                     # final calls too: runscript marks an export callee own
    s.u8()
    n = Node(op, mem)
    if op in (0, 0x48, 0x64): n.val, n.owner = s.fieldpath().split('@')
    if op == 0x19: n.kids.append(s.node()); s.i32(); n.rvalue, n.rvalue_owner = s.fieldpath().split('@'); n.kids.append(s.node())
    elif op in (0x20, 0x2E, 0x2F): n.val = _ref(s)
    elif op == 0x63: n.val = _ref(s); s.args(n.kids)                        # the signature, the dispatcher, its args
    elif op == 0x4B: n.val = s.name()
    elif op == 0x5B: n.val = s.i32()
    elif op == 0x5C: n.kids += [s.node(), s.node()]
    elif op in (0x62, 0x44): n.kids += [s.node(), s.node()]                  # RemoveMulticastDelegate; LetDelegate
    elif op == 0x5D: n.kids.append(s.node())                                 # ClearMulticastDelegate
    elif op == 0x61: n.val = s.name(); n.kids += [s.node(), s.node()]        # BindDelegate: name, variable, object
    if op in (0x2E, 0x64): n.kids.append(s.node())
    if op == 0x2F:
        s.i32()
        while True:
            k = s.node()
            if k.op == 0x30: break
            n.kids.append(k)
    return n


P.node = _node
_parse_casts = P.node


def _node_casts(s):
    """The casts runscript's parser leaves out: EX_PrimitiveCast (its cast byte), EX_InterfaceContext, and the
    interface casts EX_ObjToInterfaceCast / EX_CrossInterfaceCast / EX_InterfaceToObjCast (their class by bare name)."""
    op, mem = s.b[s.o], s.mem
    if op not in (0x38, 0x51, 0x52, 0x54, 0x55): return _parse_casts(s)
    s.u8()
    n = Node(op, mem)
    if op == 0x38: n.val = s.u8()
    elif op != 0x51: n.val = _ref(s)
    n.kids.append(s.node())
    return n


P.node = _node_casts


class Struct(list):
    def __init__(s, name, vals): super().__init__(vals); s.name = name


class Written(dict):
    """What an EX_StructConst writes, member name -> value, when VM.struct_const names its members. execStructConst
    steps each literal into its member of the destination itself (ScriptCore.cpp 3376-3405), so an EX_Let of one
    leaves a member it does not write (a Transient one) as the destination had it."""


def by_value(v):
    """v as a variable takes it: FProperty::CopyCompleteValue copies a struct or container (execLet,
    ScriptCore.cpp 2647-2686), so its dicts and lists are new, the objects they hold the same."""
    if not isinstance(v, (dict, list)): return v
    c = copy.copy(v)
    for k in (list(c) if isinstance(c, dict) else range(len(c))): c[k] = by_value(c[k])
    return c


class Obj:
    """An object the VM hands around: class name, variables, outer (OuterPrivate) and a fake address."""
    _next = 0x10000

    def __init__(s, cls, outer=None, **vars):
        s.cls, s.vars, s.outer = cls, dict(vars), outer
        Obj._next += 0x1000
        s.addr = Obj._next

    def __repr__(s): return '<%s>' % s.cls


class _Stale:
    def __repr__(s): return '<stale: the destination keeps its previous value>'


STALE = _Stale()     # what a None context with no r-value writes: nothing (VM.null_rvalues)
_CLEARED = {'IntProperty': 0, 'Int64Property': 0, 'ByteProperty': 0, 'EnumProperty': 0, 'FloatProperty': 0.0,
            'DoubleProperty': 0.0, 'BoolProperty': False, 'StrProperty': '', 'NameProperty': 'None', 'TextProperty': ''}


def cleared(ftype):
    """FProperty::ClearValue of a property class, as this VM holds values: zero, empty, or None for an object."""
    if ftype in ('ArrayProperty', 'SetProperty'): return []
    if ftype in ('MapProperty', 'StructProperty'): return {}
    return _CLEARED.get(ftype)


class VM:
    """One cooked class run the way the VM would, for what runscript cannot: latent calls, the ubergraph's persistent
    frame, delegate binds, calls and sets on other objects. Engine calls are logged as (name, context, args) and
    answered by `natives`; a test fires the recorded latent actions / broadcasts the bound delegates itself. `objects`
    are what an ObjectConst names, by object name (a global's Default__<Ns>__<Name>_C); two VMs sharing one see one."""
    def __init__(s, base, natives=None, isa=None, objects=None, **self_vars):
        s.base, s.natives, s.objects = base, dict(natives or {}), objects if objects is not None else {}
        s.exports = {e['name'] for e in dumpexp.load(base)[5]}
        s.self = Obj(os.path.basename(base) + '_C', **self_vars)
        s.frames, s.latent, s.binds, s.log, s.scripts, s.mem = {}, [], [], [], {}, {}
        s.isa = isa or (lambda o, cls: isinstance(o, Obj) and o.cls == cls)
        s.null_rvalues = False      # True: an EX_Context on None does what ProcessContextOpcode does (none_context)
        s.struct_const = None       # set: (struct name, literal values) -> Written, the members an EX_StructConst writes
        s.accessed_none = []        # (function, mem) of each EX_Context run on None: an 'Accessed None' script warning
        s.ref_params = False        # True: a script callee's reference parameters write back into their arguments (ref_writeback)
        s.env_log = []              # with ref_params, each call's frame as it starts, so ref_writeback finds the callee's
        s.classes = {}              # another class's name -> the base of its package: what runs on an object of it (peer)
        s._peers = {}

    def new(s, **vars): return Obj(s.self.cls, **vars)

    def peer(s, cls):
        """The VM of the package of class `cls` (one of `classes`), sharing this one's objects, bindings, latent actions,
        log and Accessed None list: a call on an object of that class runs that class's function, as a broadcast or a
        timer finds a bound name on the bound object's own class (FindFunction, ScriptDelegates.h 38-49, 479-502)."""
        v = s._peers.get(cls)
        if v is None:
            v = s._peers[cls] = VM(s.classes[cls], s.natives, s.isa, s.objects)
            v.binds, v.latent, v.log, v.accessed_none = s.binds, s.latent, s.log, s.accessed_none
            v.classes, v._peers, v.null_rvalues, v.struct_const = s.classes, s._peers, s.null_rvalues, s.struct_const
        return v

    def ref_writeback(s, fn, args, env, store):
        """A script callee's reference parameter is its argument's own variable: ProcessScriptFunction hands the callee an
        FOutParmRec at the address the argument left (ScriptCore.cpp 865-890). What the callee left in each is stored
        back through its argument, which runs on the caller's frame object like every argument (868)."""
        outs = runscript.params_of(s.base, fn, 0x100)
        for parm, a in zip(s.script(fn)[2], args):
            if parm in outs and parm in env: store(a, env[parm])

    def frame(s, me, uber):
        """The persistent frame of ubergraph `uber` on object me: one per object here; a subclass running a class chain
        keys it by the ubergraph too, as GetPersistentUberGraphFrame keeps one per class (BlueprintGeneratedClass.cpp)."""
        return s.frames.setdefault(id(me), {})

    def none_context(s, n, keep):
        """EX_Context on a None object with null_rvalues on, as ProcessContextOpcode runs it (ScriptCore.cpp 2940-2953):
        the r-value property is cleared in the result slot, so the destination reads zero; with no r-value nothing is
        written - STALE, which a Let or EX_Return (keep) passes on, so their destination keeps its previous value.
        Anywhere else STALE reads as None, the zeroed slot a parameter or a context object starts as."""
        if not n.rvalue: return STALE if keep else None
        owner = n.rvalue_owner.split(':', 1)[-1] if n.rvalue_owner.startswith('exp[') else None
        types = runscript.props_of(s.base, owner) if owner in s.exports else {}
        return cleared(types.get(n.rvalue.split('.')[-1]))

    def script(s, fn):
        if fn not in s.scripts:
            stmts = runscript.script_of(s.base, fn)
            s.scripts[fn] = (stmts, {n.mem: i for i, n in enumerate(stmts)}, runscript.params_of(s.base, fn))
        return s.scripts[fn]

    def call(s, fn, *args, on=None, **parms):
        me = on or s.self
        if isinstance(me, Obj) and me.cls != s.self.cls and me.cls in s.classes:
            return s.peer(me.cls).call(fn, *args, on=me, **parms)
        stmts, at, names = s.script(fn)
        env = s.frame(me, fn) if fn.startswith('ExecuteUbergraph_') else {}   # the persistent frame
        if not fn.startswith('ExecuteUbergraph_'):
            # A frame starts zeroed, its locals constructed under FUNC_HasDefaults (ScriptCore.cpp 909-916): a struct
            # local is a struct of zeros, or what InitializeValue leaves in it (runscript.frame_defaults: FTransform's
            # identity, FHitResult's Time 1, a UserDefinedStruct's defaults), which a Make Struct's temp holds. A
            # function base has no export of (a class chain's parent's) is left as it was.
            try:
                env.update((p, {}) for p, t in runscript.props_of(s.base, fn).items()
                           if t == 'StructProperty' and p not in names)
                env.update((p, v) for p, v in runscript.frame_defaults(s.base, fn).items()
                           if not isinstance(v, runscript.Unconstructed))
            except StopIteration: pass
        env.update(zip(names, args)); env.update(parms)
        if s.ref_params: s.env_log.append(env)

        def local(n):
            assert n.owner.endswith(':' + fn), '%s uses %s of %s, a property of another function' % (fn, n.val, n.owner)
            return n.val

        def ev(n, ctx=me, keep=False):
            o = n.op
            if o in (0, 0x48): return env.get(local(n), 0)
            if o == 1: return ctx.vars.get(n.val, 0)
            if o == 0x17: return me
            if o == 0x2A: return None
            if o == 0x20: return s.objects.get(n.val, n.val)
            if o in (0x1D, 0x1E, 0x1F, 0x34, 0x24, 0x2C, 0x35, 0x21, 0x5B): return n.val
            if o == 0x67: return ev(n.kids[0])                              # SoftObjectConst: its path
            if o == 0x4B: return ('delegate', n.val, me)                   # EX_InstanceDelegate binds Stack.Object
            if o in (0x25, 0x26): return o - 0x25
            if o in (0x27, 0x28): return o == 0x27
            if o == 0x2F: return (s.struct_const or Struct)(n.val, [ev(k) for k in n.kids])
            if o == 0x2E:
                v = ev(n.kids[0]); return v if s.isa(v, n.val) else None
            if o in (0x52, 0x54, 0x55):                     # an interface cast: the object behind it, if it is of the class
                v = ev(n.kids[0]); return v if v not in (None, 0) and s.isa(v, n.val) else None
            if o == 0x51: return ev(n.kids[0])              # EX_InterfaceContext: the object behind the interface
            if o == 0x38:                                   # CST_ObjectToBool / CST_InterfaceToBool: GetObject() != null
                if n.val not in (0x47, 0x49): raise SystemExit('vm: PrimitiveCast %#x' % n.val)
                return ev(n.kids[0]) not in (None, 0)
            if o == 0x19:
                obj = ev(n.kids[0])
                if obj is None: s.accessed_none.append((fn, n.mem))  # ProcessContextOpcode, ScriptCore.cpp 2904-2937
                if obj is None and s.null_rvalues: return s.none_context(n, keep)
                return None if obj is None else ev(n.kids[1], obj, keep)
            if o == 0x42:                                   # a struct member; raw-pointer reads land here too
                base = ev(n.kids[0])
                if isinstance(base, (runscript.Slot, runscript.Free)): return base[n.val]
                if n.val == '__Slots__': return runscript.slots_of(base)
                if isinstance(base, Obj): s.mem[base.addr] = base; return base.addr     # an object slot as an int64
                if isinstance(base, dict) and n.val not in base and 'Data' in base:      # an array view of an address:
                    return [s.mem[base['Data'] - 0x20].outer]                          # UObjectBase::OuterPrivate
                return base.get(n.val, 0) if isinstance(base, dict) else 0
            if o == 0x6B: return ev(n.kids[0])[ev(n.kids[1])]
            name = n.val
            if o in (0x1B, 0x45, 0x1C, 0x46, 0x68) and name in s.exports and (ctx is me or ctx.cls == s.self.cls):
                # A reference argument is stepped with no result buffer (ProcessScriptFunction): it must be a variable.
                for parm, a in zip(s.script(name)[2], n.kids):
                    if parm in runscript.params_of(s.base, name, 0x100) and a.op not in runscript.ADDRESSABLE \
                            and not (a.op in (0x19, 0x1A) and a.kids[1].op in runscript.ADDRESSABLE):
                        raise SystemExit('vm: %s: reference parameter %s gets a non-variable (op %02x), which crashes the VM' % (name, parm, a.op))
                # A reference argument the callee reads through its address is read once every argument has run, as
                # runscript reads it (read_late): a sibling may write it first.
                outs = runscript.params_of(s.base, name, 0x100)
                bound = [i < len(s.script(name)[2]) and s.script(name)[2][i] in outs and runscript.read_late(a)
                         for i, a in enumerate(n.kids)]
                vals = [None if b else ev(a) for b, a in zip(bound, n.kids)]
                vals = [ev(a) if b else v for b, a, v in zip(bound, n.kids, vals)]
                first = len(s.env_log)
                r = s.call(name, *vals, on=ctx)
                if s.ref_params: s.ref_writeback(name, n.kids, s.env_log[first], store)
                return None if r is STALE and not keep else r
            if o in (0x1B, 0x45, 0x1C, 0x46, 0x68):
                runscript.native_refs(n)
                if name.startswith(('Array_', 'Set_', 'Map_')) and n.kids and n.kids[0].op in (0x19, 0x1A) \
                        and ev(n.kids[0].kids[0]) in (None, 0):
                    # The container is a member of a null object, so the thunk finds no container property: it sets
                    # bArrayContextFailed and returns, the rest of its arguments unread (KismetArrayLibrary.h 278-286).
                    # Only an EX_Context rewinds and skips past them (ScriptCore.cpp 2896-2902).
                    if ctx is me: raise SystemExit('vm: %s on a null container outside a context: the VM runs its '
                                                   'arguments as statements, and EX_EndFunctionParms steps back onto '
                                                   'itself forever (ScriptCore.cpp 2366-2370)' % name)
                    return None
                if name in runscript.CONTAINERS: return runscript.CONTAINERS[name](ev, store, n.kids)
                if name in runscript.MATH: return runscript.MATH[name](*[ev(a) for a in n.kids])
                vals = [ev(a) for a in n.kids]
                s.log.append((name, ctx, vals))
                if name in s.natives: return s.natives[name](s, ctx, *vals)
                return vals[0] not in (None, 0) if name == 'IsValid' else None
            raise SystemExit('vm: unsupported expression %02x in %s' % (o, fn))

        def store(dest, v, ctx=me):
            if dest.op in (0, 0x48): env[local(dest)] = v
            elif dest.op == 1: ctx.vars[dest.val] = v; s.log.append(('set', ctx, dest.val))
            elif dest.op == 0x6B: ev(dest.kids[0])[ev(dest.kids[1])] = v; s.log.append(('set', ctx, dest.kids[0].val))
            elif dest.op == 0x42:
                if not isinstance(ev(dest.kids[0]), (dict, runscript.Slot, runscript.Free)): store(dest.kids[0], {})
                ev(dest.kids[0])[dest.val] = v
            elif dest.op == 0x19:
                obj = ev(dest.kids[0])
                if obj is not None: store(dest.kids[1], v, obj)
            else: raise SystemExit('vm: unsupported destination %02x' % dest.op)

        def locate(dest, ctx=me):
            # execLet steps the destination (a context's object, an array and its index) before the value.
            if dest.op == 0x19:
                obj = ev(dest.kids[0], ctx)
                return lambda v: obj is not None and store(dest.kids[1], v, obj)
            if dest.op == 0x6B:
                arr, i = ev(dest.kids[0], ctx), ev(dest.kids[1], ctx)
                return lambda v: (arr.__setitem__(i, v), s.log.append(('set', ctx, dest.kids[0].val)))
            st = ev(dest.kids[0], ctx) if dest.op == 0x42 else None
            if isinstance(st, (dict, runscript.Slot, runscript.Free)): return lambda v: st.__setitem__(dest.val, v)
            return lambda v: store(dest, v, ctx)

        pc = 0
        for _ in range(100000):
            n = stmts[pc]; o = n.op; pc += 1
            named = o == 0xF and n.kids[1].op == 0x2F and s.struct_const \
                and s.struct_const(n.kids[1].val, list(range(len(n.kids[1].kids))))
            if isinstance(named, Written):
                # execLet hands EX_StructConst the variable's own address (ScriptCore.cpp 2647-2686), and it steps each
                # member straight into it (3376-3405): a member that reads the variable sees the ones written before it.
                put, dest = locate(n.kids[0]), ev(n.kids[0])
                if not isinstance(dest, dict):
                    dest = Written()
                    put(dest)
                for member, k in zip(named, n.kids[1].kids): dest[member] = by_value(ev(k))
            elif o in (0xF, 0x14, 0x5F):
                # EX_Let steps the value into the destination itself, so a None context's STALE keeps it; EX_LetBool /
                # EX_LetObj step it into a local that starts false / NULL, then store that (ScriptCore.cpp 2688-2800).
                put, v = locate(n.kids[0]), ev(n.kids[1], keep=o == 0xF)
                if isinstance(v, Written) and isinstance(ev(n.kids[0]), dict): v = Written({**ev(n.kids[0]), **v})
                if v is not STALE: put(False if v is None and o == 0x14 and s.null_rvalues else by_value(v))
            elif o == 0x64:                                 # LetValueOnPersistentFrame: into the ubergraph's frame
                uber = n.owner.split(':', 1)[-1]           # exp[2]:ExecuteUbergraph_A::Gun: a namespaced class's '::'
                uber = uber.split("'")[1] if "'" in uber else uber
                assert uber.startswith('ExecuteUbergraph_'), n.owner
                s.frame(me, uber)[n.val] = by_value(ev(n.kids[0]))
            elif o == 0x5C:                                 # AddMulticastDelegate(Obj.Prop, delegate)
                t = n.kids[0]
                obj, prop = (ev(t.kids[0]), t.kids[1].val) if t.op == 0x19 else (me, t.val)
                if obj is None:                             # Accessed None, and no address: nothing added (ScriptCore.cpp 3098)
                    s.accessed_none.append((fn, t.mem)); continue
                _, dfn, dobj = ev(n.kids[1])
                if (obj, prop, dfn, dobj) not in s.binds: s.binds.append((obj, prop, dfn, dobj))   # AddUnique
            elif o in (0x62, 0x5D):                         # Remove(Obj.Prop, delegate): one match; Clear(Obj.Prop): all
                t = n.kids[0]
                obj, prop = (ev(t.kids[0]), t.kids[1].val) if t.op == 0x19 else (me, t.val)
                drop = [b for b in s.binds if b[0] is obj and b[1] == prop]
                if o == 0x62:
                    _, dfn, dobj = ev(n.kids[1])
                    drop = [b for b in drop if b[2:] == (dfn, dobj)][:1]
                for b in drop: s.binds.remove(b)
            elif o == 0x44: locate(n.kids[0])(ev(n.kids[1]))                    # LetDelegate
            elif o == 0x61: locate(n.kids[0])(('delegate', n.val, ev(n.kids[1])))   # BindDelegate(name, var, obj)
            elif o == 0x63:                                # CallMulticastDelegate: the dispatcher, then its arguments
                t = n.kids[0]
                obj, prop = (ev(t.kids[0]), t.kids[1].val) if t.op == 0x19 else (me, t.val)
                vals = [ev(a) for a in n.kids[1:]]
                for bobj, bprop, dfn, dobj in list(s.binds):
                    if bobj is obj and bprop == prop: s.call(dfn, *vals, on=dobj)
            elif o == 6: pc = at[n.val]
            elif o == 7:
                if not ev(n.kids[0]): pc = at[n.val]
            elif o == 0x4E: pc = at[ev(n.kids[0])]
            elif o == 4: return None if n.kids[0].op == 0xB else ev(n.kids[0], keep=True)
            elif o == 0x31: store(n.kids[0], [ev(k) for k in n.kids[1:]])   # SetArray: the variable, emptied, then each element
            elif o != 0xB: ev(n)
        raise SystemExit('vm: runaway loop in ' + fn)

    def fire(s, i=0, result=None):
        """Finishes pending latent action i as FLatentActionManager does: the completion delegate (if the call had
        one) gets `result`, then CallbackTarget->ProcessEvent(FindFunction(ExecutionFunction), &Linkage)."""
        fn, ctx, (link, uuid, execfn, target), delegate = s.latent.pop(i)
        assert execfn in s.exports, execfn
        if delegate: s.call(delegate[1], result, on=delegate[2])
        s.call(execfn, on=target, EntryPoint=link)

    def broadcast(s, obj, prop, *args):
        for o, p, fn, bound in list(s.binds):
            if o is obj and p == prop: s.call(fn, *args, on=bound)


ALWAYS_NEW = {'LoadAsset', 'LoadAssetClass'}    # "We always spawn a new load" (KismetSystemLibrary.cpp 2662, 2691)


def latent_call(vm, ctx, *args):
    """A latent library call (Delay, LoadAsset, ...) as a native: recorded with its FLatentActionInfo and completion
    delegate; like FindExistingAction, a second action with the same CallbackTarget and UUID is dropped (Delay ignores
    it, RetriggerableDelay only resets the one it has: either way one action resumes), except the ALWAYS_NEW calls."""
    info = next(a for a in args if isinstance(a, Struct) and a.name == 'LatentActionInfo')
    delegate = next((a for a in args if isinstance(a, tuple) and a[0] == 'delegate'), None)
    if vm.log[-1][0] in ALWAYS_NEW or not any(p[2][1] == info[1] and p[2][3] is info[3] for p in vm.latent):
        vm.latent.append((vm.log[-1][0], args[0], info, delegate))
