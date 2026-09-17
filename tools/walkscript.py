#!/usr/bin/env python3
"""usage: walkscript.py <base-path-without-ext> <export-index>  — walks a UFunction export's bytecode with the 4.27 grammar,
printing each top-level expression with its disk and memory size, and comparing the totals to the header."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dumpexp

NAMES = {0:'LocalVariable',1:'InstanceVariable',2:'DefaultVariable',4:'Return',6:'Jump',7:'JumpIfNot',8:'Assert',
 0xB:'Nothing',0xF:'Let',0x12:'ClassContext',0x13:'MetaCast',0x14:'LetBool',0x15:'EndParmValue',0x16:'EndFunctionParms',
 0x17:'Self',0x18:'Skip',0x19:'Context',0x1A:'Context_FailSilent',0x1B:'VirtualFunction',0x1C:'FinalFunction',0x1D:'IntConst',
 0x1E:'FloatConst',0x1F:'StringConst',0x20:'ObjectConst',0x21:'NameConst',0x22:'RotationConst',0x23:'VectorConst',0x24:'ByteConst',
 0x25:'IntZero',0x26:'IntOne',0x27:'True',0x28:'False',0x29:'TextConst',0x2A:'NoObject',0x2B:'TransformConst',0x2C:'IntConstByte',
 0x2D:'NoInterface',0x2E:'DynamicCast',0x2F:'StructConst',0x30:'EndStructConst',0x31:'SetArray',0x32:'EndArray',0x33:'PropertyConst',
 0x34:'UnicodeStringConst',0x35:'Int64Const',0x36:'UInt64Const',0x38:'PrimitiveCast',0x39:'SetSet',0x3A:'EndSet',0x3B:'SetMap',
 0x3C:'EndMap',0x3D:'SetConst',0x3E:'EndSetConst',0x3F:'MapConst',0x40:'EndMapConst',0x42:'StructMemberContext',
 0x43:'LetMulticastDelegate',0x44:'LetDelegate',0x45:'LocalVirtualFunction',0x46:'LocalFinalFunction',0x48:'LocalOutVariable',
 0x4B:'InstanceDelegate',0x4C:'PushExecutionFlow',0x4D:'PopExecutionFlow',0x4E:'ComputedJump',0x4F:'PopExecutionFlowIfNot',
 0x50:'Breakpoint',0x51:'InterfaceContext',0x52:'ObjToInterfaceCast',0x53:'EndOfScript',0x54:'CrossInterfaceCast',
 0x55:'InterfaceToObjCast',0x5A:'WireTracepoint',0x5B:'SkipOffsetConst',0x5C:'AddMulticastDelegate',0x5D:'ClearMulticastDelegate',
 0x5E:'Tracepoint',0x5F:'LetObj',0x60:'LetWeakObjPtr',0x61:'BindDelegate',0x62:'RemoveMulticastDelegate',0x63:'CallMulticastDelegate',
 0x64:'LetValueOnPersistentFrame',0x65:'ArrayConst',0x66:'EndArrayConst',0x67:'SoftObjectConst',0x68:'CallMath',0x69:'SwitchValue',
 0x6A:'InstrumentationEvent',0x6B:'ArrayGetByRef',0x6C:'ClassSparseDataVariable',0x6D:'FieldPathConst'}

class W:
    def __init__(s, b, o, names, imports, exports):
        s.b, s.o, s.mem, s.names, s.imports, s.exports = b, o, 0, names, imports, exports
        s.depth = 0; s.log = []
    def u8(s):  v = s.b[s.o]; s.o += 1; s.mem += 1; return v
    def u16(s): v = struct.unpack_from('<H', s.b, s.o)[0]; s.o += 2; s.mem += 2; return v
    def i32(s): v = struct.unpack_from('<i', s.b, s.o)[0]; s.o += 4; s.mem += 4; return v
    def raw(s, n): s.o += n; s.mem += n
    def ptr(s):
        v = struct.unpack_from('<i', s.b, s.o)[0]; s.o += 4; s.mem += 8
        return dumpexp.pidx(v, s.imports, s.exports)
    def name(s):
        i = struct.unpack_from('<i', s.b, s.o)[0]; n = struct.unpack_from('<i', s.b, s.o + 4)[0]
        s.o += 8; s.mem += 12
        return s.names[i] + (('_%d' % (n - 1)) if n else '')
    def fieldpath(s):
        cnt = struct.unpack_from('<i', s.b, s.o)[0]; s.o += 4
        segs = []
        for _ in range(cnt):
            i = struct.unpack_from('<i', s.b, s.o)[0]; segs.append(s.names[i]); s.o += 8
        owner = struct.unpack_from('<i', s.b, s.o)[0]; s.o += 4
        s.mem += 8
        return '.'.join(segs) + '@' + dumpexp.pidx(owner, s.imports, s.exports)
    def cstr(s):
        e = s.b.index(b'\0', s.o); n = e - s.o + 1; t = s.b[s.o:e].decode('latin-1'); s.raw(n); return t
    def wstr(s):
        st = s.o
        while s.b[s.o] or s.b[s.o+1]: s.o += 2
        s.o += 2; s.mem += s.o - st; return '<wide>'
    def parms(s):
        while True:
            if s.expr() == 0x16: break
    def expr(s):
        start_o, start_m = s.o, s.mem
        op = s.u8()
        nm = NAMES.get(op, '?%02x' % op)
        info = ''
        s.depth += 1
        if op in (0, 1, 2, 0x48, 0x6C, 0x33): info = s.fieldpath()
        elif op in (4, 0x51, 0x4E, 0x4F, 0x67, 0x6D): s.expr()
        elif op == 6: info = 'to %d' % s.i32()
        elif op == 7: info = 'to %d' % s.i32(); s.expr()
        elif op == 8: s.u16(); s.u8(); s.expr()
        elif op in (0xB, 0x15, 0x16, 0x17, 0x25, 0x26, 0x27, 0x28, 0x2A, 0x2D, 0x30, 0x32, 0x3A, 0x3C, 0x3E, 0x40, 0x4D, 0x50, 0x53, 0x5A, 0x5E, 0x66): pass
        elif op == 0xF: info = s.fieldpath(); s.expr(); s.expr()
        elif op in (0x14, 0x43, 0x44, 0x5F, 0x60): s.expr(); s.expr()
        elif op in (0x13, 0x2E, 0x52, 0x54, 0x55): info = s.ptr(); s.expr()
        elif op == 0x18: info = 'skip %d' % s.i32(); s.expr()
        elif op in (0x19, 0x1A, 0x12): s.expr(); info = 'skip %d ' % s.i32(); info += s.fieldpath(); s.expr()
        elif op in (0x1B, 0x45): info = s.name(); s.parms()
        elif op in (0x1C, 0x46, 0x68): info = s.ptr(); s.parms()
        elif op == 0x63: info = s.ptr(); s.expr(); s.parms()
        elif op == 0x1D: info = str(s.i32())
        elif op == 0x1E: s.raw(4)
        elif op == 0x1F: info = repr(s.cstr())
        elif op == 0x34: info = s.wstr()
        elif op == 0x20: info = s.ptr()
        elif op == 0x21: info = s.name()
        elif op in (0x22, 0x23): s.raw(12)
        elif op == 0x2B: s.raw(40)
        elif op in (0x24, 0x2C): info = str(s.u8())
        elif op == 0x29:
            t = s.u8(); info = 'type %d' % t
            if t == 1: s.expr(); s.expr(); s.expr()
            elif t in (2, 3): s.expr()
            elif t == 4: s.ptr(); s.expr(); s.expr()
        elif op == 0x2F: info = s.ptr() + ' size %d' % s.i32();
        elif op == 0x2F: pass
        elif op == 0x31: s.expr();
        elif op in (0x35, 0x36): s.raw(8)
        elif op == 0x38: info = 'cast %d' % s.u8(); s.expr()
        elif op in (0x39, 0x3B): s.expr(); s.i32()
        elif op == 0x3D: info = s.fieldpath(); s.i32()
        elif op == 0x3F: info = s.fieldpath() + ',' + s.fieldpath(); s.i32()
        elif op == 0x65: info = s.fieldpath(); s.i32()
        elif op == 0x42: info = s.fieldpath(); s.expr()
        elif op == 0x4B: info = s.name()
        elif op == 0x4C: info = 'skip %d' % s.i32()
        elif op == 0x5B: s.i32()
        elif op in (0x5C, 0x62): s.expr(); s.expr()
        elif op == 0x5D: s.expr()
        elif op == 0x61: info = s.name(); s.expr(); s.expr()
        elif op == 0x64: info = s.fieldpath(); s.expr()
        elif op == 0x69:
            s.expr(); n = s.u16(); s.i32()
            for _ in range(n): s.expr(); s.i32(); s.expr()
            s.expr()
        elif op == 0x6A:
            t = s.u8()
            if t == 0x0A: s.name()   # InlineEvent
        elif op == 0x6B: s.expr(); s.expr()
        else: raise SystemExit('unknown op %02x at %d' % (op, start_o))
        # the run-until-terminator forms
        if op in (0x2F, 0x31, 0x39, 0x3B, 0x3D, 0x3F, 0x65):
            term = {0x2F:0x30, 0x31:0x32, 0x39:0x3A, 0x3B:0x3C, 0x3D:0x3E, 0x3F:0x40, 0x65:0x66}[op]
            while s.expr() != term: pass
        s.depth -= 1
        s.log.append((start_o, start_m, s.o - start_o, s.mem - start_m, '  ' * s.depth + nm, info))
        return op

def main():
    base, idx = sys.argv[1], int(sys.argv[2])
    ua, ue, total, names, imports, exports = dumpexp.load(base)
    e = exports[idx]
    blob = ue[e['off'] - total: e['off'] - total + e['size']]
    trailer = 12
    script_end = len(blob) - trailer
    p = None
    for cand in range(0, script_end - 4):
        if struct.unpack_from('<i', blob, cand)[0] == script_end - (cand + 4) and struct.unpack_from('<i', blob, cand - 4)[0] > 0:
            try:
                t = W(blob, cand + 4, names, imports, exports)
                while t.o < script_end: t.expr()
                if t.o == script_end: p = cand; break
            except (Exception, SystemExit): pass     # expr() exits on an unknown op; a wrong candidate hits one
    assert p is not None, 'no ScriptStorageSize found'
    bytecode, storage = struct.unpack_from('<i', blob, p - 4)[0], struct.unpack_from('<i', blob, p)[0]
    print('%s: header ScriptBytecodeSize=%d ScriptStorageSize=%d, script at +%d' % (e['name'], bytecode, storage, p + 4))
    w = W(blob, p + 4, names, imports, exports)
    while w.o < script_end:
        w.expr()
        if w.mem >= bytecode and w.o < script_end:
            print('  ** loader would STOP here: mem %d >= %d with %d disk bytes unread' % (w.mem, bytecode, script_end - w.o))
    w.log.sort()
    for o, m, dsz, msz, nm, info in w.log:
        print('  +%4d mem %4d  disk %3d mem %3d  %-22s %s' % (o, m, dsz, msz, nm, info))
    print('walked: disk %d (header %d)  mem %d (header %d)' % (w.o - (p + 4), storage, w.mem, bytecode))

if __name__ == "__main__":
    main()
