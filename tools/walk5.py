#!/usr/bin/env python3
"""usage: walk5.py <base-path-without-ext> [function-name-substring...]
Every script function of a UE 5.6 cooked package (or those whose name contains a substring), expression by expression
with its memory offset: walkscript.py's walker with the 5.6 operands (LWC doubles in vector, rotator and transform
consts; EX_DoubleConst 0x37, EX_Vector3fConst 0x41, EX_NothingInt32 0x0C, EX_BitFieldConst 0x11). The memory offsets
are the ones jumps and the ubergraph entry points name."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dumpexp, walkscript
from walkscript import W

NAMES = dict(walkscript.NAMES)
NAMES.update({0x37: 'DoubleConst', 0x41: 'Vector3fConst', 0x0C: 'NothingInt32', 0x11: 'BitFieldConst'})

class W5(W):
    def f32(s): v = struct.unpack_from('<f', s.b, s.o)[0]; s.raw(4); return v
    def f64(s): v = struct.unpack_from('<d', s.b, s.o)[0]; s.raw(8); return v
    def expr(s):
        start_o, start_m = s.o, s.mem
        op = s.b[s.o]
        if op not in (0x37, 0x41, 0x0C, 0x11, 0x1E, 0x22, 0x23, 0x2B):
            return W.expr(s)
        s.u8(); info = ''
        if op == 0x37: info = '%g' % s.f64()
        elif op == 0x1E: info = '%g' % s.f32()
        elif op == 0x41: info = '(%g, %g, %g)' % (s.f32(), s.f32(), s.f32())
        elif op == 0x0C: info = str(s.i32())
        elif op == 0x11: info = s.fieldpath() + ' = %d' % s.u8()
        elif op in (0x22, 0x23): info = '(%g, %g, %g)' % (s.f64(), s.f64(), s.f64())
        elif op == 0x2B: s.raw(80)                      # FTransform: a quat and two vectors, all doubles
        s.log.append((start_o, start_m, s.o - start_o, s.mem - start_m, '  ' * s.depth + NAMES[op], info))
        return op

def main():
    base, wanted = sys.argv[1], sys.argv[2:]
    ua, ue, total, names, imports, exports = dumpexp.load(base)
    for e in exports:
        if 'Function' not in dumpexp.pidx(e['cls'], imports, exports): continue
        if wanted and not any(w in e['name'] for w in wanted): continue
        blob = ue[e['off'] - total: e['off'] - total + e['size']]
        found = None
        for trailer in (12, 14):                        # 14: FUNC_Net adds RepOffset
            script_end = len(blob) - trailer
            for cand in range(4, script_end - 4):
                if struct.unpack_from('<i', blob, cand)[0] != script_end - (cand + 4): continue
                try:
                    t = W5(blob, cand + 4, names, imports, exports)
                    while t.o < script_end: t.expr()
                    if t.o == script_end: found = t; break
                except Exception: pass                  # a wrong candidate hits an unknown op or runs off the end
            if found: break
        print('// Function %s' % e['name'])
        if not found:                                   # a delegate signature has no script
            print('  (no script)\n'); continue
        found.log.sort()
        for o, m, dsz, msz, nm, info in found.log:
            print('[%04d] %-28s %s' % (m, nm, info))
        print()

if __name__ == '__main__':
    main()
