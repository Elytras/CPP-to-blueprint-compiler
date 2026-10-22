#!/usr/bin/env python3
"""usage: dumpar.py <AssetRegistry.bin>"""
import struct
import sys

BEGIN_MAGIC = 0x12345679
END_MAGIC = 0x87654321
VERSION_GUID = (0x717F9EE7, 0xE9B0493A, 0x88B39132, 0x1B388107)
NUMBERED_BIT = 0x80000000
STORE_VIEWS = 11


class Reader(object):
    def __init__(self, data):
        self.d = data
        self.p = 0

    def take(self, n):
        out = self.d[self.p:self.p + n]
        if len(out) != n:
            sys.exit("truncated at 0x%x: wanted %d bytes" % (self.p, n))
        self.p += n
        return out

    def u32(self):
        return struct.unpack_from("<I", self.take(4))[0]

    def i32(self):
        return struct.unpack_from("<i", self.take(4))[0]

    def u64(self):
        return struct.unpack_from("<Q", self.take(8))[0]

    def i64(self):
        return struct.unpack_from("<q", self.take(8))[0]


def read_name_batch(r):
    """Counts, then hashes / 2-byte headers / string bytes as three runs."""
    count = r.u32()
    if count == 0:
        return []
    string_bytes = r.u32()
    r.u64()                                     # hash algorithm id
    r.take(8 * count)                           # hashes

    headers = [r.take(2) for _ in range(count)]
    strings = r.take(string_bytes)

    names, at = [], 0
    for h in headers:
        wide = h[0] & 0x80
        length = ((h[0] & 0x7F) << 8) | h[1]
        size = length * (2 if wide else 1)
        at += wide and at & 1                   # the loader aligns a UTF-16 name to 2 bytes
        raw = strings[at:at + size]
        at += size
        names.append(raw.decode("utf-16-le" if wide else "latin-1"))   # ANSI widens byte by byte, as the loader does
    if at != len(strings):
        sys.exit("name batch: %d string bytes unread" % (len(strings) - at))
    return names


def skip_name(r):
    if r.u32() & NUMBERED_BIT:
        r.u32()


def skip_string(r):
    n = r.i32()
    r.take(-2 * n if n < 0 else n)


def read_store(r, name):
    """The tag values (4.27 FixedTagPrivate::FStore), as a function from a row's map handle to its tags. The counts are
    in the store's member order, the data texts first, then the rest in that order. A value names its view in its low
    3 bits (EValueType) and its index in the rest; a text value reads as <text>, as nothing here decodes them."""
    if r.u32() != BEGIN_MAGIC:
        sys.exit("tag store: bad begin magic")
    (numberless_names, names, numberless_paths, paths, _texts, ansi_offsets, wide_offsets, ansi_chars, wide_chars,
     numberless_pairs, pairs) = [r.i32() for _ in range(STORE_VIEWS)]
    r.take(r.i32())                             # the texts, sized so a reader can step over them

    def path():
        cls, obj, pkg = name(), name(), name()  # FAssetRegistryExportPath: Class, Object, Package
        out = pkg + ("." + obj if obj != "None" else "")
        return "%s'%s'" % (cls, out) if cls != "None" else out

    value_names = [name() for _ in range(numberless_names + names)]
    value_paths = [path() for _ in range(numberless_paths + paths)]
    ansi_at = [r.u32() for _ in range(ansi_offsets)]
    wide_at = [r.u32() for _ in range(wide_offsets)]
    ansi = r.take(ansi_chars)
    wide = r.take(2 * wide_chars)
    keyed = [(name(), r.u32()) for _ in range(numberless_pairs)]
    numbered = [(name(), r.u32()) for _ in range(pairs)]
    if r.u32() != END_MAGIC:
        sys.exit("tag store: bad end magic")

    def value(v):
        kind, i = v & 7, v >> 3
        if kind == 0:
            return ansi[ansi_at[i]:ansi.index(b"\0", ansi_at[i])].decode("latin-1")
        if kind == 1:
            end = next(k for k in range(2 * wide_at[i], len(wide), 2) if wide[k:k + 2] == b"\0\0")
            return wide[2 * wide_at[i]:end].decode("utf-16-le")
        if kind in (2, 3):
            return value_names[i if kind == 2 else numberless_names + i]
        if kind in (4, 5):
            return value_paths[i if kind == 4 else numberless_paths + i]
        return "<text>"

    def tags(handle):
        pairs_of = keyed if handle >> 63 else numbered
        begin, num = handle & 0xFFFFFFFF, (handle >> 32) & 0xFFFF
        return [(k, value(v)) for k, v in pairs_of[begin:begin + num]]
    return tags


def read(path):
    """Every asset row of an AssetRegistry.bin, assetgen's or a cooked game's, as dicts; tags are kept as (key, value)
    pairs, bundles and chunks are counted. Exits unless the whole file parses."""
    r = Reader(open(path, "rb").read())
    guid = tuple(r.u32() for _ in range(4))
    if guid != VERSION_GUID:
        sys.exit("not an asset registry: guid %s" % (guid,))
    version = r.i32()
    names = read_name_batch(r)

    def name():
        idx = r.u32()
        if not idx & NUMBERED_BIT:
            return names[idx]
        base = names[idx & ~NUMBERED_BIT]
        return "%s_%d" % (base, r.u32() - 1)

    tags = read_store(r, name)
    rows = []
    for _ in range(r.i32()):
        row = dict(object_path=name(), package_path=name(), asset_class=name(), package_name=name(),
                   asset_name=name(), tags=tags(r.u64()), bundles=r.i32())
        for _ in range(row["bundles"]):
            skip_name(r)
            for _ in range(r.i32()):
                skip_name(r)
                skip_string(r)
        row["chunks"] = r.i32()
        r.take(4 * row["chunks"])
        row["flags"] = r.u32()
        rows.append(row)

    end = r.p + 8 + r.i64()
    dependencies = r.i32()
    r.p = end
    packages = r.i32()
    for _ in range(packages):                   # FAssetPackageData: name, disk size, guid, cooked MD5
        skip_name(r)
        r.take(8 + 16)
        if r.u32():
            r.take(16)
    if r.p != len(r.d):
        sys.exit("%d trailing byte(s) after the body" % (len(r.d) - r.p))
    return version, len(names), rows, dependencies, packages, len(r.d)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    version, names, rows, dependencies, packages, size = read(sys.argv[1])
    print("version %d" % version)
    print("%d asset(s), %d name(s)" % (len(rows), names))
    for a in rows:
        print("  %-52s %s" % (a["object_path"], a["asset_class"]))
        print("     package %s   path %s   asset %s" % (a["package_name"], a["package_path"], a["asset_name"]))
        print("     flags 0x%08X  tags %d  bundles %d  chunks %d" % (a["flags"], len(a["tags"]), a["bundles"], a["chunks"]))
        for key, value in a["tags"]:
            print("     tag %s = %s" % (key, value))
    print("dependencies: %d node(s)" % dependencies)
    print("package data: %d entr(y/ies)" % packages)
    print("OK - consumed the whole file (%d bytes)" % size)


if __name__ == "__main__":
    main()
