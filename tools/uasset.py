#!/usr/bin/env python3
"""usage: uasset.py summary <file.uasset>... [--names] [--imports] [--exports] [--ue5=N]
       uasset.py exports <base> <export-index> [count]
       uasset.py struct <base> <export-index>
       uasset.py tags <base> <export-index> [byte-offset]
       uasset.py deps <file.uasset>
       uasset.py registry <AssetRegistry.bin>
       uasset.py walk <base> <export-index>
       uasset.py walk --ue5 <base> [function-name-substring...]

Looks inside a package; <base> is its path without .uasset/.uexp, export indexes count from 0.
  summary   the FPackageFileSummary: version stamps, custom versions, package flags, table offsets; with --names,
            --imports, --exports the tables too. An unversioned UE5 package is read at --ue5=N (default 1017, stock
            5.6, RogueCore), as the engine reads it at its own version.
  exports   the export's class, super and outer, then its payload as 4-byte words, each annotated with the name or
            import it may stand for
  struct    a Function / BlueprintGeneratedClass export's UStruct body: SuperStruct, Children, every ChildProperty with
            its flags and type tail, the bytecode sizes, then the function or class trailer
  tags      the export's tagged properties, decoded. A UserDefinedStruct's default instance starts after its UStruct
            body, so pass that offset to read it
  deps      every export with its index and its EDL preload dependencies
  registry  every asset row of an AssetRegistry.bin, AssetGen's or a cooked game's, with its tags
  walk      a UFunction export's bytecode with the 4.27 grammar, each top-level expression with its disk and memory
            size, the totals compared to the header. --ue5: every script function of a UE 5.6 package (or those whose
            name contains a substring) with the 5.6 operands, each expression at the memory offset jumps name"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from uasset_io import (PACKAGE_FILE_TAG, R, decode_flags, fstring, load, pidx, summary as read_summary,  # noqa: E402
                       tables as read_raw_tables)


def export_blob(base, idx):
    ua, ue, total, names, imports, exports = load(base)
    e = exports[idx]
    return ue[e['off'] - total: e['off'] - total + e['size']], e, names, imports, exports


# ---- summary

def summary(args):
    """The package header dump, one per file argument."""
    want = {a.lstrip('-') for a in args if a.startswith('--')}
    ue5 = [int(a.split('=')[1]) for a in args if a.startswith('--ue5=')]
    for p in [a for a in args if not a.startswith('--')]:
        dump_summary(p, want, ue5[0] if ue5 else 1017)
        print()


def dump_summary(path, want, ue5_version):
    data = open(path, 'rb').read()
    tag = struct.unpack_from('<I', data)[0]
    print(f"== {path}  ({len(data)} bytes)")
    if tag != PACKAGE_FILE_TAG:
        print(f"   not a package (tag {tag:#010x})"); return
    s = read_summary(data, ue5_version)
    print(f"   LegacyFileVersion  {s['legacy']}")
    print(f"   FileVersionUE4     {s['ue4']}      FileVersionLicenseeUE4 {s['licensee']}")
    if s['ue5']:
        print(f"   FileVersionUE5     {s['file_ue5']} in file, read as {s['ue5_version']}")
        print(f"   SoftObjectPaths {s['soft_paths']}  Cells(exp n/off, imp n/off) {s['cells']}  MetaData @ {s['metadata_off']}")
        print(f"   NamesReferencedFromExportData {s['names_from_export']}  PayloadToc @ {s['payload_toc']}  "
              f"DataResource @ {s['data_resource']}")
    print(f"   summary ends at {s['end']}")
    flags = s['flags']
    eng, ceng = s['engine'], s['compatible']
    print(f"   PackageFlags       {flags:#010x}  = {decode_flags(flags)}")
    print(f"   >> UnversionedProperties: {'YES' if flags & 0x2000 else 'NO'}")
    print(f"   EngineVersion      {eng[0]}.{eng[1]}.{eng[2]}-CL{eng[3]} '{eng[4]}'")
    print(f"   CompatibleWith     {ceng[0]}.{ceng[1]}.{ceng[2]}-CL{ceng[3]} '{ceng[4]}'")
    print(f"   CustomVersions     {len(s['customs'])}")
    for g, v in s['customs']: print(f"      {g}  v{v}")
    print(f"   TotalHeaderSize    {s['total']}   FolderName '{s['folder']}'")
    print(f"   Names   {s['ncount']:5} @ {s['noff']}")
    print(f"   Imports {s['icount']:5} @ {s['ioff']}")
    print(f"   Exports {s['ecount']:5} @ {s['eoff']}")
    print(f"   Depends       @ {s['depends_off']}   SoftRefs {s['soft_count']} @ {s['soft_off']}   "
          f"AssetRegistry @ {s['asset_registry_off']}")
    print(f"   PreloadDeps  {s['pcount']:5} @ {s['poff']}   (EDL dependency table)")
    print(f"   BulkDataStart {s['bulk_start']}   PackageSource {s['package_source']:#x}   "
          f"CompressionFlags {s['compression_flags']:#x} chunks {s['chunk_count']}")
    print(f"   Guid {s['guid']}  Generations {s['generations']}")
    if not want: return

    # The tables, each FName with its number and a bad index shown rather than raised:
    names, irows, erows = read_raw_tables(data, s)
    def fname(v):
        n = names[v[0]] if 0 <= v[0] < len(names) else f"<bad:{v[0]}>"
        return f"{n}_{v[1]-1}" if v[1] else n
    imports = [f"{fname(i['class_name'])}'{fname(i['name'])}'" for i in irows]
    exports = [fname(e['name']) for e in erows]
    def pkgindex(v):
        if v == 0: return "null"
        if v < 0:
            i = -v - 1
            return f"import[{i}]:{imports[i] if i < len(imports) else '?'}"
        i = v - 1
        return f"export[{i}]:{exports[i] if i < len(exports) else '?'}"
    if 'imports' in want:
        for k, i in enumerate(irows):
            print(f"   import[{k:3}] {fname(i['class_package'])}.{fname(i['class_name'])} {fname(i['name'])}  outer={i['outer']}")
    if 'names' in want:
        for i, n in enumerate(names): print(f"   name[{i:4}] {n}")
    if 'exports' in want:
        for i, e in enumerate(erows):
            print(f"   export[{i:3}] {exports[i]:40} class={pkgindex(e['cls'])}")
            print(f"              super={pkgindex(e['super'])} template={pkgindex(e['tmpl'])} outer={pkgindex(e['outer'])}")
            print(f"              flags={e['flags']:#010x} size={e['size']} off={e['off']} firstDep={e['first']} "
                  f"deps(serBefSer,createBefSer,serBefCreate,createBefCreate)={list(e['counts'])}")
    print(f"   [tables parsed OK: {len(names)} names, {len(imports)} imports, {len(exports)} exports]")


# ---- exports

def exports(args):
    base, idx = args[0], int(args[1])
    count = int(args[2]) if len(args) > 2 else 1
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


# ---- tags

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
        elif ptype == "ArrayProperty" and extra == " inner=StructProperty":
            # count, then one inner tag naming the struct, then each element's own tags up to its None
            nested = []
            try:
                count = struct.unpack_from('<i', b, o)[0]
                at = name(name(o + 4)[1])[1] + 8
                sname, at = name(at)
                at += 17
                for k in range(count):
                    nested.append("%s  [%d] %s" % (pad, k, sname))
                    at = tags(b, at, o + size, names, indent + 2, nested)
                out.extend(nested)
            except (ValueError, struct.error, IndexError):
                pass
        o += size
    return o


def ue5_package(base):
    """invariants.Package of base if it is a UE5 package (the 5.x tag grammar and RC's unversioned header are read there), else None."""
    import invariants
    pkg = invariants.Package(base)
    return pkg if pkg.ue5 else None


def tags5(pkg, idx, start):
    t = pkg.tags(idx, start)
    print("=== export[%d] %s" % (idx, pkg.exports[idx]['name']))
    if getattr(pkg, 'unversioned_props', False):
        print("  unversioned properties: a %d-byte header, values only the class's schema can place%s"
              % (t.header_end - start, "" if t.end is not None else " (it counts values)"))
        return
    for g in t:
        kinds = [g.get(k) for k in ('struct', 'enum', 'inner', 'value_type') if g.get(k)]
        extra = ('bool=%d' % g['bool']) if 'bool' in g else ' '.join(kinds)
        print("  %s %s%s size=%d %s" % (g['type'], g['name'], '[%d]' % g['index'] if g['index'] else '', g['size'], extra))


def struct5(pkg, idx):
    st = pkg.struct(idx)
    print("=== export[%d] %s  class=%s" % (idx, pkg.exports[idx]['name'], pkg.class_of(idx + 1)))
    if st is None:
        print("not a struct"); return
    print("SuperStruct %s" % pkg.path(st.super))
    print("Children %s" % [pkg.path(c) for c in st.children])
    def show(p, pad):
        print("%s%s %s dim=%d size=%d flags=%#x" % (pad, p.type, p.name, p.dim, p.elem_size, p.flags))
        for sub in p.subs: show(sub, pad + "    ")
    for p in st.props: show(p, "  ")
    print("Script mem=%d disk=%d" % (st.script_mem, st.script_disk))
    for k in ('function_flags', 'class_flags', 'cdo', 'struct_flags'):
        if hasattr(st, k): print("%s %#x" % (k, getattr(st, k)))
    if hasattr(st, 'interfaces'): print("Interfaces %s" % [(pkg.path(i), o) for i, o, _ in st.interfaces])
    print("consumed %d of %d" % (st.end, st.size))


def tags_cmd(args):
    base, idx = args[0], int(args[1])
    start = int(args[2], 0) if len(args) > 2 else 0
    if (pkg := ue5_package(base)): return tags5(pkg, idx, start)
    blob, e, names, _, _ = export_blob(base, idx)
    print("=== export[%d] %s" % (idx, e['name']))
    out = []
    tags(blob, start, len(blob), names, 1, out)
    print("\n".join(out))


# ---- struct

class StructReader(R):
    def __init__(s, b, names, imports, exports):
        R.__init__(s, b)
        s.names, s.imports, s.exports = names, imports, exports
    def idx(s): return pidx(s.i32(), s.imports, s.exports)
    def name(s):
        i, n = s.i32(), s.i32()
        return s.names[i] + ('_%d' % (n - 1) if n else '')


def field(r, ftype, pad, out):
    fname, flags = r.name(), r.u32()
    dim, size, pflags, rep, notify, cond = r.i32(), r.i32(), r.u64(), r.u16(), r.name(), r.u8()
    tail = ''
    if ftype in ('ObjectProperty', 'WeakObjectProperty', 'LazyObjectProperty', 'SoftObjectProperty',
                 'InterfaceProperty', 'StructProperty', 'ByteProperty', 'DelegateProperty',
                 'MulticastInlineDelegateProperty', 'MulticastSparseDelegateProperty'):
        tail = r.idx()
    elif ftype in ('ClassProperty', 'SoftClassProperty'):
        tail = r.idx() + ' meta=' + r.idx()
    elif ftype == 'BoolProperty':
        tail = 'bool ' + r.b[r.o:r.o + 6].hex(); r.o += 6
    out.append('%s%s %s objflags=%#x dim=%d size=%d flags=%#x rep=%d notify=%s cond=%d %s'
               % (pad, ftype, fname, flags, dim, size, pflags, rep, notify, cond, tail))
    subs = {'ArrayProperty': 1, 'SetProperty': 1, 'MapProperty': 2, 'EnumProperty': 1}.get(ftype, 0)
    if ftype == 'EnumProperty':
        out[-1] += r.idx()
    for _ in range(subs):
        field(r, r.name(), pad + '    ', out)


def struct_cmd(args):
    i = int(args[1])
    if (pkg := ue5_package(args[0])): return struct5(pkg, i)
    blob, e, names, imports, exports = export_blob(args[0], i)
    r = StructReader(blob, names, imports, exports)
    out = ['=== export[%d] %s  class=%s' % (i, e['name'], pidx(e['cls'], imports, exports))]
    r.o = tags(blob, 0, len(blob), names, 1, out)
    if r.i32():
        r.o += 16                                   # lazy-object guid
    out.append('SuperStruct %s' % r.idx())
    out.append('Children %s' % [r.idx() for _ in range(r.i32())])
    for _ in range(r.i32()):
        field(r, r.name(), '  ', out)
    mem, disk = r.i32(), r.i32()
    out.append('Script mem=%d disk=%d' % (mem, disk))
    r.o += disk
    cls = pidx(e['cls'], imports, exports)
    if cls.endswith("'Function'") or cls.endswith("'DelegateFunction'"):
        flags = r.u32()
        out.append('FunctionFlags %#x' % flags)
        if flags & 0x40:
            out.append('RepOffset %d' % r.u16())
        out.append('EventGraphFunction %s offset %d' % (r.idx(), r.i32()))
    elif 'Class' in cls:
        out.append('FuncMap %s' % [(r.name(), r.idx()) for _ in range(r.i32())])
        out.append('ClassFlags %#x within=%s config=%s' % (r.u32(), r.idx(), r.name()))
        out.append('ClassGeneratedBy %s' % r.idx())
        out.append('Interfaces %s' % [(r.idx(), r.i32(), r.i32()) for _ in range(r.i32())])
        out.append('bDeprecatedForceScriptOrder %d dummy=%s bCooked %d CDO %s'
                   % (r.i32(), r.name(), r.i32(), r.idx()))
    out.append('consumed %d of %d' % (r.o, len(blob)))
    print('\n'.join(out))


# ---- deps

def read_tables(data):
    """A cooked .uasset's name map, imports as (class name, object name), exports and preload dependencies."""
    sm = read_summary(data)
    names, irows, erows = read_raw_tables(data, sm)

    def name_at(v):
        base = names[v[0]] if 0 <= v[0] < len(names) else "<bad name %d>" % v[0]
        return base if v[1] == 0 else "%s_%d" % (base, v[1] - 1)

    imports = [(name_at(i['class_name']), name_at(i['name'])) for i in irows]
    exports = [{"class": e['cls'], "outer": e['outer'], "name": name_at(e['name']), "is_asset": e['is_asset'] != 0,
                "first": e['first'], "counts": e['counts']} for e in erows]
    deps = list(struct.unpack_from("<%di" % sm['pcount'], data, sm['poff'])) if sm['pcount'] else []
    return names, imports, exports, deps


def deps(args):
    if len(args) < 1:
        sys.exit("usage: uasset.py deps <file.uasset>")
    _names, imports, exports, deps = read_tables(open(args[0], "rb").read())
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
          % (args[0], len(exports), len(imports), total))
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


# ---- registry

BEGIN_MAGIC = 0x12345679
END_MAGIC = 0x87654321
VERSION_GUID = (0x717F9EE7, 0xE9B0493A, 0x88B39132, 0x1B388107)
NUMBERED_BIT = 0x80000000
STORE_VIEWS = 11


class Taker(object):
    """A forward-only reader that stops the tool on a truncated file, as a registry must parse whole."""
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


def read_registry(path):
    """Every asset row of an AssetRegistry.bin, assetgen's or a cooked game's, as dicts; tags are kept as (key, value)
    pairs, bundles and chunks are counted. Exits unless the whole file parses."""
    r = Taker(open(path, "rb").read())
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


def registry(args):
    if len(args) != 1:
        sys.exit("usage: uasset.py registry <AssetRegistry.bin>")
    version, names, rows, dependencies, packages, size = read_registry(args[0])
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


# ---- walk

NAMES = {0:'LocalVariable',1:'InstanceVariable',2:'DefaultVariable',4:'Return',6:'Jump',7:'JumpIfNot',9:'Assert',
 0xB:'Nothing',0xF:'Let',0x12:'ClassContext',0x13:'MetaCast',0x14:'LetBool',0x15:'EndParmValue',0x16:'EndFunctionParms',
 0x17:'Self',0x18:'Skip',0x19:'Context',0x1A:'Context_FailSilent',0x1B:'VirtualFunction',0x1C:'FinalFunction',0x1D:'IntConst',
 0x1E:'FloatConst',0x1F:'StringConst',0x20:'ObjectConst',0x21:'NameConst',0x22:'RotationConst',0x23:'VectorConst',0x24:'ByteConst',
 0x25:'IntZero',0x26:'IntOne',0x27:'True',0x28:'False',0x29:'TextConst',0x2A:'NoObject',0x2B:'TransformConst',0x2C:'IntConstByte',
 0x2D:'NoInterface',0x2E:'DynamicCast',0x2F:'StructConst',0x30:'EndStructConst',0x31:'SetArray',0x32:'EndArray',0x33:'PropertyConst',
 0x34:'UnicodeStringConst',0x35:'Int64Const',0x36:'UInt64Const',0x38:'PrimitiveCast',0x39:'SetSet',0x3A:'EndSet',0x3B:'SetMap',
 0x3C:'EndMap',0x3D:'SetConst',0x3E:'EndSetConst',0x3F:'MapConst',0x40:'EndMapConst',0x42:'StructMemberContext',
 0x43:'LetMulticastDelegate',0x44:'LetDelegate',0x45:'LocalVirtualFunction',0x46:'LocalFinalFunction',0x48:'LocalOutVariable',
 0x4B:'InstanceDelegate',0x4C:'PushExecutionFlow',0x4D:'PopExecutionFlow',0x4E:'ComputedJump',0x4F:'PopExecutionFlowIfNot',
 0x50:'Breakpoint',0x51:'InterfaceContext',0x52:'ObjToInterfaceCast',0x53:'EndOfScript',0x54:'CrossInterfaceCast',
 0x55:'InterfaceToObjCast',0x5A:'WireTracepoint',0x5B:'SkipOffsetConst',0x5C:'AddMulticastDelegate',0x5D:'ClearMulticastDelegate',
 0x5E:'Tracepoint',0x5F:'LetObj',0x60:'LetWeakObjPtr',0x61:'BindDelegate',0x62:'RemoveMulticastDelegate',0x63:'CallMulticastDelegate',
 0x64:'LetValueOnPersistentFrame',0x65:'ArrayConst',0x66:'EndArrayConst',0x67:'SoftObjectConst',0x68:'CallMath',0x69:'SwitchValue',
 0x6A:'InstrumentationEvent',0x6B:'ArrayGetByRef',0x6C:'ClassSparseDataVariable',0x6D:'FieldPathConst'}


class W:
    """The 4.27 bytecode grammar (ScriptSerialization.h), walking a script and logging each expression with its disk
    and memory size. runscript.py and invariants.py subclass it to build trees instead."""
    def __init__(s, b, o, names, imports, exports):
        s.b, s.o, s.mem, s.names, s.imports, s.exports = b, o, 0, names, imports, exports
        s.depth = 0; s.log = []
    def u8(s):  v = s.b[s.o]; s.o += 1; s.mem += 1; return v
    def u16(s): v = struct.unpack_from('<H', s.b, s.o)[0]; s.o += 2; s.mem += 2; return v
    def i32(s): v = struct.unpack_from('<i', s.b, s.o)[0]; s.o += 4; s.mem += 4; return v
    def raw(s, n): s.o += n; s.mem += n
    def ptr(s):
        v = struct.unpack_from('<i', s.b, s.o)[0]; s.o += 4; s.mem += 8
        return pidx(v, s.imports, s.exports)
    def name(s):
        i = struct.unpack_from('<i', s.b, s.o)[0]; n = struct.unpack_from('<i', s.b, s.o + 4)[0]
        s.o += 8; s.mem += 12
        return s.names[i] + (('_%d' % (n - 1)) if n else '')
    def fieldpath(s):
        cnt = struct.unpack_from('<i', s.b, s.o)[0]; s.o += 4
        segs = []
        for _ in range(cnt):
            i, n = struct.unpack_from('<ii', s.b, s.o); segs.append(s.names[i] + (('_%d' % (n - 1)) if n else '')); s.o += 8
        owner = struct.unpack_from('<i', s.b, s.o)[0]; s.o += 4
        s.mem += 8
        return '.'.join(segs) + '@' + pidx(owner, s.imports, s.exports)
    def cstr(s):
        e = s.b.index(b'\0', s.o); n = e - s.o + 1; t = s.b[s.o:e].decode('latin-1'); s.raw(n); return t
    def wstr(s):
        st = s.o
        while s.b[s.o] or s.b[s.o+1]: s.o += 2
        s.o += 2; s.mem += s.o - st; return '<wide>'
    def parms(s):
        while True:
            if s.expr() == 0x16: break
    def expr(s):
        start_o, start_m = s.o, s.mem
        op = s.u8()
        nm = NAMES.get(op, '?%02x' % op)
        info = ''
        s.depth += 1
        if op in (0, 1, 2, 0x48, 0x6C, 0x33): info = s.fieldpath()
        elif op in (4, 0x51, 0x4E, 0x4F, 0x67, 0x6D): s.expr()
        elif op == 6: info = 'to %d' % s.i32()
        elif op == 7: info = 'to %d' % s.i32(); s.expr()
        elif op == 9: s.u16(); s.u8(); s.expr()   # EX_Assert; 0x08 is unassigned in 4.27 (Script.h)
        elif op in (0xB, 0x15, 0x16, 0x17, 0x25, 0x26, 0x27, 0x28, 0x2A, 0x2D, 0x30, 0x32, 0x3A, 0x3C, 0x3E, 0x40, 0x4D, 0x50, 0x53, 0x5A, 0x5E, 0x66): pass
        elif op == 0xF: info = s.fieldpath(); s.expr(); s.expr()
        elif op in (0x14, 0x43, 0x44, 0x5F, 0x60): s.expr(); s.expr()
        elif op in (0x13, 0x2E, 0x52, 0x54, 0x55): info = s.ptr(); s.expr()
        elif op == 0x18: info = 'skip %d' % s.i32(); s.expr()
        elif op in (0x19, 0x1A, 0x12): s.expr(); info = 'skip %d ' % s.i32(); info += s.fieldpath(); s.expr()
        elif op in (0x1B, 0x45): info = s.name(); s.parms()
        elif op in (0x1C, 0x46, 0x68): info = s.ptr(); s.parms()
        elif op == 0x63: info = s.ptr(); s.expr(); s.parms()
        elif op == 0x1D: info = str(s.i32())
        elif op == 0x1E: s.raw(4)
        elif op == 0x1F: info = repr(s.cstr())
        elif op == 0x34: info = s.wstr()
        elif op == 0x20: info = s.ptr()
        elif op == 0x21: info = s.name()
        elif op in (0x22, 0x23): s.raw(12)
        elif op == 0x2B: s.raw(40)
        elif op in (0x24, 0x2C): info = str(s.u8())
        elif op == 0x29:
            t = s.u8(); info = 'type %d' % t
            if t == 1: s.expr(); s.expr(); s.expr()
            elif t in (2, 3): s.expr()
            elif t == 4: s.ptr(); s.expr(); s.expr()
        elif op == 0x2F: info = s.ptr() + ' size %d' % s.i32();
        elif op == 0x31: s.expr();
        elif op in (0x35, 0x36): s.raw(8)
        elif op == 0x38: info = 'cast %d' % s.u8(); s.expr()
        elif op in (0x39, 0x3B): s.expr(); s.i32()
        elif op == 0x3D: info = s.fieldpath(); s.i32()
        elif op == 0x3F: info = s.fieldpath() + ',' + s.fieldpath(); s.i32()
        elif op == 0x65: info = s.fieldpath(); s.i32()
        elif op == 0x42: info = s.fieldpath(); s.expr()
        elif op == 0x4B: info = s.name()
        elif op == 0x4C: info = 'skip %d' % s.i32()
        elif op == 0x5B: s.i32()
        elif op in (0x5C, 0x62): s.expr(); s.expr()
        elif op == 0x5D: s.expr()
        elif op == 0x61: info = s.name(); s.expr(); s.expr()
        elif op == 0x64: info = s.fieldpath(); s.expr()
        elif op == 0x69:
            n = s.u16(); s.i32(); s.expr()   # count, end skip, index (ScriptSerialization.h)
            for _ in range(n): s.expr(); s.i32(); s.expr()
            s.expr()
        elif op == 0x6A:   # the loader transfers no operand byte, only moves its memory index (ScriptSerialization.h 275-281)
            s.mem += 1
        elif op == 0x6B: s.expr(); s.expr()
        else: raise ValueError('unknown op %02x at %d' % (op, start_o))   # an Exception: a sweep reports it and goes on
        # the run-until-terminator forms
        if op in (0x2F, 0x31, 0x39, 0x3B, 0x3D, 0x3F, 0x65):
            term = {0x2F:0x30, 0x31:0x32, 0x39:0x3A, 0x3B:0x3C, 0x3D:0x3E, 0x3F:0x40, 0x65:0x66}[op]
            while s.expr() != term: pass
        s.depth -= 1
        s.log.append((start_o, start_m, s.o - start_o, s.mem - start_m, '  ' * s.depth + nm, info))
        return op


NAMES5 = dict(NAMES)
NAMES5.update({0x37: 'DoubleConst', 0x41: 'Vector3fConst', 0x0C: 'NothingInt32', 0x11: 'BitFieldConst'})


class W5(W):
    """W with the 5.6 operands: LWC doubles in vector, rotator and transform consts, EX_DoubleConst 0x37,
    EX_Vector3fConst 0x41, EX_NothingInt32 0x0C, EX_BitFieldConst 0x11."""
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
        s.log.append((start_o, start_m, s.o - start_o, s.mem - start_m, '  ' * s.depth + NAMES5[op], info))
        return op


def walk(args):
    if args[:1] == ['--ue5']:
        return walk5(args[1:])
    base, idx = args[0], int(args[1])
    blob, e, names, imports, exports = export_blob(base, idx)
    p = None
    for trailer, cand in ((t, c) for t in (12, 14) for c in range(0, len(blob) - t - 4)):   # 14: FUNC_Net adds RepOffset
        script_end = len(blob) - trailer
        if struct.unpack_from('<i', blob, cand)[0] == script_end - (cand + 4) and struct.unpack_from('<i', blob, cand - 4)[0] > 0:
            try:
                t = W(blob, cand + 4, names, imports, exports)
                while t.o < script_end: t.expr()
                if t.o == script_end: p = cand; break
            except (Exception, SystemExit): pass     # a wrong candidate hits an unknown op
    assert p is not None, 'no ScriptStorageSize found'
    bytecode, storage = struct.unpack_from('<i', blob, p - 4)[0], struct.unpack_from('<i', blob, p)[0]
    print('%s: header ScriptBytecodeSize=%d ScriptStorageSize=%d, script at +%d' % (e['name'], bytecode, storage, p + 4))
    w = W(blob, p + 4, names, imports, exports)
    while w.o < script_end:
        w.expr()
        if w.mem >= bytecode and w.o < script_end:
            print('  ** loader would STOP here: mem %d >= %d with %d disk bytes unread' % (w.mem, bytecode, script_end - w.o))
    w.log.sort()
    for o, m, dsz, msz, nm, info in w.log:
        print('  +%4d mem %4d  disk %3d mem %3d  %-22s %s' % (o, m, dsz, msz, nm, info))
    print('walked: disk %d (header %d)  mem %d (header %d)' % (w.o - (p + 4), storage, w.mem, bytecode))


def walk5(args):
    base, wanted = args[0], args[1:]
    ua, ue, total, names, imports, exports = load(base)
    for e in exports:
        if 'Function' not in pidx(e['cls'], imports, exports): continue
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


COMMANDS = {'summary': summary, 'exports': exports, 'struct': struct_cmd, 'tags': tags_cmd, 'deps': deps,
            'registry': registry, 'walk': walk}


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv or argv[0] not in COMMANDS:
        sys.exit(__doc__)
    COMMANDS[argv[0]](argv[1:])


if __name__ == "__main__":
    main()
