#!/usr/bin/env python3
"""usage: repath.py <in-base> <out-base> <old>=<new> ...

Rewrites whole entries of a cooked 4.27 package's name table: its own package path, the packages and objects it
imports, a name it stores. Each <old> must be an entry; <new> must not already be one. The table is the first thing
after the summary, so every offset past it moves by the change in its size: the summary's, each export's
SerialOffset. <in-base> and <out-base> are paths without .uasset/.uexp.

The package's own asset-registry block (object path and tags) is left as it is, so keep the asset's object name and
change only its package path: `/Old/Dir/Asset=/New/Dir/Asset`. A package whose block names objects, or that
carries text to gather, soft package references or searchable names (whose offsets this does not move), is refused.
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from dumpexp import R  # noqa: E402

DEPRECATED, REFLECTED = [], []
for i in range(256):
    c = i << 24
    for _ in range(8):
        c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
    DEPRECATED.append(c)
    c = i
    for _ in range(8):
        c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
    REFLECTED.append(c)


def strihash(s):
    """FCrc::Strihash_DEPRECATED over an ANSI name (AssetGen/Package.cpp)."""
    h = 0
    for ch in s.encode("latin-1"):
        u = ch - 32 if 97 <= ch <= 122 else ch
        h = ((h >> 8) & 0x00FFFFFF) ^ DEPRECATED[(h ^ u) & 0xFF]
    return h


def strcrc32(s):
    """FCrc::StrCrc32: all four bytes of each widened TCHAR."""
    c = 0xFFFFFFFF
    for ch in s.encode("latin-1"):
        for shift in (0, 8, 16, 24):
            c = (c >> 8) ^ REFLECTED[(c ^ ((ch >> shift) & 0xFF)) & 0xFF]
    return c ^ 0xFFFFFFFF


def skip(r, size):
    """A counted array of fixed-size rows. Not `r.o += size * r.i32()`, which reads r.o before the count."""
    n = r.i32()
    r.o += size * n


def fstring(s):
    return struct.pack("<i", len(s) + 1) + s.encode("latin-1") + b"\0"


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    renames = dict(a.split("=", 1) for a in sys.argv[3:])
    ua = bytearray(open(src + ".uasset", "rb").read())
    uexp = open(src + ".uexp", "rb").read()

    # The summary, noting where each offset that points past the name table sits.
    r = R(ua, 4)
    if r.i32() != -7:
        sys.exit("not a 4.27 package")
    r.o += 4 + 4 + 4
    skip(r, 20)                                     # custom versions
    slots = [("i32", r.o)]; total = r.i32()         # TotalHeaderSize
    r.fstr(); flags = r.u32()
    if not flags & 0x80000000:
        sys.exit("not a cooked package (no FilterEditorOnly)")
    ncount = r.i32(); noff = r.i32()
    gcount = r.i32(); r.i32()
    r.i32(); slots.append(("i32", r.o)); r.i32()    # exports
    r.i32(); slots.append(("i32", r.o)); r.i32()    # imports
    slots.append(("i32", r.o)); r.i32()             # depends
    scount = r.i32(); r.i32()
    searchable = r.i32(); thumbs = r.i32()
    if gcount or scount or searchable or thumbs:
        sys.exit("gatherable text, soft package references, searchable names or thumbnails: offsets not handled")
    r.o += 16
    skip(r, 8)                                      # generations
    for _ in range(2):
        r.o += 10; r.fstr()
    r.u32(); skip(r, 16); r.u32()
    for _ in range(r.i32()):
        r.fstr()
    slots.append(("i32", r.o)); arof = r.i32()      # asset registry data
    slots.append(("i64", r.o)); r.i64()             # bulk data start
    tile = r.i32()
    if tile:
        sys.exit("world tile info: offset not handled")
    skip(r, 4)                                      # chunk ids
    r.i32(); slots.append(("i32", r.o)); r.i32()    # preload dependencies
    if struct.unpack_from("<i", ua, arof)[0] != 0:
        sys.exit("the asset registry block names objects; this keeps it as it is, so it would go stale")

    # The name table, rewritten.
    n = R(ua, noff)
    names, out = [], bytearray()
    for _ in range(ncount):
        s = n.fstr(); hashes = struct.unpack_from("<HH", ua, n.o); n.o += 4
        if hashes != (strihash(s) & 0xFFFF, strcrc32(s) & 0xFFFF):
            sys.exit("name %r: stored hashes differ from the computed ones" % s)   # the control on the hash code
        names.append(s)
    for old, new in renames.items():
        if old not in names:
            sys.exit("%r is not in the name table" % old)
        if new in names:
            sys.exit("%r is already in the name table" % new)
    for s in names:
        s = renames.get(s, s)
        out += fstring(s) + struct.pack("<HH", strihash(s) & 0xFFFF, strcrc32(s) & 0xFFFF)
    end = n.o
    delta = len(out) - (end - noff)

    # Every offset past the table moves by delta: each export's SerialOffset (read where the table still is), then
    # the summary's.
    eoff = struct.unpack_from("<i", ua, slots[1][1])[0]
    ecount = struct.unpack_from("<i", ua, slots[1][1] - 4)[0]
    for k in range(ecount):
        at = eoff + 104 * k + 36                    # SerialSize at +28, SerialOffset at +36
        struct.pack_into("<q", ua, at, struct.unpack_from("<q", ua, at)[0] + delta)
    for kind, at in slots:
        fmt = "<i" if kind == "i32" else "<q"
        v = struct.unpack_from(fmt, ua, at)[0]
        if v >= end:
            struct.pack_into(fmt, ua, at, v + delta)
    res = bytes(ua[:noff]) + bytes(out) + bytes(ua[end:])
    if len(res) != total + delta:
        sys.exit("header size: %d, expected %d" % (len(res), total + delta))
    open(dst + ".uasset", "wb").write(res)
    open(dst + ".uexp", "wb").write(uexp)
    print("%s: %d names renamed, header %+d bytes" % (dst, len(renames), delta))


if __name__ == "__main__":
    main()
