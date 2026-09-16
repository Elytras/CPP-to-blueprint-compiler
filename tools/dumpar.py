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
        raw = strings[at:at + size]
        at += size
        names.append(raw.decode("utf-16-le" if wide else "ascii"))
    if at != len(strings):
        sys.exit("name batch: %d string bytes unread" % (len(strings) - at))
    return names


def read_store(r):
    if r.u32() != BEGIN_MAGIC:
        sys.exit("tag store: bad begin magic")
    counts = [r.i32() for _ in range(STORE_VIEWS)]
    text_bytes = r.i32()
    r.take(text_bytes)
    if any(counts) or text_bytes:
        sys.exit("tag store is not empty (%r) - this reader only knows the empty form" % counts)
    if r.u32() != END_MAGIC:
        sys.exit("tag store: bad end magic")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    r = Reader(open(sys.argv[1], "rb").read())

    guid = tuple(r.u32() for _ in range(4))
    if guid != VERSION_GUID:
        sys.exit("not an asset registry: guid %s" % (guid,))
    print("version %d" % r.i32())

    names = read_name_batch(r)
    read_store(r)

    def name():
        idx = r.u32()
        if not idx & NUMBERED_BIT:
            return names[idx]
        base = names[idx & ~NUMBERED_BIT]
        return "%s_%d" % (base, r.u32() - 1)

    count = r.i32()
    print("%d asset(s), %d name(s)" % (count, len(names)))
    for _ in range(count):
        object_path, package_path, asset_class = name(), name(), name()
        package_name, asset_name = name(), name()
        tags, bundles, chunks, flags = r.u64(), r.i32(), r.i32(), r.u32()
        print("  %-52s %s" % (object_path, asset_class))
        print("     package %s   path %s   asset %s" % (package_name, package_path, asset_name))
        print("     flags 0x%08X  tagmap 0x%X  bundles %d  chunks %d" % (flags, tags, bundles, chunks))

    size = r.i64()
    end = r.p + size
    print("dependencies: %d node(s)" % r.i32())
    r.p = end
    print("package data: %d entr(y/ies)" % r.i32())

    if r.p != len(r.d):
        sys.exit("%d trailing byte(s) after the body" % (len(r.d) - r.p))
    print("OK - consumed the whole file (%d bytes)" % len(r.d))


main()
