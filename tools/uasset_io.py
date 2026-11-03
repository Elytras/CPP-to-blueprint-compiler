#!/usr/bin/env python3
"""The one package reader: a byte reader with UE's FString, the FPackageFileSummary of a 4.27 or a 5.x package, and
its name, import and export tables. uasset.py's subcommands, invariants.py, runscript.py and the other tools read
packages through it."""
import struct

PKG_FLAGS = [
    (0x00000001, "NewlyCreated"), (0x00000002, "ClientOptional"), (0x00000004, "ServerSideOnly"),
    (0x00000010, "CompiledIn"), (0x00000020, "ForDiffing"), (0x00000040, "EditorOnly"),
    (0x00000080, "Developer"), (0x00000100, "UncookedOnly"), (0x00000200, "Cooked"),
    (0x00000400, "ContainsNoAsset"), (0x00002000, "UnversionedProperties"),
    (0x00004000, "ContainsMapData"), (0x00010000, "Compiling"), (0x00020000, "ContainsMap"),
    (0x00040000, "RequiresLocalizationGather"), (0x00100000, "PlayInEditor"),
    (0x00200000, "ContainsScript"), (0x00400000, "DisallowExport"), (0x10000000, "DynamicImports"),
    (0x20000000, "RuntimeGenerated"), (0x40000000, "ReloadingForCooker"), (0x80000000, "FilterEditorOnly"),
]
PACKAGE_FILE_TAG = 0x9E2A83C1
FILTER_EDITOR_ONLY, UNVERSIONED_PROPERTIES = 0x80000000, 0x2000
UE4_427, UE5_56 = 522, 1017                         # what an unversioned package is read at: the engine's own version


def fstring(b, o):
    """An FString at o: (text, the offset past it). A negative length is UTF-16, as FString reads it."""
    n = struct.unpack_from('<i', b, o)[0]
    o += 4
    if n == 0:
        return "", o
    if n < 0:
        return b[o:o - 2 * n - 2].decode('utf-16-le', 'replace'), o - 2 * n
    return b[o:o + n - 1].decode('latin-1', 'replace'), o + n


class R:
    def __init__(s, b, o=0): s.b, s.o = b, o
    def i32(s): v = struct.unpack_from('<i', s.b, s.o)[0]; s.o += 4; return v
    def u32(s): v = struct.unpack_from('<I', s.b, s.o)[0]; s.o += 4; return v
    def i64(s): v = struct.unpack_from('<q', s.b, s.o)[0]; s.o += 8; return v
    def u64(s): v = struct.unpack_from('<Q', s.b, s.o)[0]; s.o += 8; return v
    def u16(s): v = struct.unpack_from('<H', s.b, s.o)[0]; s.o += 2; return v
    def u8(s): v = s.b[s.o]; s.o += 1; return v
    def fstr(s): t, s.o = fstring(s.b, s.o); return t
    def guid(s): v = struct.unpack_from('<4I', s.b, s.o); s.o += 16; return "%08X%08X%08X%08X" % v
    def engine_version(s):                          # FEngineVersion: uint16 major/minor/patch, uint32 CL, branch
        return (s.u16(), s.u16(), s.u16(), s.u32(), s.fstr())


class Imp(str):
    """An import as the dumps print it, Class'Name'; .outer is its outer's FPackageIndex."""


def decode_flags(f):
    return " | ".join(n for m, n in PKG_FLAGS if f & m) or "None"


def summary(ua, ue5_version=UE5_56):
    """Every FPackageFileSummary field, in the order PackageFileSummary.cpp reads them (4.27, and 5.6's serializer for
    LegacyFileVersion -8/-9), plus each table row's size. An unversioned UE5 package is read at ue5_version, which the
    file does not record (the engine reads it at its own); `ue5` is whether the package is a UE5 one at all. A cooked
    package sets PKG_FilterEditorOnly and so has no LocalizationId, PersistentGuid or import PackageName."""
    r = R(ua)
    s = dict(tag=r.u32())
    s['legacy'] = legacy = r.i32()
    if legacy != -4: r.i32()                        # LegacyUE3Version
    ue4 = r.i32()
    s['ue5'] = legacy <= -8
    ue5 = s['file_ue5'] = r.i32() if s['ue5'] else 0
    s['licensee'] = lic = r.i32()
    if s['ue5'] and not (ue4 or ue5 or lic):        # unversioned
        ue4, ue5 = UE4_427, ue5_version
    s['ue4'], s['ue5_version'] = ue4, ue5
    ue4 = ue4 or UE4_427                            # an unversioned 4.27 package reads at 4.27's own version
    s['saved_hash'] = None
    if ue5 >= 1016:                                 # PACKAGE_SAVED_HASH: FIoHash, then TotalHeaderSize
        s['saved_hash'] = ua[r.o:r.o + 20].hex(); r.o += 20
        s['total'] = r.i32()
    s['customs'] = [(r.guid(), r.i32()) for _ in range(r.i32())]
    if ue5 < 1016: s['total'] = r.i32()
    s['folder'] = r.fstr()
    s['flags'] = flags = r.u32()
    editor = not flags & FILTER_EDITOR_ONLY
    s['ncount'], s['noff'] = r.i32(), r.i32()
    s['soft_paths'] = (r.i32(), r.i32()) if ue5 >= 1008 else None             # ADD_SOFTOBJECTPATH_LIST
    if editor and ue4 >= 516: r.fstr()              # LocalizationId (VER_UE4_ADDED_PACKAGE_SUMMARY_LOCALIZATION_ID)
    s['gather'] = (r.i32(), r.i32())
    s['ecount'], s['eoff'] = r.i32(), r.i32()
    s['icount'], s['ioff'] = r.i32(), r.i32()
    s['cells'] = (r.i32(), r.i32(), r.i32(), r.i32()) if ue5 >= 1015 else None   # VERSE_CELLS
    s['metadata_off'] = r.i32() if ue5 >= 1014 else None                     # METADATA_SERIALIZATION_OFFSET
    s['depends_off'] = r.i32()
    s['soft_count'], s['soft_off'] = r.i32(), r.i32()
    s['searchable_off'], s['thumbnail_off'] = r.i32(), r.i32()
    s['guid'] = r.guid() if ue5 < 1016 else s['saved_hash']
    if editor and ue4 >= 518: r.guid()              # PersistentGuid (VER_UE4_ADDED_PACKAGE_OWNER)
    s['generations'] = [(r.i32(), r.i32()) for _ in range(r.i32())]
    s['engine'], s['compatible'] = r.engine_version(), r.engine_version()
    s['compression_flags'] = r.u32()
    s['chunk_count'] = r.i32()                      # CompressedChunks, none in a cooked package
    r.o += 16 * s['chunk_count']
    s['package_source'] = r.u32()
    for _ in range(r.i32()): r.fstr()               # AdditionalPackagesToCook
    s['asset_registry_off'], s['bulk_start'], s['world_tile_off'] = r.i32(), r.i64(), r.i32()
    n = r.i32(); r.o += 4 * n                       # ChunkIDs: not `r.o += 4 * r.i32()`, which reads r.o first
    s['pcount'], s['poff'] = r.i32(), r.i32()
    s['names_from_export'] = r.i32() if ue5 >= 1001 else None                # NAMES_REFERENCED_FROM_EXPORT_DATA
    s['payload_toc'] = r.i64() if ue5 >= 1002 else None                      # PAYLOAD_TOC
    s['data_resource'] = r.i32() if ue5 >= 1009 else None                    # DATA_RESOURCES
    s['end'] = r.o

    # The table rows (ObjectResource.cpp). From 5.x an import gains bImportOptional; an export loses its PackageGuid,
    # gains bIsInheritedInstance and bGeneratePublicHash, and in a tagged package the script start/end (two int64):
    tagged = not flags & UNVERSIONED_PROPERTIES and ue5 >= 1010
    s['import_package_name'] = editor and ue4 >= 520                         # VER_UE4_NON_OUTER_PACKAGE_IMPORT
    s['irow'] = 28 + 8 * s['import_package_name'] + 4 * (ue5 >= 1003)
    s['erow'] = 104 - 16 * (ue5 >= 1005) + 4 * (ue5 >= 1006) + 4 * (ue5 >= 1003) + 16 * tagged
    s['edeps'] = s['erow'] - 20 - 16 * tagged       # FirstExportDependency and the four counts
    return s


def tables(ua, sm):
    """The name map, and the import and export rows with their FNames as (index, number) pairs, unresolved, so each
    caller names them its own way."""
    names = []
    r = R(ua, sm['noff'])
    for _ in range(sm['ncount']):
        names.append(r.fstr()); r.o += 4            # the two hashes
    imports = []
    for k in range(sm['icount']):
        v = struct.unpack_from('<7i', ua, sm['ioff'] + sm['irow'] * k)
        imports.append(dict(class_package=v[0:2], class_name=v[2:4], outer=v[4], name=v[5:7]))
    exports = []
    ue5 = sm['ue5_version']
    for k in range(sm['ecount']):
        r = R(ua, sm['eoff'] + sm['erow'] * k)
        e = dict(cls=r.i32(), super=r.i32(), tmpl=r.i32(), outer=r.i32(), name=(r.i32(), r.i32()), flags=r.u32())
        e['size'], e['off'] = r.i64(), r.i64()
        e['forced'], e['not_client'], e['not_server'] = r.i32(), r.i32(), r.i32()
        if ue5 < 1005: r.o += 16                    # PackageGuid
        if ue5 >= 1006: r.o += 4                    # bIsInheritedInstance
        e['package_flags'] = r.u32()
        e['not_always_loaded'], e['is_asset'] = r.i32(), r.i32()
        if ue5 >= 1003: r.o += 4                    # bGeneratePublicHash
        e['first'], e['counts'] = r.i32(), (r.i32(), r.i32(), r.i32(), r.i32())
        exports.append(e)
    return names, imports, exports


def load(base):
    """A package by its path without .uasset/.uexp: (uasset bytes, uexp bytes, the header size export offsets count
    from, names, imports as Imp, exports as dicts of cls/super/tmpl/outer FPackageIndex, name, flags, size, off)."""
    ua = open(base + ".uasset", "rb").read()
    try: ue = open(base + ".uexp", "rb").read()
    except FileNotFoundError: ue = b""
    sm = summary(ua)
    names, irows, erows = tables(ua, sm)
    imports = []
    for i in irows:
        imports.append(Imp(f"{names[i['class_name'][0]]}'{names[i['name'][0]]}'")); imports[-1].outer = i['outer']
    exports = []
    for e in erows:
        nm, num = names[e['name'][0]], e['name'][1]
        if num: nm = f"{nm}_{num-1}"
        exports.append(dict(name=nm, cls=e['cls'], super=e['super'], tmpl=e['tmpl'], outer=e['outer'],
                            flags=e['flags'], size=e['size'], off=e['off']))
    # An uncooked package is one file: payloads sit in the .uasset at absolute offsets, so hand
    # callers the same (blob, base) pair a cooked pair gives them.
    if not ue: ue, total = ua, 0
    else: total = sm['total']
    return ua, ue, total, names, imports, exports


def preload(base):
    """Each export's EDL preload dependencies, as FPackageIndex lists: serialize-before-serialize, create-before-
    serialize, serialize-before-create, create-before-create. A cooked package only (the summary walk of Cooked.cpp)."""
    ua = open(base + ".uasset", "rb").read()
    sm = summary(ua)
    deps = struct.unpack_from('<%di' % max(sm["pcount"], 0), ua, sm["poff"])   # an editor package has -1
    out = []
    for e in tables(ua, sm)[2]:
        lists, at = [], e['first']
        for n in e['counts']:
            lists.append(list(deps[at:at + n]) if e['first'] >= 0 else [])
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
