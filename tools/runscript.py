#!/usr/bin/env python3
"""usage: runscript.py <base-path-without-ext> <Function> [Parm=value ...]

Runs one generated UFunction's bytecode offline, for the int / bool / float subset of Kismet:
locals, jumps, the execution-flow stack, SwitchValue, KismetMathLibrary calls. Prints the
ReturnValue. Anything outside the subset stops with `unsupported`, so a test that passes ran
every instruction it reached. Parms not given start at 0, the way a frame zeroes them.

As a module: run(base, function, **parms) -> (return value, locals)."""
import copy, struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dumpexp
from walkscript import W

FLOW_OPS = {6, 7, 0x4C, 0x4D, 0x4E, 0x4F}


class Node:
    def __init__(s, op, mem):
        s.op, s.mem, s.kids, s.val = op, mem, [], None


class P(W):
    """walkscript's reader, building a tree instead of a log."""
    def node(s):
        mem = s.mem
        op = s.u8()
        n = Node(op, mem)
        k = n.kids
        if op in (0, 1, 0x48): n.val = s.fieldpath().split('@')[0]
        elif op in (4, 0x4E, 0x4F): k.append(s.node())
        elif op == 6: n.val = s.i32()
        elif op == 7: n.val = s.i32(); k.append(s.node())
        elif op in (0xB, 0x16, 0x17, 0x25, 0x26, 0x27, 0x28, 0x2A, 0x2D, 0x4D, 0x53): pass
        elif op == 0x1F: n.val = s.cstr()
        elif op == 0x34:                                             # UnicodeStringConst: UTF-16 up to a 0 unit
            st = s.o
            while s.b[s.o] or s.b[s.o + 1]: s.o += 2
            n.val = s.b[st:s.o].decode('utf-16-le'); s.raw(2); s.mem += s.o - 2 - st
        elif op == 0x29:                                             # TextConst: its source string stands for the text
            n.val = s.u8()
            if n.val == 1: k.extend(s.node() for _ in range(3))
            elif n.val in (2, 3): k.append(s.node())
            elif n.val != 0: raise SystemExit('unsupported text literal type %d at mem %d' % (n.val, mem))
        elif op in (0x19, 0x1A): k.append(s.node()); s.i32(); s.fieldpath(); k.append(s.node())
        elif op == 0xF: s.fieldpath(); k.append(s.node()); k.append(s.node())
        elif op in (0x14, 0x5F, 0x6B): k.append(s.node()); k.append(s.node())
        elif op in (0x1B, 0x45): n.val = s.name(); s.args(k)
        elif op in (0x1C, 0x46, 0x68): n.val = s.ptr().split("'")[-2]; s.args(k)
        elif op == 0x1D: n.val = s.i32()
        elif op == 0x1E: n.val = struct.unpack_from('<f', s.b, s.o)[0]; s.raw(4)
        elif op in (0x24, 0x2C): n.val = s.u8()
        elif op == 0x35: n.val = struct.unpack_from('<q', s.b, s.o)[0]; s.raw(8)
        elif op == 0x4C: n.val = s.i32()
        elif op == 0x21: n.val = s.name()
        elif op == 0x42: n.val = s.fieldpath().split('@')[0]; k.append(s.node())
        elif op == 0x69:
            cnt = s.u16(); s.i32(); k.append(s.node())
            n.val = []
            for _ in range(cnt):
                key = s.node(); s.i32(); n.val.append((key, s.node()))
            k.append(s.node())
        else: raise SystemExit('unsupported op %02x at mem %d' % (op, mem))
        return n

    def args(s, out):
        while True:
            a = s.node()
            if a.op == 0x16: return
            out.append(a)


def script_of(base, function):
    ua, ue, total, names, imports, exports = dumpexp.load(base)
    for e in exports:
        if e['name'] != function: continue
        blob = ue[e['off'] - total: e['off'] - total + e['size']]
        for end, cand in ((len(blob) - t, c) for t in (12, 14) for c in range(4, len(blob) - t - 4)):   # 14: FUNC_Net
            if struct.unpack_from('<i', blob, cand)[0] != end - (cand + 4) or struct.unpack_from('<i', blob, cand - 4)[0] <= 0:
                continue
            try:
                t = P(blob, cand + 4, names, imports, exports)
                stmts = []
                while t.o < end: stmts.append(t.node())
                if t.o == end: return stmts
            except (Exception, SystemExit): pass
        raise SystemExit('%s: no script found' % function)
    raise SystemExit('%s: no such export' % function)


def params_of(base, function, flag=0x80):
    """The function's parameters in order, the return value left out, read off dumpstruct.py's property lines.
    flag=0x100 (CPF_OutParm): only its reference parameters."""
    import os, re, subprocess
    exports = dumpexp.load(base)[5]
    idx = next(i for i, e in enumerate(exports) if e['name'] == function)
    out = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dumpstruct.py'), base, str(idx)],
                         capture_output=True, encoding='utf-8').stdout
    found = re.findall(r'^\s+\w+Property (\w+) .*? flags=(0x[0-9a-fA-F]+)', out, re.M)
    return [name for name, flags in found if int(flags, 16) & flag and not int(flags, 16) & 0x400]


def props_of(base, function, _cache={}):
    """The function's own properties (parms and locals), name -> FProperty class, off dumpstruct.py's top-level lines."""
    import os, re, subprocess
    if (base, function) not in _cache:
        exports = dumpexp.load(base)[5]
        idx = next(i for i, e in enumerate(exports) if e['name'] == function)
        out = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dumpstruct.py'), base, str(idx)],
                             capture_output=True, encoding='utf-8').stdout
        _cache[base, function] = dict((n, t) for t, n in re.findall(r'^  (\w+Property) (\w+) ', out, re.M))
    return _cache[base, function]


# A native writes its whole return type through RESULT_PARAM, so these put 4 bytes wherever they are evaluated into.
INT32_RESULT = {'Add_IntInt', 'Subtract_IntInt', 'Multiply_IntInt', 'Divide_IntInt', 'Percent_IntInt', 'Not_Int', 'Or_IntInt',
                'And_IntInt', 'Xor_IntInt', 'Conv_BoolToInt', 'Conv_ByteToInt', 'Conv_Int64ToInt'}
# ...and these 8 bytes; an int64 parameter starts at 0 and takes only the 4 bytes an int32 operand copies in.
INT64_RESULT = {'Conv_IntToInt64', 'FTrunc64', 'Not_Int64'} | {op + '_Int64Int64' for op in
                ('Add', 'Subtract', 'Multiply', 'Divide', 'Percent', 'And', 'Or', 'Xor')}
# The operands that leave Stack.MostRecentPropertyAddress, which StructMemberContext and ArrayGetByRef offset into:
# a call evaluated into nothing leaves none (and a native writes its result through a null RESULT_PARAM).
ADDRESSABLE = {0, 1, 0x48, 0x42, 0x6B}


def i32(v): return (v + 2**31) % 2**32 - 2**31
def idiv(a, b): q = abs(a) // abs(b); return q if (a < 0) == (b < 0) else -q

MATH = {
    'Add_IntInt': lambda a, b: i32(a + b), 'Subtract_IntInt': lambda a, b: i32(a - b),
    'Multiply_IntInt': lambda a, b: i32(a * b), 'Divide_IntInt': idiv,
    'Percent_IntInt': lambda a, b: a - idiv(a, b) * b,
    'Less_IntInt': lambda a, b: a < b, 'Greater_IntInt': lambda a, b: a > b,
    'LessEqual_IntInt': lambda a, b: a <= b, 'GreaterEqual_IntInt': lambda a, b: a >= b,
    'EqualEqual_ByteByte': lambda a, b: a == b, 'EqualEqual_IntInt': lambda a, b: a == b, 'NotEqual_IntInt': lambda a, b: a != b,
    'Conv_IntToBool': lambda a: a != 0,
    'Not_PreBool': lambda a: not a, 'BooleanAND': lambda a, b: a and b, 'BooleanOR': lambda a, b: a or b,
    'Add_FloatFloat': lambda a, b: a + b, 'Multiply_FloatFloat': lambda a, b: a * b,
    'Conv_IntToFloat': float, 'Not_Int': lambda a: ~a, 'Not_Int64': lambda a: ~a,
    'Subtract_FloatFloat': lambda a, b: a - b, 'Divide_FloatFloat': lambda a, b: a / b,
    'Add_Int64Int64': lambda a, b: a + b, 'Subtract_Int64Int64': lambda a, b: a - b,
    'Less_FloatFloat': lambda a, b: a < b, 'Greater_FloatFloat': lambda a, b: a > b, 'NotEqual_FloatFloat': lambda a, b: a != b,
    'Conv_ByteToInt': lambda a: int(a) & 0xFF, 'NotEqual_ByteByte': lambda a, b: a != b,
    'Conv_IntToByte': lambda a: int(a) & 0xFF, 'Conv_Int64ToByte': lambda a: int(a) & 0xFF, 'Conv_BoolToByte': int,
    'NotEqual_Int64Int64': lambda a, b: a != b, 'EqualEqual_Int64Int64': lambda a, b: a == b, 'NotEqual_NameName': lambda a, b: str(a).lower() != str(b).lower(),
    'EqualEqual_NameName': lambda a, b: str(a).lower() == str(b).lower(),
    'Or_IntInt': lambda a, b: int(a) | int(b), 'And_IntInt': lambda a, b: int(a) & int(b), 'Xor_IntInt': lambda a, b: int(a) ^ int(b),
    'Divide_Int64Int64': idiv, 'And_Int64Int64': lambda a, b: int(a) & int(b),
    'FTrunc': lambda a: i32(int(a)), 'FTrunc64': int,     # FMath::TruncToInt: toward zero, as C++ converts
    'Conv_BoolToInt': int, 'Multiply_Int64Int64': lambda a, b: a * b, 'Conv_IntToInt64': int, 'Conv_Int64ToInt': lambda a: i32(a),
    'InRange_IntInt': lambda v, lo, hi, imin, imax: (v >= lo if imin else v > lo) and (v <= hi if imax else v < hi),
    'Abs_Int': lambda a: i32(abs(a)), 'RandomInteger': lambda a: 0,     # RandomInteger: a stand-in; CALLS shows it ran
}
CALLS = []     # every library call run, as (name, args): a test can see an impure or kept call happen
# The process memory a raw pointer reads and writes, address -> byte (unset bytes read 0). A deref is an
# ArrayGetByRef of a view struct's TArray field over the FDeref scratch; the field decides the element's size
# and signedness, as the engine's TArray<uint8> / <int32> / <int64> does.
MEM = {}
VIEWS = {'Kilobyte': (1, False), 'Mapping': (4, True), 'NameHashes': (8, True)}


def mem_read(addr, size, signed):
    return int.from_bytes(bytes(MEM.get(addr + i, 0) for i in range(size)), 'little', signed=signed)


def mem_write(addr, size, v):
    for i, b in enumerate((int(v) & ((1 << 8 * size) - 1)).to_bytes(size, 'little')): MEM[addr + i] = b


# A native int32 / uint8 parameter receives the low bytes of whatever the VM copies into it, so a wider value
# handed to an _IntInt / _ByteByte function is truncated, as it is in the engine.
for _k, _f in list(MATH.items()):
    if _k.endswith('_IntInt'): MATH[_k] = (lambda f: lambda *a: f(*[i32(x) for x in a]))(_f)
    elif _k.endswith('_ByteByte'): MATH[_k] = (lambda f: lambda *a: f(*[x & 0xFF for x in a]))(_f)


def sanitize_float(v):
    t = ('%f' % (v or 0.0)).rstrip('0')
    return t + '0' if t.endswith('.') else t


def atoi(t):
    import re
    m = re.match(r'\s*[+-]?\d+', t)
    return i32(int(m.group())) if m else 0


def int64_to_text(v, sign, grouping, lo, hi):
    t = ('{:,}' if grouping else '{}').format(abs(v)).rjust(lo, '0')
    return ('-' if v < 0 else '+' if sign else '') + t


# Strings, names and texts are Python str (FName and FString == compare case-insensitively). Objects: SELF is the
# running object, None is null. A few engine calls the test mods make are stubbed: MESSAGES collects PostGameMessage.
SELF, GAME_STATE, MESSAGES = 'Self', 'GameState', []
MATH.update({
    'Concat_StrStr': lambda a, b: a + b, 'EqualEqual_StriStri': lambda a, b: a.lower() == b.lower(),
    'Conv_IntToString': str, 'Conv_FloatToString': sanitize_float, 'Conv_BoolToString': lambda b: 'true' if b else 'false',
    'Conv_StringToInt': atoi, 'Conv_Int64ToText': int64_to_text, 'Conv_ObjectToString': lambda o: str(o) if o else 'None',
    'Conv_StringToName': str, 'Conv_NameToString': str, 'Conv_NameToText': str, 'Conv_StringToText': str, 'Conv_TextToString': str,
    'EqualEqual_ObjectObject': lambda a, b: a == b, 'NotEqual_ObjectObject': lambda a, b: a != b, 'IsValid': bool,
    'GetFSDGameState': lambda world: GAME_STATE, 'PostGameMessage': MESSAGES.append, 'GetTickableWhenPaused': lambda: False,
})


# Container library calls see their argument NODES, so an out-parm can be written. A TSet is a list of
# unique values and a TMap a dict, both in insertion order. An unset container variable reads as 0 and is
# created on first use; values are copied in, as the VM copies them.
def _made(ev, store, node, empty):
    v = ev(node)
    if v == 0:
        store(node, empty)
        v = ev(node)                # the stored object, not the copy store() was handed
    return v


def _append(ev, store, a):
    target, source = _made(ev, store, a[0], []), _made(ev, store, a[1], [])
    if source is target:            # GenericArray_Append rereads the source's length as it grows it: past the end
        raise RuntimeError('Array_Append of an array onto itself')
    target.extend(copy.deepcopy(source))


def _union(ev, store, a):
    sets = [_made(ev, store, x, []) for x in a[:3]]
    sets[2].clear()                 # GenericSet_Union empties Result first, so an input that is Result reads empty
    for v in sets[0] + sets[1]:
        if v not in sets[2]: sets[2].append(copy.deepcopy(v))


CONTAINERS = {
    'Array_Length': lambda ev, store, a: len(_made(ev, store, a[0], [])),
    'Set_Length': lambda ev, store, a: len(_made(ev, store, a[0], [])),
    'Array_Add': lambda ev, store, a: (_made(ev, store, a[0], []).append(copy.deepcopy(ev(a[1]))), len(ev(a[0])) - 1)[1],
    'Array_Get': lambda ev, store, a: store(a[2], copy.deepcopy(ev(a[0])[ev(a[1])])),
    'Array_Append': _append,
    'Set_Union': _union,
    'Set_ToArray': lambda ev, store, a: store(a[1], list(ev(a[0]))),
    'Map_Keys': lambda ev, store, a: store(a[1], list(ev(a[0]).keys())),
    'Map_Find': lambda ev, store, a: (store(a[2], _made(ev, store, a[0], {}).get(ev(a[1]), 0)), ev(a[1]) in ev(a[0]))[1],
    'Map_Add': lambda ev, store, a: _made(ev, store, a[0], {}).__setitem__(ev(a[1]), copy.deepcopy(ev(a[2]))),
    'Array_Clear': lambda ev, store, a: store(a[0], []),
    'Set_Clear': lambda ev, store, a: store(a[0], []),
    'Map_Clear': lambda ev, store, a: store(a[0], {}),
}

# No map value is one of these: a container inside a map is the Value of a wrapper struct.
BARE_CONTAINERS = {'ArrayProperty', 'SetProperty', 'MapProperty'}


def run(base, function, self_vars=None, **parms):
    stmts = script_of(base, function)
    at = {n.mem: i for i, n in enumerate(stmts)}
    env = dict(parms)
    flow = []
    self_vars = self_vars if self_vars is not None else {}
    types = props_of(base, function)

    def addressable(n):
        if n.op in (0x42, 0x6B) and n.kids[0].op not in ADDRESSABLE:
            raise SystemExit('op %02x at mem %d reads through op %02x, which leaves no address' % (n.op, n.mem, n.kids[0].op))

    def is32(v):
        return v.op == 0x1D or v.op in (0x1C, 0x46, 0x68) and v.val in INT32_RESULT or v.op in (0, 0x48) and types.get(v.val) == 'IntProperty'

    def fits(dest, v):
        if is32(v) and types.get(dest) == 'ByteProperty':
            raise SystemExit('an int32 evaluated into the 1-byte %s at mem %d' % (dest, v.mem))
        if v.op in (0x1C, 0x46, 0x68) and v.val in INT64_RESULT and types.get(dest) in ('IntProperty', 'ByteProperty'):
            raise SystemExit('an int64 evaluated into the %s %s at mem %d' % (types[dest], dest, v.mem))

    def ev(n):
        o = n.op
        addressable(n)
        if o in (0, 0x48): return env.get(n.val, 0)
        if o == 1: return self_vars.get(n.val, 0)
        if o in (0x1D, 0x1E, 0x24, 0x2C, 0x35, 0x21): return n.val
        if o == 0x42 and n.val == 'Value': return ev(n.kids[0])      # a nested container's wrapper is its Value
        if o == 0x42:                                                # a struct is a dict; an unset member reads 0
            s = ev(n.kids[0])
            return s.get(n.val, 0) if isinstance(s, dict) else 0
        if o in (0x1F, 0x34): return n.val
        if o == 0x29: return ev(n.kids[0]) if n.kids else ''
        if o == 0x17: return SELF
        if o in (0x2A, 0x2D): return None
        if o in (0x19, 0x1A):                                        # a native call on another object; null skips it
            if n.kids[1].op != 0x1C: raise SystemExit('unsupported context expression op %02x' % n.kids[1].op)
            return ev(n.kids[1]) if ev(n.kids[0]) else 0
        if o == 0x25: return 0
        if o == 0x26: return 1
        if o == 0x27: return True
        if o == 0x28: return False
        if o == 0x6B and n.kids[0].op == 0x42 and n.kids[0].val in VIEWS:
            size, signed = VIEWS[n.kids[0].val]
            return mem_read(ev(n.kids[0].kids[0])['Data'] + ev(n.kids[1]) * size, size, signed)
        if o == 0x6B: return ev(n.kids[0])[ev(n.kids[1])]
        if o in (0x1B, 0x45):                                        # the class's own function, by name: a frame of its own
            names = params_of(base, n.val)
            outs = params_of(base, n.val, 0x100)
            # The VM steps a reference argument with no result buffer (ProcessScriptFunction), so a constant or a
            # call there writes through null in game: it must be a variable.
            for name, a in zip(names, n.kids):
                if name in outs and a.op not in ADDRESSABLE and not (a.op in (0x19, 0x1A) and a.kids[1].op in ADDRESSABLE):
                    raise SystemExit('%s: reference parameter %s gets a non-variable (op %02x), which crashes the VM' % (n.val, name, a.op))
            r, callee = run(base, n.val, self_vars, **dict(zip(names, [copy.deepcopy(ev(a)) for a in n.kids])))
            for name, a in zip(names, n.kids):                       # a reference parameter is its argument's variable
                if name in outs and a.op in ADDRESSABLE: store(a, callee.get(name, 0))
            return r
        if o in (0x1C, 0x46, 0x68) and n.val == 'Map_Find' and n.kids[2].op in (0, 0x48) and types.get(n.kids[2].val) in BARE_CONTAINERS:
            # execMap_Find writes in place only into the map's value property class, and a container value is its
            # wrapper StructProperty: a bare container local is left as it was.
            return CONTAINERS[n.val](ev, lambda d, v: d is n.kids[2] or store(d, v), n.kids)
        if o in (0x1C, 0x46, 0x68) and n.val in CONTAINERS: return CONTAINERS[n.val](ev, store, n.kids)
        if o in (0x1C, 0x46, 0x68):
            if n.val not in MATH: raise SystemExit('unsupported call ' + n.val)
            args = [ev(a) for a in n.kids]
            if n.val.endswith('_Int64Int64'):
                args = [v & 0xFFFFFFFF if is32(a) else v for a, v in zip(n.kids, args)]
            CALLS.append((n.val, tuple(args)))
            return MATH[n.val](*args)
        if o == 0x69:
            v = ev(n.kids[0])
            for key, res in n.val:
                if ev(key) == v: return ev(res)
            return ev(n.kids[1])
        raise SystemExit('unsupported expression op %02x at mem %d' % (o, n.mem))

    def store(dest, v):
        v = copy.deepcopy(v)
        addressable(dest)
        if dest.op == 0x42 and dest.val == 'Value': store(dest.kids[0], v)
        elif dest.op == 0x42: _made(ev, store, dest.kids[0], {})[dest.val] = v
        elif dest.op in (0, 0x48): env[dest.val] = v
        elif dest.op == 1: self_vars[dest.val] = v
        elif dest.op == 0x6B and dest.kids[0].op == 0x42 and dest.kids[0].val in VIEWS: locate(dest)(v)
        elif dest.op == 0x6B: ev(dest.kids[0])[ev(dest.kids[1])] = v
        else: raise SystemExit('unsupported destination op %02x' % dest.op)

    def locate(dest):
        # UObject::execLet steps the destination (its struct, array and index) before the value: evaluate those now,
        # return what stores into the place they found.
        addressable(dest)
        if dest.op == 0x42 and dest.val == 'Value': return locate(dest.kids[0])
        if dest.op == 0x42:
            s = _made(ev, store, dest.kids[0], {})
            return lambda v: s.__setitem__(dest.val, copy.deepcopy(v))
        if dest.op == 0x6B and dest.kids[0].op == 0x42 and dest.kids[0].val in VIEWS:
            size = VIEWS[dest.kids[0].val][0]
            at = ev(dest.kids[0].kids[0])['Data'] + ev(dest.kids[1]) * size
            return lambda v: mem_write(at, size, v)
        if dest.op == 0x6B:
            arr, i = ev(dest.kids[0]), ev(dest.kids[1])
            return lambda v: arr.__setitem__(i, copy.deepcopy(v))
        return lambda v: store(dest, v)

    pc, steps = 0, 0
    while True:
        steps += 1
        if steps > 1000000: raise SystemExit('runaway loop')
        n = stmts[pc]
        o = n.op
        nxt = pc + 1
        if o in (0xF, 0x14, 0x5F):
            if n.kids[0].op in (0, 0x48): fits(n.kids[0].val, n.kids[1])
            locate(n.kids[0])(ev(n.kids[1]))
        elif o == 6: nxt = at[n.val]
        elif o == 7:
            if not ev(n.kids[0]): nxt = at[n.val]
        elif o == 0x4C: flow.append(n.val)
        elif o == 0x4D:
            if not flow: raise SystemExit('pop from an empty flow stack')
            nxt = at[flow.pop()]
        elif o == 0x4F:
            if not ev(n.kids[0]):
                if not flow: raise SystemExit('pop from an empty flow stack')
                nxt = at[flow.pop()]
        elif o == 0x4E: nxt = at[ev(n.kids[0])]
        elif o == 4:
            r = n.kids[0]
            if r.op != 0xB: fits('ReturnValue', r)
            return (ev(r) if r.op != 0xB else None), env
        elif o == 0xB: pass
        elif o in (0x1B, 0x45, 0x1C, 0x46, 0x68, 0x19, 0x1A): ev(n)
        elif o == 0x53: raise SystemExit('ran off the end of the script')
        else: raise SystemExit('unsupported statement op %02x at mem %d' % (o, n.mem))
        pc = nxt


def main():
    base, fn = sys.argv[1], sys.argv[2]
    parms = {}
    for a in sys.argv[3:]:
        k, v = a.split('=', 1)
        parms[k] = float(v) if '.' in v else int(v)
    r, env = run(base, fn, **parms)
    print(r)


if __name__ == "__main__":
    main()
