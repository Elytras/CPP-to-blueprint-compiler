#!/usr/bin/env python3
"""Print a cooked package's event-driven-loader preload dependency table, with names resolved.

The EDL table is the part of a package that decides load ORDER, and getting it wrong does not
corrupt anything - it produces "Missing Dependency, request for X but it was still waiting for
serialization" at runtime and nothing at all offline. So it is worth reading out of assets that
are known to load, rather than reasoned about.

Each export declares four lists, in the order the loader reads them:
  SerBeforeSer     these must be SERIALIZED before I am serialized
  CreateBeforeSer  these must be CREATED before I am serialized
  SerBeforeCreate  these must be SERIALIZED before I am created
  CreateBeforeCre  these must be CREATED before I am created

usage: dumpedl.py <file.uasset>
"""
import io
import struct
import sys


class Reader(object):
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]
        self.p += 4
        return v

    def i64(self):
        v = struct.unpack_from("<q", self.d, self.p)[0]
        self.p += 8
        return v

    def fstring(self):
        n = self.i32()
        if n == 0:
            return ""
        if n < 0:                                   # UTF-16, negative length
            s = self.d[self.p:self.p - 2 * n - 2].decode("utf-16-le", "replace")
            self.p += -2 * n
            return s
        s = self.d[self.p:self.p + n - 1].decode("latin-1", "replace")
        self.p += n
        return s


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    data = io.open(sys.argv[1], "rb").read()

    r = Reader(data, 4)                             # skip the magic
    r.i32()                                         # LegacyFileVersion
    r.i32()                                         # LegacyUE3Version
    r.i32()                                         # FileVersionUE4
    r.i32()                                         # FileVersionLicenseeUE4
    ncustom = r.i32()
    r.p += ncustom * 20
    r.i32()                                         # TotalHeaderSize
    r.fstring()                                     # FolderName
    r.i32()                                         # PackageFlags
    name_count, name_off = r.i32(), r.i32()
    # LocalizationId and PersistentGuid are absent: these packages set PKG_FilterEditorOnly.
    r.i32(); r.i32()                                # GatherableTextData count/offset
    export_count, export_off = r.i32(), r.i32()
    import_count, import_off = r.i32(), r.i32()
    r.i32()                                         # DependsOffset
    r.i32(); r.i32()                                # SoftPackageReferences count/offset
    r.i32()                                         # SearchableNamesOffset
    r.i32()                                         # ThumbnailTableOffset
    r.p += 16                                       # package Guid
    for _ in range(r.i32()):                        # generations
        r.i32(); r.i32()
    r.p += 2 * 14                                   # SavedBy / CompatibleWith engine versions
    r.i32()                                         # CompressionFlags
    r.i32()                                         # CompressedChunks
    r.i32()                                         # PackageSource
    r.i32()                                         # AdditionalPackagesToCook
    r.i32()                                         # AssetRegistryOffset
    r.i64()                                         # BulkDataStartOffset
    r.i32()                                         # WorldTileInfoDataOffset
    r.i32()                                         # ChunkIDs
    preload_count, preload_off = r.i32(), r.i32()

    names = []
    n = Reader(data, name_off)
    for _ in range(name_count):
        names.append(n.fstring())
        n.p += 4                                    # the two hashes

    def name_at(rd):
        idx, num = rd.i32(), rd.i32()
        base = names[idx] if 0 <= idx < len(names) else "<bad name %d>" % idx
        return base if num == 0 else "%s_%d" % (base, num - 1)

    imports = []
    im = Reader(data, import_off)
    for _ in range(import_count):
        name_at(im)                                 # ClassPackage
        cls = name_at(im)
        im.i32()                                    # OuterIndex
        imports.append((cls, name_at(im)))

    exports, dep_meta = [], []
    ex = Reader(data, export_off)
    for _ in range(export_count):
        ex.i32(); ex.i32(); ex.i32(); ex.i32()      # class/super/template/outer
        obj = name_at(ex)
        ex.i32()                                    # ObjectFlags
        ex.i64(); ex.i64()                          # SerialSize / SerialOffset
        ex.p += 3 * 4 + 16 + 4 + 2 * 4              # flags, guid, package flags, two bools
        first = ex.i32()
        counts = (ex.i32(), ex.i32(), ex.i32(), ex.i32())
        exports.append(obj)
        dep_meta.append((first, counts))

    # The summary states where the table is; every dep index is relative to its start.
    total = preload_count
    deps = list(struct.unpack_from("<%di" % total, data, preload_off)) if total else []

    def label(v):
        if v == 0:
            return "null"
        if v > 0:
            i = v - 1
            return "exp:%s" % (exports[i] if i < len(exports) else "?%d" % v)
        i = -v - 1
        if i >= len(imports):
            return "imp:?%d" % v
        return "imp:%s(%s)" % (imports[i][1], imports[i][0])

    print("%s  (%d exports, %d imports, %d preload entries)"
          % (sys.argv[1], export_count, import_count, total))
    phases = ["SerBeforeSer", "CreateBeforeSer", "SerBeforeCreate", "CreateBeforeCre"]
    for i, obj in enumerate(exports):
        first, counts = dep_meta[i]
        print("\n[%d] %s" % (i, obj))
        if first < 0:
            print("      (no preload dependencies)")
            continue
        at = first
        for phase, count in zip(phases, counts):
            if not count:
                continue
            print("      %-16s %s" % (phase, ", ".join(label(v) for v in deps[at:at + count])))
            at += count


main()
