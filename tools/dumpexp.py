#!/usr/bin/env python3
"""usage: dumpexp.py <base-path-without-ext> <export-index> [count]"""
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
        if n < 0:                                   # UTF-16, as FString reads a negative length
            t = s.b[s.o:s.o - 2 * n - 2].decode('utf-16-le', 'replace'); s.o -= 2 * n; return t
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
        # Uncooked: FObjectImport carries an editor-only PackageName FName (VER_UE4_NON_OUTER_PACKAGE_IMPORT).
        if not (flags & 0x80000000): im.o += 8
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
    # An uncooked package is one file: payloads sit in the .uasset at absolute offsets, so hand
    # callers the same (blob, base) pair a cooked pair gives them.
    if not ue: ue, total = ua, 0
    return ua, ue, total, names, imports, exports

def preload(base):
    """Each export's EDL preload dependencies, as FPackageIndex lists: serialize-before-serialize, create-before-
    serialize, serialize-before-create, create-before-create. A cooked package only (the summary walk of Cooked.cpp)."""
    ua = open(base + ".uasset", "rb").read()
    r = R(ua, 8)                                # past the tag and LegacyFileVersion (-7)
    def skip(size):                             # a counted array of fixed-size rows. Not `r.o += size * r.i32()`:
        n = r.i32(); r.o += size * n            # that reads r.o first, and drops the count's own 4 bytes
    r.o += 12                                  # LegacyUE3Version, FileVersionUE4, licensee
    skip(20)                                    # custom versions
    r.i32(); r.fstr(); r.u32()                  # TotalHeaderSize, FolderName, PackageFlags
    r.o += 16                                   # names, gatherable text
    ecount, eoff = r.i32(), r.i32()
    r.o += 8 + 4 + 8 + 4 + 4 + 16               # imports, depends, soft package references, searchable names, thumbnails, guid
    skip(8)                                     # generations
    for _ in range(2):                          # saved-by and compatible-with engine versions
        r.o += 10; r.fstr()
    r.u32(); skip(16)                           # compression flags, compressed chunks
    r.u32()                                     # PackageSource
    for _ in range(r.i32()): r.fstr()           # additional packages to cook
    r.i32(); r.i64(); r.i32()                   # asset registry, bulk data start, world tile info
    skip(4)                                     # chunk ids
    count, off = r.i32(), r.i32()
    deps = struct.unpack_from('<%di' % count, ua, off)
    out = []
    for k in range(ecount):
        first, *counts = struct.unpack_from('<5i', ua, eoff + 104 * k + 84)
        lists, at = [], first
        for n in counts:
            lists.append(list(deps[at:at + n]) if first >= 0 else [])
            at += n
        out.append(lists)
    return out

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
