#!/usr/bin/env python3
"""A class run the way the Blueprint VM runs it, for what runscript.py alone cannot: latent calls and the ubergraph's
persistent frame, delegate binds and broadcasts, calls and sets on other objects. Engine calls are logged and answered
by a test's fakes. Importing this extends runscript's parser with the opcodes those bodies use."""
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
    if op not in (0, 0x48, 0x19, 0x1C, 0x20, 0x2E, 0x2F, 0x30, 0x46, 0x4B, 0x5B, 0x5C, 0x63, 0x64, 0x68):
        return _parse(s)
    s.u8()
    n = Node(op, mem)
    if op in (0, 0x48, 0x64): n.val, n.owner = s.fieldpath().split('@')
    if op == 0x19: n.kids.append(s.node()); s.i32(); s.fieldpath(); n.kids.append(s.node())
    elif op in (0x20, 0x2E, 0x2F): n.val = _ref(s)
    elif op in (0x1C, 0x46, 0x68): n.val = _ref(s); s.args(n.kids)          # an export callee has no quotes
    elif op == 0x63: n.val = _ref(s); s.args(n.kids)                        # the signature, the dispatcher, its args
    elif op == 0x4B: n.val = s.name()
    elif op == 0x5B: n.val = s.i32()
    elif op == 0x5C: n.kids += [s.node(), s.node()]
    if op in (0x2E, 0x64): n.kids.append(s.node())
    if op == 0x2F:
        s.i32()
        while True:
            k = s.node()
            if k.op == 0x30: break
            n.kids.append(k)
    return n


P.node = _node


class Struct(list):
    def __init__(s, name, vals): super().__init__(vals); s.name = name


class Obj:
    """An object the VM hands around: class name, variables, outer (OuterPrivate) and a fake address."""
    _next = 0x10000

    def __init__(s, cls, outer=None, **vars):
        s.cls, s.vars, s.outer = cls, dict(vars), outer
        Obj._next += 0x1000
        s.addr = Obj._next

    def __repr__(s): return '<%s>' % s.cls


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

    def new(s, **vars): return Obj(s.self.cls, **vars)

    def script(s, fn):
        if fn not in s.scripts:
            stmts = runscript.script_of(s.base, fn)
            s.scripts[fn] = (stmts, {n.mem: i for i, n in enumerate(stmts)}, runscript.params_of(s.base, fn))
        return s.scripts[fn]

    def call(s, fn, *args, on=None, **parms):
        me = on or s.self
        stmts, at, names = s.script(fn)
        env = s.frames.setdefault(id(me), {}) if fn.startswith('ExecuteUbergraph_') else {}   # the persistent frame
        env.update(zip(names, args)); env.update(parms)

        def local(n):
            assert n.owner.endswith(':' + fn), '%s uses %s of %s, a property of another function' % (fn, n.val, n.owner)
            return n.val

        def ev(n, ctx=me):
            o = n.op
            if o in (0, 0x48): return env.get(local(n), 0)
            if o == 1: return ctx.vars.get(n.val, 0)
            if o == 0x17: return me
            if o == 0x2A: return None
            if o == 0x20: return s.objects.get(n.val, n.val)
            if o in (0x1D, 0x1E, 0x1F, 0x34, 0x24, 0x2C, 0x35, 0x21, 0x5B): return n.val
            if o == 0x4B: return ('delegate', n.val, me)                   # EX_InstanceDelegate binds Stack.Object
            if o in (0x25, 0x26): return o - 0x25
            if o in (0x27, 0x28): return o == 0x27
            if o == 0x2F: return Struct(n.val, [ev(k) for k in n.kids])
            if o == 0x2E:
                v = ev(n.kids[0]); return v if s.isa(v, n.val) else None
            if o == 0x19:
                obj = ev(n.kids[0])
                return None if obj is None else ev(n.kids[1], obj)
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
                return s.call(name, *[ev(a) for a in n.kids], on=ctx)
            if o in (0x1B, 0x45, 0x1C, 0x46, 0x68):
                runscript.native_refs(n)
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
            if o in (0xF, 0x14, 0x5F): locate(n.kids[0])(ev(n.kids[1]))
            elif o == 0x64:                                 # LetValueOnPersistentFrame: into the ubergraph's frame
                assert n.owner.split(':')[-1].startswith('ExecuteUbergraph_'), n.owner
                s.frames.setdefault(id(me), {})[n.val] = ev(n.kids[0])
            elif o == 0x5C:                                 # AddMulticastDelegate(Obj.Prop, delegate)
                t = n.kids[0]
                obj, prop = (ev(t.kids[0]), t.kids[1].val) if t.op == 0x19 else (me, t.val)
                _, dfn, dobj = ev(n.kids[1])
                s.binds.append((obj, prop, dfn, dobj))
            elif o == 0x63:                                 # CallMulticastDelegate: the dispatcher, then its arguments
                t = n.kids[0]
                obj, prop = (ev(t.kids[0]), t.kids[1].val) if t.op == 0x19 else (me, t.val)
                vals = [ev(a) for a in n.kids[1:]]
                for bobj, bprop, dfn, dobj in list(s.binds):
                    if bobj is obj and bprop == prop: s.call(dfn, *vals, on=dobj)
            elif o == 6: pc = at[n.val]
            elif o == 7:
                if not ev(n.kids[0]): pc = at[n.val]
            elif o == 0x4E: pc = at[ev(n.kids[0])]
            elif o == 4: return None if n.kids[0].op == 0xB else ev(n.kids[0])
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


def latent_call(vm, ctx, *args):
    """A latent library call (Delay, LoadAsset, ...) as a native: recorded with its FLatentActionInfo and completion
    delegate; like FindExistingAction, a second action with the same CallbackTarget and UUID is dropped."""
    info = next(a for a in args if isinstance(a, Struct) and a.name == 'LatentActionInfo')
    delegate = next((a for a in args if isinstance(a, tuple) and a[0] == 'delegate'), None)
    if not any(p[2][1] == info[1] and p[2][3] is info[3] for p in vm.latent):
        vm.latent.append((vm.log[-1][0], args[0], info, delegate))
