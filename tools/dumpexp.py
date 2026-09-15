#!/usr/bin/env python3
"""Dump one cooked export's payload with FName / FPackageIndex annotations.

usage: dumpexp.py <base-path-without-ext> <export-index> [count]
"""
import struct, sys

class R:
    def __init__(s, b, o=0): s.b, s.o = b, o
    def i32(s):
        v = struct.unpack_from('<i', s.b, s.o)[0]; s.o += 4; return v
    def u32(s):
        v = struct.unpack_from('<I', s.b, s.o)[0]; s.o += 4; return v
    def i64(s):
        v = struct.unpack_from('<q', s.b, s.o)[0]; s.o += 8; return v
    def u16(s):
        v = struct.unpack_from('<H', s.b, s.o)[0]; s.o += 2; return v
    def fstr(s):
        n = s.i32()
        if n == 0: return ""
        t = s.b[s.o:s.o + n - 1].decode('latin-1', 'replace'); s.o += n; return t

def load(base):
    ua = open(base + ".uasset", "rb").read()
    try: ue = open(base + ".uexp", "rb").read()
    except FileNotFoundError: ue = b""
    r = R(ua, 4)
    legacy = r.i32()
    if legacy != -4: r.i32()
    r.i32(); r.i32()
    for _ in range(r.i32()): r.o += 20
    total = r.i32(); r.fstr(); flags = r.u32()
    ncount, noff = r.i32(), r.i32()
    if not (flags & 0x80000000): r.fstr()
    r.i32(); r.i32()
    ecount, eoff = r.i32(), r.i32()
    icount, ioff = r.i32(), r.i32()

    names = []
    n = R(ua, noff)
    for _ in range(ncount):
        names.append(n.fstr()); n.o += 4
    imports = []
    im = R(ua, ioff)
    for _ in range(icount):
        im.o += 8; cls = names[im.i32()]; im.o += 4
        outer = im.i32(); obj = names[im.i32()]; im.o += 4
        imports.append(f"{cls}'{obj}'")
    exports = []
    ex = R(ua, eoff)
    for _ in range(ecount):
        ci, si, ti, oi = ex.i32(), ex.i32(), ex.i32(), ex.i32()
        nm = names[ex.i32()]; num = ex.i32()
        if num: nm = f"{nm}_{num-1}"
        fl = ex.u32(); size, off = ex.i64(), ex.i64()
        ex.o += 12 + 20 + 8 + 4 + 16
        exports.append(dict(name=nm, cls=ci, super=si, tmpl=ti, outer=oi, flags=fl, size=size, off=off))
    return ua, ue, total, names, imports, exports

def pidx(v, imports, exports):
    if v == 0: return "null"
    if v < 0:
        i = -v - 1
        return f"imp[{i}]:{imports[i]}" if i < len(imports) else f"imp[{i}]:?"
    i = v - 1
    return f"exp[{i}]:{exports[i]['name']}" if i < len(exports) else f"exp[{i}]:?"

def main():
    base, idx = sys.argv[1], int(sys.argv[2])
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    ua, ue, total, names, imports, exports = load(base)
    for k in range(idx, min(idx + count, len(exports))):
        e = exports[k]
        blob = ue[e['off'] - total: e['off'] - total + e['size']]
        print(f"=== export[{k}] {e['name']}  size={e['size']} flags={e['flags']:#x}")
        print(f"    class={pidx(e['cls'],imports,exports)} super={pidx(e['super'],imports,exports)}"
              f" outer={pidx(e['outer'],imports,exports)}")
        for i in range(0, len(blob), 4):
            c = blob[i:i+4]
            if len(c) < 4:
                print(f"  +{i:03x} {c.hex()}"); break
            v = struct.unpack('<I', c)[0]
            sv = struct.unpack('<i', c)[0]
            note = ""
            if v < len(names): note = f"name:{names[v]}"
            elif sv < 0 and -sv - 1 < len(imports): note = f"-> {pidx(sv,imports,exports)}"
            print(f"  +{i:03x} {c.hex()}  {sv:<12} {note}")
        print()

if __name__ == "__main__":
    main()
