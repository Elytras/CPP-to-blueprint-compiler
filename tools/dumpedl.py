#!/usr/bin/env python3
"""usage: dumpedl.py <file.uasset>"""
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


def engine_version(r):
    r.p += 3 * 2 + 4                                # major, minor, patch, changelist
    r.fstring()                                     # branch: empty in AssetGen's, "++UE4+Release-4.27" in an editor cook


def read_tables(data):
    """A cooked .uasset's name map, imports as (class name, object name), exports and preload dependencies."""
    # UE4.27 PackageFileSummary.cpp, PKG_FilterEditorOnly form (no LocalizationId / PersistentGuid).
    r = Reader(data, 4)                             # magic
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
    engine_version(r)                               # SavedByEngineVersion
    engine_version(r)                               # CompatibleWithEngineVersion
    r.i32()                                         # CompressionFlags
    r.i32()                                         # CompressedChunks
    r.i32()                                         # PackageSource
    for _ in range(r.i32()):                        # AdditionalPackagesToCook
        r.fstring()
    r.i32()                                         # AssetRegistryOffset
    r.i64()                                         # BulkDataStartOffset
    r.i32()                                         # WorldTileInfoDataOffset
    chunks = r.i32()                                # ChunkIDs
    r.p += 4 * chunks
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

    exports = []
    ex = Reader(data, export_off)
    for _ in range(export_count):
        cls = ex.i32()
        ex.i32(); ex.i32()                          # super / template
        outer = ex.i32()
        obj = name_at(ex)
        ex.i32()                                    # ObjectFlags
        ex.i64(); ex.i64()                          # SerialSize / SerialOffset
        ex.p += 3 * 4 + 16 + 4 + 4                  # forced / not-for-client / -server, guid, package flags, bool
        is_asset = ex.i32() != 0
        first = ex.i32()
        counts = (ex.i32(), ex.i32(), ex.i32(), ex.i32())
        exports.append({"class": cls, "outer": outer, "name": obj, "is_asset": is_asset, "first": first, "counts": counts})

    deps = list(struct.unpack_from("<%di" % preload_count, data, preload_off)) if preload_count else []
    return names, imports, exports, deps


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    _names, imports, exports, deps = read_tables(io.open(sys.argv[1], "rb").read())
    total = len(deps)

    def label(v):
        if v == 0:
            return "null"
        if v > 0:
            i = v - 1
            return "exp:%s" % (exports[i]["name"] if i < len(exports) else "?%d" % v)
        i = -v - 1
        if i >= len(imports):
            return "imp:?%d" % v
        return "imp:%s(%s)" % (imports[i][1], imports[i][0])

    print("%s  (%d exports, %d imports, %d preload entries)"
          % (sys.argv[1], len(exports), len(imports), total))
    phases = ["SerBeforeSer", "CreateBeforeSer", "SerBeforeCreate", "CreateBeforeCre"]
    for i, e in enumerate(exports):
        first, counts = e["first"], e["counts"]
        print("\n[%d] %s" % (i, e["name"]))
        if first < 0:
            print("      (no preload dependencies)")
            continue
        at = first
        for phase, count in zip(phases, counts):
            if not count:
                continue
            print("      %-16s %s" % (phase, ", ".join(label(v) for v in deps[at:at + count])))
            at += count


if __name__ == "__main__":
    main()
