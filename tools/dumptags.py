#!/usr/bin/env python3
"""usage: dumptags.py <base-path-without-ext> <export-index> [byte-offset]
The export's tagged properties, decoded. A UserDefinedStruct's default instance starts after its
UStruct body, so pass that offset to read it."""
import struct
import sys

from dumpexp import load


def fstring(b, o):
    n = struct.unpack_from('<i', b, o)[0]
    o += 4
    if n == 0:
        return "", o
    if n < 0:
        return b[o:o - 2 * n - 2].decode('utf-16-le', 'replace'), o - 2 * n
    return b[o:o + n - 1].decode('latin-1', 'replace'), o + n


def tags(b, o, end, names, indent, out):
    """Append one line per FPropertyTag until None; raises on a bad name index, so a binary
    struct payload is detected rather than printed as garbage."""
    def name(at):
        i, num = struct.unpack_from('<ii', b, at)
        if not 0 <= i < len(names) or num < 0:
            raise ValueError("not a name")
        return (names[i] + "_%d" % (num - 1) if num else names[i]), at + 8

    pad = "  " * indent
    while o + 8 <= end:
        pname, o = name(o)
        if pname == "None":
            return o
        ptype, o = name(o)
        size, index = struct.unpack_from('<ii', b, o)
        o += 8
        extra = ""
        if ptype == "StructProperty":
            sname, o = name(o)
            extra = " struct=%s" % sname
            o += 16
        elif ptype == "BoolProperty":
            extra = " value=%d" % b[o]
            o += 1
        elif ptype in ("ByteProperty", "EnumProperty"):
            ename, o = name(o)
            extra = " enum=%s" % ename
        elif ptype in ("ArrayProperty", "SetProperty"):
            inner, o = name(o)
            extra = " inner=%s" % inner
        elif ptype == "MapProperty":
            key, o = name(o)
            val, o = name(o)
            extra = " key=%s value=%s" % (key, val)
        if b[o]:
            o += 16
        o += 1
        shown = b[o:o + size].hex()
        if ptype in ("StrProperty", "TextProperty") and size >= 4:
            shown = b[o:o + size].hex() if ptype == "TextProperty" else repr(fstring(b, o)[0])
        elif ptype == "NameProperty" or (ptype in ("ByteProperty", "EnumProperty") and size == 8):
            shown = name(o)[0]
        elif ptype == "IntProperty":
            shown = str(struct.unpack_from('<i', b, o)[0])
        elif ptype == "Int64Property":
            shown = str(struct.unpack_from('<q', b, o)[0])
        elif ptype == "FloatProperty":
            shown = str(struct.unpack_from('<f', b, o)[0])
        elif ptype == "ObjectProperty":
            shown = "index %d" % struct.unpack_from('<i', b, o)[0]
        out.append("%s%s [%d] %s size=%d%s: %s" % (pad, pname, index, ptype, size, extra, shown))
        if ptype == "StructProperty" and size >= 8:
            nested = []
            try:
                tags(b, o, o + size, names, indent + 1, nested)
                out.extend(nested)
            except (ValueError, struct.error, IndexError):
                pass
        o += size
    return o


def main():
    base, idx = sys.argv[1], int(sys.argv[2])
    start = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
    ua, ue, total, names, imports, exports = load(base)
    e = exports[idx]
    blob = ue[e['off'] - total: e['off'] - total + e['size']]
    print("=== export[%d] %s" % (idx, e['name']))
    out = []
    tags(blob, start, len(blob), names, 1, out)
    print("\n".join(out))


if __name__ == "__main__":
    main()
