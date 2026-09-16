#!/usr/bin/env python3
"""usage: dumpstruct.py <base-path-without-ext> <export-index>
A Function / BlueprintGeneratedClass export's UStruct body: SuperStruct, Children, every
ChildProperty with its flags and type tail, the bytecode sizes, then the function or class
trailer (FunctionFlags, FuncMap, ClassFlags, Interfaces, ...)."""
import struct
import sys

import dumpexp
from dumptags import tags


class R:
    def __init__(s, b, names, imports, exports):
        s.b, s.o, s.names, s.imports, s.exports = b, 0, names, imports, exports
    def i32(s): v = struct.unpack_from('<i', s.b, s.o)[0]; s.o += 4; return v
    def u32(s): v = struct.unpack_from('<I', s.b, s.o)[0]; s.o += 4; return v
    def u64(s): v = struct.unpack_from('<Q', s.b, s.o)[0]; s.o += 8; return v
    def u16(s): v = struct.unpack_from('<H', s.b, s.o)[0]; s.o += 2; return v
    def u8(s): v = s.b[s.o]; s.o += 1; return v
    def idx(s): return dumpexp.pidx(s.i32(), s.imports, s.exports)
    def name(s):
        i, n = s.i32(), s.i32()
        return s.names[i] + ('_%d' % (n - 1) if n else '')


def field(r, ftype, pad, out):
    fname, flags = r.name(), r.u32()
    dim, size, pflags, rep, notify, cond = r.i32(), r.i32(), r.u64(), r.u16(), r.name(), r.u8()
    tail = ''
    if ftype in ('ObjectProperty', 'WeakObjectProperty', 'LazyObjectProperty', 'SoftObjectProperty',
                 'InterfaceProperty', 'StructProperty', 'ByteProperty', 'DelegateProperty',
                 'MulticastInlineDelegateProperty', 'MulticastSparseDelegateProperty'):
        tail = r.idx()
    elif ftype in ('ClassProperty', 'SoftClassProperty'):
        tail = r.idx() + ' meta=' + r.idx()
    elif ftype == 'BoolProperty':
        tail = 'bool ' + r.b[r.o:r.o + 6].hex(); r.o += 6
    out.append('%s%s %s objflags=%#x dim=%d size=%d flags=%#x rep=%d notify=%s cond=%d %s'
               % (pad, ftype, fname, flags, dim, size, pflags, rep, notify, cond, tail))
    subs = {'ArrayProperty': 1, 'SetProperty': 1, 'MapProperty': 2, 'EnumProperty': 1}.get(ftype, 0)
    if ftype == 'EnumProperty':
        out[-1] += r.idx()
    for _ in range(subs):
        field(r, r.name(), pad + '    ', out)


def main():
    base, i = sys.argv[1], int(sys.argv[2])
    ua, ue, total, names, imports, exports = dumpexp.load(base)
    e = exports[i]
    blob = ue[e['off'] - total: e['off'] - total + e['size']]
    r = R(blob, names, imports, exports)
    out = ['=== export[%d] %s  class=%s' % (i, e['name'], dumpexp.pidx(e['cls'], imports, exports))]
    r.o = tags(blob, 0, len(blob), names, 1, out)
    if r.i32():
        r.o += 16                                   # lazy-object guid
    out.append('SuperStruct %s' % r.idx())
    out.append('Children %s' % [r.idx() for _ in range(r.i32())])
    for _ in range(r.i32()):
        field(r, r.name(), '  ', out)
    mem, disk = r.i32(), r.i32()
    out.append('Script mem=%d disk=%d' % (mem, disk))
    r.o += disk
    cls = dumpexp.pidx(e['cls'], imports, exports)
    if cls.endswith("'Function'") or cls.endswith("'DelegateFunction'"):
        flags = r.u32()
        out.append('FunctionFlags %#x' % flags)
        if flags & 0x40:
            out.append('RepOffset %d' % r.u16())
        out.append('EventGraphFunction %s offset %d' % (r.idx(), r.i32()))
    elif 'Class' in cls:
        out.append('FuncMap %s' % [(r.name(), r.idx()) for _ in range(r.i32())])
        out.append('ClassFlags %#x within=%s config=%s' % (r.u32(), r.idx(), r.name()))
        out.append('ClassGeneratedBy %s' % r.idx())
        out.append('Interfaces %s' % [(r.idx(), r.i32(), r.i32()) for _ in range(r.i32())])
        out.append('bDeprecatedForceScriptOrder %d dummy=%s bCooked %d CDO %s'
                   % (r.i32(), r.name(), r.i32(), r.idx()))
    out.append('consumed %d of %d' % (r.o, len(blob)))
    print('\n'.join(out))


if __name__ == '__main__':
    main()
