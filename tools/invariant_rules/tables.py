import sys
from invariants import *
"""TABLES: the rules the package's own tables must keep - the summary's name, import and export tables, the preload
dependency list, each export's archetype - and the few payload facts that are read against those tables (every FName
reference, each export's SerialSize, the SuperIndex a struct body repeats). Each rule cites the UE 4.27 source that
makes it one. The header is re-read raw here (indices unformatted, negative ones kept) because Package._name formats
and a negative index would wrap in Python."""
import collections, copy
import invariants

RF_Public, RF_ClassDefaultObject, RF_Transient = 0x1, 0x10, 0x40
RF_InheritableComponentTemplate = 0x400000
RF_Load = 0x2D4003B            # ObjectMacros.h:524, RF_Public | RF_Standalone | RF_Transactional | ... | RF_NonPIEDuplicateTransient
CLASS_Abstract, CLASS_NewerVersionExists = 0x1, 0x80000000           # ObjectMacros.h:172, 242
NAME_SIZE = 1024                                                     # NameTypes.h:36
CONTENT_ROOTS = []             # extra Content folders imports_resolve looks in, after the package's own and its siblings'


# ---- the header, raw

class Raw:
    """A package's header tables with every index as the loader reads it: names (decoded, and raw with their length
    field), imports and exports (FPackageIndex and (NameIndex, Number) pairs unformatted), the preload dependencies."""
    def name(s, pair):
        i, n = pair
        base = s.names[i] if 0 <= i < len(s.names) else '<bad name %d>' % i
        return base + ('_%d' % (n - 1) if n else '')

    def key(s, pair):
        """FName identity: the entry case-folded (FName compares case-insensitively) and the Number exactly."""
        i, n = pair
        return (s.names[i].lower() if 0 <= i < len(s.names) else None), n

    def row(s, idx):
        if idx < 0 and -idx - 1 < len(s.imports): return s.imports[-idx - 1]
        if idx > 0 and idx - 1 < len(s.exports): return s.exports[idx - 1]
        return None

    def root(s, idx):
        """The outermost import of an import's chain (an FPackageIndex), or None for a broken chain."""
        seen = set()
        while idx < 0 and idx not in seen:
            seen.add(idx)
            r = s.row(idx)
            if r is None: return None
            if r['outer'] == 0: return idx
            idx = r['outer']
        return None

    def path(s, idx):
        r = s.row(idx)
        if r is None: return None
        me = s.name(r['obj'])
        if r['outer'] == 0: return me if idx < 0 else s.package + '.' + me
        up = s.path(r['outer'])
        return None if up is None else up + (':' if '.' in up else '.') + me


def parse_tables(ua, base):
    """Raw over the .uasset bytes `ua` (FPackageFileSummary, PackageFileSummary.cpp; FObjectImport / FObjectExport,
    ObjectResource.cpp), the summary walked as dumpexp.preload walks it."""
    t, r = Raw(), Rd(ua, 4)

    def skip(size):                                             # a counted run of rows. Not r.o += size * r.i32():
        n = r.i32(); r.o += size * n                            # that reads r.o before the count moves it
    if r.i32() != -4: r.i32()
    r.i32(); r.i32()
    skip(20)                                                    # custom versions
    r.i32(); r.fstr(); t.flags_at = r.o; t.flags = r.u32()
    t.name_count, t.name_offset = r.i32(), r.i32()
    t.cooked = bool(t.flags & 0x80000000)
    if not t.cooked: r.fstr()
    r.o += 8                                                    # gatherable text
    t.export_count, t.export_offset = r.i32(), r.i32()
    t.import_count, t.import_offset = r.i32(), r.i32()
    r.o += 4 + 8 + 4 + 4 + 16                                   # depends, soft package refs, searchable names, thumbnails, guid
    skip(8)                                                     # generations
    for _ in range(2): r.o += 10; r.fstr()                      # saved-by and compatible-with engine versions
    r.u32(); skip(16)                                           # compression flags, compressed chunks
    r.u32()                                                     # package source
    for _ in range(r.i32()): r.fstr()                           # additional packages to cook
    r.i32(); r.i64(); r.i32()                                   # asset registry data, bulk data start, world tile info
    skip(4)                                                     # chunk ids
    t.preload_count, t.preload_offset = r.i32(), r.i32()
    t.names, t.names_raw, t.names_at = [], [], []            # names_at: each entry's length field
    r = Rd(ua, t.name_offset)
    for _ in range(t.name_count):
        t.names_at.append(r.o); n = r.i32()
        size = n if n >= 0 else -2 * n
        raw = ua[r.o:r.o + size]
        t.names_raw.append((n, raw))
        t.names.append(raw[:-1].decode('latin-1') if n > 0 else raw[:-2].decode('utf-16-le', 'replace') if n < 0 else '')
        r.o += size + 4                                         # the two unused hashes
        if size > 4 * NAME_SIZE or r.o > len(ua): break         # a wild length: table_bounds reports it
    stride = 28 if t.cooked else 36
    t.imports = []
    for k in range(t.import_count):
        cp, cpn, cn, cnn, outer, obj, objn = struct.unpack_from('<7i', ua, t.import_offset + stride * k)
        t.imports.append(dict(cp=(cp, cpn), cn=(cn, cnn), outer=outer, obj=(obj, objn)))
    t.exports = []
    for k in range(t.export_count):
        v = struct.unpack_from('<4i2iIqq3i16sI2i5i', ua, t.export_offset + 104 * k)
        t.exports.append(dict(cls=v[0], super=v[1], tmpl=v[2], outer=v[3], obj=(v[4], v[5]), flags=v[6], size=v[7],
                              off=v[8], forced=v[9], package_flags=v[13], not_always_loaded=v[14], is_asset=v[15],
                              first_dep=v[16], dep_counts=v[17:21]))
    ok = 0 <= t.preload_offset and t.preload_offset + 4 * max(t.preload_count, 0) <= len(ua)
    t.preload = list(struct.unpack_from('<%di' % t.preload_count, ua, t.preload_offset)) if ok and t.preload_count > 0 else []
    t.package = '/Game/' + base.replace(os.sep, '/').split('/Content/')[-1]
    return t


def raw_tables(pkg):
    t = pkg.__dict__.get('_tables_raw')
    if t is None: t = pkg._tables_raw = parse_tables(pkg.ua, pkg.base)
    return t


_HEADERS = {}


def header_at(base):
    """Raw of another package on disk; only its header is kept, the last 4096 read (a sweep of the whole game reaches
    tens of thousands)."""
    key = os.path.normcase(os.path.abspath(base))
    t = _HEADERS.pop(key, None)
    if t is None:
        with open(base + '.uasset', 'rb') as f: t = parse_tables(f.read(), base)
        if len(_HEADERS) >= 4096: del _HEADERS[next(iter(_HEADERS))]
    _HEADERS[key] = t
    return t


_SIBLINGS = {}


def content_roots(pkg):
    """Where a /Game import of pkg is looked for: its own Content folder; the Content folders of the mods built beside
    it (a suite build is <root>/<Mod>/FSD/Content each); CONTENT_ROOTS; the game's (GAME_CONTENT, --game)."""
    here = pkg.base.replace(os.sep, '/')
    if '/Content/' not in here: return CONTENT_ROOTS + GAME_CONTENT
    own = here[:here.rindex('/Content/') + len('/Content')]
    if own not in _SIBLINGS:
        parts = own.split('/')
        sib = []
        if parts[-2:] == ['FSD', 'Content'] and len(parts) > 3:
            sib = sorted(p.replace(os.sep, '/') for p in glob.glob(os.path.join('/'.join(parts[:-3]), '*', 'FSD', 'Content')))
        _SIBLINGS[own] = [own] + [p for p in sib if os.path.normcase(p) != os.path.normcase(own)]
    return _SIBLINGS[own] + CONTENT_ROOTS + GAME_CONTENT


def find_package(pkg, name):
    """The base path of the /Game package `name` as pkg's loader would find it, or None."""
    for root in content_roots(pkg):
        p = root.rstrip('/') + name[len('/Game'):]
        if os.path.exists(p + '.uasset'): return p
    return None


# ---- every FName reference in the package

class _SafeNames(list):
    """The name map, answering an out-of-range index (a negative one too) with a marker instead of wrapping or raising."""
    def __getitem__(s, i):
        if isinstance(i, int) and not 0 <= i < len(s): return '<bad name %d>' % i
        return list.__getitem__(s, i)


class _NameWalk(Walk):
    """invariants.Walk, handing each raw (NameIndex, Number) it reads - a name operand, a FieldPath segment - to sink."""
    def __init__(s, b, o, pkg, sink):
        Walk.__init__(s, b, o, pkg)
        s.names, s.sink = pkg.names, sink
    def name(s):
        s.sink(*struct.unpack_from('<ii', s.b, s.o))
        return Walk.name(s)
    def fieldpath(s):
        cnt = struct.unpack_from('<i', s.b, s.o)[0]
        for k in range(max(cnt, 0)): s.sink(*struct.unpack_from('<ii', s.b, s.o + 4 + 8 * k))
        return Walk.fieldpath(s)


# Classes whose Serialize writes no tagged properties in a cooked package (URigVM::Serialize goes straight to its own
# Save/Load): their payload is not read for names. Calibrated: the only such exports in the game's 51,579 packages.
UNTAGGED = {'RigVM'}


def name_refs(pkg):
    """Every FName reference the loader resolves against the name map, as (export index or None, where, NameIndex,
    Number): the import and export tables; each export's tags (names, types, struct / enum / inner names, NameProperty
    and TArray<FName> values); each struct body (property and RepNotify names, FuncMap keys, config and sparse-delegate
    names, a UserDefinedStruct's default tags); each script's name operands and FieldPath segments. A payload this
    reader cannot decode stops that export's collection (struct_body_exact reports a struct that does not read); an
    export of an UNTAGGED class has no tags to read."""
    refs = pkg.__dict__.get('_tables_refs')
    if refs is not None: return refs
    t, refs = raw_tables(pkg), []
    for j, m in enumerate(t.imports):
        for f in ('cp', 'cn', 'obj'): refs.append((None, 'import %d %s' % (j, f), *m[f]))
    for k, e in enumerate(t.exports): refs.append((k, 'export name', *e['obj']))
    rec = copy.copy(pkg)
    rec._tags, rec._structs, rec.names = {}, {}, _SafeNames(pkg.names)
    where = [None, '']
    sink = lambda i, n: refs.append((where[0], where[1], i, n))

    def _name(r):
        i, n = r.i32(), r.i32()
        sink(i, n)
        return rec.names[i] + ('_%d' % (n - 1) if n else '')
    rec._name = _name
    for k in range(len(pkg.exports)):
        where[0] = k
        if pkg.class_of(k + 1) in UNTAGGED: continue
        try:
            where[1] = 'tags'
            for tg in rec.tags(k):
                v = tg['value']
                if tg['type'] == 'NameProperty' and len(v) == 8: sink(*struct.unpack('<ii', v))
                elif tg['type'] == 'ArrayProperty' and tg.get('inner') == 'NameProperty' and len(v) >= 4:
                    cnt = struct.unpack_from('<i', v)[0]
                    if 4 + 8 * cnt == len(v):
                        for c in range(cnt): sink(*struct.unpack_from('<ii', v, 4 + 8 * c))
            if not rec.is_struct(k): continue
            where[1] = 'struct'
            st = rec.struct(k)
            if st.script_disk:
                where[1] = 'script'
                w = _NameWalk(rec.blob(k), st.script_at, rec, sink)
                while w.o < st.script_at + st.script_disk: w.expr()
        except (Exception, SystemExit):
            continue
    pkg._tables_refs = refs
    return refs


def engine_splits(s):
    """Whether FName(s) splits a number off s: UnrealNames.cpp ParseNumber (1928-1956) - a trailing run of 1..10 ASCII
    digits after a '_' with something before it, no leading zero unless it is the only digit, below MAX_int32."""
    d = len(s) - len(s.rstrip('0123456789'))
    return (1 <= d <= 10 and d < len(s) and s[-d - 1] == '_' and (d == 1 or s[-d] != '0')
            and int(s[-d:]) < 2**31 - 1)


# ---- the rules

@rule
def table_bounds(pkg):
    """Every FPackageIndex in the header - export ClassIndex / SuperIndex / TemplateIndex / OuterIndex, import
    OuterIndex, each preload dependency - is 0 or names a row that exists: the EDL indexes Imp()/Exp() with it,
    unchecked in Shipping (LinkerLoad.cpp 5397-5421, Linker.h 110-114). An export's preload run (FirstExportDependency
    and its four counts) lies inside PreloadDependencies, whose entries are never null (AsyncLoading.cpp 2447-2499, a
    check(!Dep.IsNull()) per entry). Every FName reference - header, tags, property and FuncMap names, script operands -
    has 0 <= NameIndex < NameCount: a bad index is 'Bad name index', the name becomes None and the linker takes a
    critical error (LinkerLoad.h 862-875, LinkerLoad.cpp 5460-5463). Its Number is >= 0: not an engine check, but no
    text makes a negative one (ParseNumber stops below MAX_int32, UnrealNames.cpp 1928-1956) and the game has none.
    Every name-map entry is 1..NAME_SIZE units NUL included, the NUL last and nowhere before: a longer one is 'String is
    too long' and every later entry is misread (UnrealNames.cpp 2657-2672); the name is read up to the first NUL from
    one buffer reused for every entry (UnrealNames.cpp 2051-2058, LinkerLoad.cpp 1540-1545), so a missing or early NUL,
    or an empty entry, gives a wrong name."""
    t = raw_tables(pkg)
    ok = lambda v: -t.import_count <= v <= t.export_count
    for k, e in enumerate(t.exports):
        for f in ('cls', 'super', 'tmpl', 'outer'):
            if not ok(e[f]): yield k, '%s %d is outside the %d imports / %d exports' % (f, e[f], t.import_count, t.export_count)
        first, counts = e['first_dep'], e['dep_counts']
        if first == -1:
            if any(counts): yield k, 'no preload run (FirstExportDependency -1) but counts %s' % (counts,)
        elif first < 0 or min(counts) < 0 or first + sum(counts) > t.preload_count:
            yield k, 'preload run %d + %s outside the %d dependencies' % (first, counts, t.preload_count)
    for j, m in enumerate(t.imports):
        if not ok(m['outer']): yield None, 'import %d outer %d is outside the tables' % (j, m['outer'])
    if t.preload_count and len(t.preload) != t.preload_count:
        yield None, 'PreloadDependencyCount %d at %d lies outside the file' % (t.preload_count, t.preload_offset)
    for j, d in enumerate(t.preload):
        if d == 0 or not ok(d): yield None, 'preload dependency %d is %d' % (j, d)
    for k, where, i, n in name_refs(pkg):
        if not 0 <= i < t.name_count or n < 0:
            yield k, '%s: FName (%d, %d) with %d names' % (where, i, n, t.name_count)
    if len(t.names_raw) != t.name_count: yield None, 'name map stops after %d of %d entries' % (len(t.names_raw), t.name_count)
    for j, (n, raw) in enumerate(t.names_raw):
        w = 1 if n >= 0 else 2
        units = [raw[x:x + w] for x in range(0, len(raw), w)]
        if not n: yield None, 'name %d is empty: the reused buffer keeps the previous entry' % j
        elif abs(n) > NAME_SIZE: yield None, 'name %d is %d units long, over NAME_SIZE %d' % (j, abs(n), NAME_SIZE)
        elif len(units) != abs(n) or units[-1] != b'\0' * w or b'\0' * w in units[:-1]:
            yield None, 'name %d %r does not end in its only NUL' % (j, t.names[j][:40])


@rule
def names_split(pkg):
    """A name reference with Number 0 names an entry FName would not split, and one with a Number names a non-empty
    entry: the loader makes each reference FName(NameMap[Index], Number) from the entry taken whole
    (NAME_NO_NUMBER_INTERNAL, UnrealNames.cpp 2051-2058; LinkerLoad.h 854-877), while FName("Tag_7") made at run time is
    ('Tag', 8) (ParseNumber, UnrealNames.cpp 1928-1956). ('Tag_7', 0) is a different FName, so every match by name
    against the engine's misses - an import, a property, a function looked up by name. An entry may itself end in
    _digits when every reference to it carries a Number (the split base of a doubly-suffixed name, 'SM_Cylinder_1_1' #2)."""
    names = raw_tables(pkg).names
    seen = set()
    for k, where, i, n in name_refs(pkg):
        if not 0 <= i < len(names) or (k, where, i, n) in seen: continue
        seen.add((k, where, i, n))
        if n == 0 and engine_splits(names[i]):
            yield k, '%s: %r #0, which FName(%r) splits' % (where, names[i], names[i])
        elif n > 0 and names[i] == '':
            yield k, '%s: an empty entry with Number %d' % (where, n)


def _import_chain(t, j):
    """[root, ..., j] (import indices) of import j, or a string saying why the chain is broken."""
    chain, seen, idx = [], set(), -j - 1
    while True:
        if idx in seen: return 'its outer chain loops'
        seen.add(idx)
        if idx > 0: return 'its chain reaches export %d: a cooked import never has an export outer' % (idx - 1)
        r = t.row(idx)
        if r is None: return 'its chain reaches index %d, outside the imports' % idx
        chain.append(-idx - 1)
        if r['outer'] == 0: return chain[::-1]
        idx = r['outer']


@rule
def import_chains(pkg):
    """Each import's outer chain runs through imports only, without a loop, to an import with OuterIndex 0
    (AsyncLoading.cpp 1728-1743, 2002-2016: check(OuterMostIndex.IsImport()), a loop spins forever in Shipping). An
    import with OuterIndex 0 is a package: ClassName Package (AsyncLoading.cpp 1747-1752, check(0) and the import
    skipped otherwise) and a long package name (1768-1790, check(!IsShortPackageName) before it is requested; a malformed
    one - no leading '/', an empty segment, a trailing '/' - maps to no file, 5737-5797). Its ClassPackage
    /Script/CoreUObject is not checked by the loader: it is what the cook writes (UPackage's own package) and the game
    keeps. None is the package being loaded - its own objects are exports (AsyncLoading.cpp 1986-1989, 2075-2078
    check(ImportLinker->AsyncRoot != this); Shipping compiles the check out and resolves the import back onto the
    package's own export)."""
    t = raw_tables(pkg)
    own = pkg.package_name().lower()
    for j, m in enumerate(t.imports):
        chain = _import_chain(t, j)
        me = t.path(-j - 1) or 'import %d' % j
        if isinstance(chain, str): yield None, 'import %s: %s' % (me, chain); continue
        if m['outer']: continue
        cp, cn, name = t.name(m['cp']), t.name(m['cn']), t.name(m['obj'])
        if (cp.lower(), cn.lower()) != ('/script/coreuobject', 'package'):          # FName compares case-insensitively
            yield None, 'import %s has no outer but is a %s.%s, not a package' % (name, cp, cn)
        if not name.startswith('/') or name.endswith('/') or '//' in name:
            yield None, 'root import %r is not a long package name' % name
        if name.lower() == own: yield None, 'import %s names this package itself' % name


@rule
def import_unique(pkg):
    """No two imports name one object: the cook builds the import map from the objects it marked for import, one row
    each (SavePackage.cpp 3288-3339), and an object is its outer chain of FNames, each compared case-insensitively with
    its Number exact. Two rows for one object load as one, but each is a dependency of its own, which the cook never
    writes; the game's 51,579 packages have no such pair."""
    t = raw_tables(pkg)
    seen = {}
    for j in range(len(t.imports)):
        chain = _import_chain(t, j)
        if isinstance(chain, str): continue                             # import_chains
        key = tuple(t.key(t.imports[step]['obj']) for step in chain)
        if key in seen:
            yield None, 'imports %s and %s are one object' % (t.path(-seen[key] - 1), t.path(-j - 1))
        else:
            seen[key] = j


@rule
def imports_resolve(pkg):
    """An import of an object in another /Game package names an export there: the EDL maps the import's outer chain
    onto the target's exports (the root package is 0) and looks up (ObjectName, outer) in ObjectNameWithOuterToExport,
    case-insensitively on the name and exactly on its Number (AsyncLoading.cpp 1917-1933, 2075-2129; LinkerLoad.cpp
    2396-2403). A miss is 'Could not find import' and the import loads as null - as a ClassIndex the export fails, as a
    SuperIndex it is Fatal. ClassPackage.ClassName is that export's own class: an object found in memory is checked
    against its class's name, BlueprintGeneratedClass-for-DynamicClass and Function-for-DelegateFunction excepted
    (AsyncLoading.cpp 1637-1669), and the editor's VerifyImport needs both equal (LinkerLoad.cpp 3307-3355). The package
    itself must exist (AsyncLoading.cpp 5737-5797 'Couldn't find file'): that is checked when GAME_CONTENT is set, so
    an import of the game's content is not taken as missing where the game is not at hand. /Script imports are the
    compiler's UeApi's business and bpcheck's in game."""
    t = raw_tables(pkg)
    exempt = {('blueprintgeneratedclass', 'dynamicclass'), ('function', 'delegatefunction')}
    for j, m in enumerate(t.imports):
        chain = _import_chain(t, j)
        if isinstance(chain, str): continue                             # import_chains
        top = t.name(t.imports[chain[0]]['obj'])
        if not top.startswith('/Game/'): continue
        base = find_package(pkg, top)
        me = t.path(-j - 1)
        if base is None:                                                # said once, at the package's own import
            if GAME_CONTENT and len(chain) == 1: yield None, 'import %s: no such package' % me
            continue
        if len(chain) < 2: continue
        other = header_at(base)
        exports = _by_key(other)
        outer = 0
        for step in chain[1:]:
            k = exports.get((t.key(t.imports[step]['obj']), outer))
            if k is None:
                yield None, 'import %s: %s has no export %s under %s' % (me, top, t.name(t.imports[step]['obj']),
                                                                          other.path(outer) if outer else top)
                break
            outer = k + 1
        else:
            e = other.exports[outer - 1]
            if e['cls'] > 0: want = (other.package, other.name(other.exports[e['cls'] - 1]['obj']))
            elif e['cls'] < 0:
                croot = other.root(e['cls'])
                want = (other.name(other.row(croot)['obj']) if croot else '?', other.name(other.row(e['cls'])['obj']))
            else: want = ('/Script/CoreUObject', 'Class')
            got = (t.name(m['cp']), t.name(m['cn']))
            if got[1].lower() != want[1].lower() and (got[1].lower(), want[1].lower()) not in exempt:
                yield None, 'import %s is a %s.%s, the export is a %s.%s' % (me, got[0], got[1], want[0], want[1])
            elif got[0].lower() != want[0].lower():
                yield None, 'import %s: class package %s, the export\'s class is in %s' % (me, got[0], want[0])


def _by_key(other):
    """{(FName key, OuterIndex): export index} of Raw `other`: its ObjectNameWithOuterToExport (LinkerLoad.cpp 2396-2403)."""
    exports = other.__dict__.get('by_key')
    if exports is None:
        exports = other.by_key = {}
        for k, e in enumerate(other.exports): exports[other.key(e['obj']), e['outer']] = k
    return exports


def _export_of_import(pkg, t, idx):
    """(Raw of the /Game package import `idx` of pkg lives in, its export index there), mapped the way
    FindExportFromImport maps it; None for a /Script import or one whose package or export is not at hand."""
    chain = _import_chain(t, -idx - 1)
    if isinstance(chain, str) or len(chain) < 2: return None
    top = t.name(t.imports[chain[0]]['obj'])
    base = find_package(pkg, top) if top.startswith('/Game/') else None
    if base is None: return None
    other = header_at(base)
    exports, outer = _by_key(other), 0
    for step in chain[1:]:
        k = exports.get((t.key(t.imports[step]['obj']), outer))
        if k is None: return None
        outer = k + 1
    return other, outer - 1


def _raw_class_path(t, k):
    """The class of export k of Raw t, as a lower-case path."""
    cls = t.exports[k]['cls']
    if cls > 0: return (t.package + '.' + t.name(t.exports[cls - 1]['obj'])).lower()
    return (t.path(cls) or '/script/coreuobject.class').lower()


def _top_export(other, key):
    """The top-level export of Raw `other` whose FName is `key`, or None."""
    return next((k for k, e in enumerate(other.exports) if e['outer'] == 0 and other.key(e['obj']) == key), None)


_CLASS_INFO = {}


def _class_info(pkg, cls):
    """(ClassFlags, ClassWithin as a path) of class `cls` when this package, or the /Game package its import names,
    holds it; else None."""
    if cls > 0:
        st = pkg.struct(cls - 1)
        return (st.class_flags, pkg.path(st.within)) if hasattr(st, 'class_flags') else None
    t = raw_tables(pkg)
    root = t.root(cls)
    top = t.name(t.row(root)['obj']) if root is not None else ''
    base = find_package(pkg, top) if top.startswith('/Game/') else None
    if base is None: return None
    key = (os.path.normcase(os.path.abspath(base)), t.key(t.row(cls)['obj']))
    if key not in _CLASS_INFO:
        other = Package(base)                                   # not invariants.load: the game has thousands of these
        k = _top_export(raw_tables(other), key[1])
        st = other.struct(k) if k is not None else None
        _CLASS_INFO[key] = (st.class_flags, other.path(st.within)) if hasattr(st, 'class_flags') else None
    return _CLASS_INFO[key]


def _class_flags(pkg, cls):
    """The ClassFlags of class `cls` when this package, or the /Game package its import names, holds it; else None."""
    info = _class_info(pkg, cls)
    return info[0] if info else None


def is_class_kind(kind):
    """Whether objects of class `kind` are classes: UClass, a DynamicClass, a *GeneratedClass (Blueprint, Widget,
    Anim, ControlRig ... - the game has all four)."""
    return kind in ('Class', 'DynamicClass') or kind.endswith('GeneratedClass')


GENERATED_CLASS_PATHS = {'/Script/Engine.BlueprintGeneratedClass', '/Script/UMG.WidgetBlueprintGeneratedClass',
                         '/Script/Engine.AnimBlueprintGeneratedClass'}


@rule
def export_rows(pkg):
    """Each export row is one the EDL can construct as written. ClassIndex is non-null and names a class: a null one is
    bExportLoadFailed, a non-class one fails CastChecked<UClass> (AsyncLoading.cpp 2443-2506, 2858-2874). bForcedExport
    is 0 (AsyncLoading.cpp 2880-2884, 2913-2917
    check(!bForcedExport)). A public export has a name (AsyncLoading.cpp 2445, 2811). ObjectFlags lie inside RF_Load
    (ObjectMacros.h 524): the cook masks them so (ObjectResource.cpp 60-62), and the EDL constructs the export with them
    as they stand (AsyncLoading.cpp 2970-2971), so a stray bit - RF_Transient, RF_MarkAsRootSet, RF_NeedLoad - lands on
    the live object. A Blueprint class export's class is one of the /Script generated-class kinds, as the compiler
    makes it. RF_ClassDefaultObject is on each class
    tail's CDO, and only on a Default__<class> beside a class of this package - it routes an export through the CDO
    path and renames it to the class's default-object name (AsyncLoading.cpp 2970-3016, UObjectGlobals.cpp 2369-2376).
    bIsAsset is IsAsset() (ObjectResource.cpp 86-90): RF_Public, neither RF_Transient nor RF_ClassDefaultObject, outer
    the package (Obj.cpp 1953-1966), a BPGC also not CLASS_NewerVersionExists (BlueprintGeneratedClass.cpp 1619-1625);
    the cooked AssetRegistry scan lists only bIsAsset exports (PackageReader.cpp 270-329). A non-CDO export's class is
    not CLASS_Abstract: StaticAllocateObject check()s it (UObjectGlobals.cpp 2362) when the EDL constructs the export
    (AsyncLoading.cpp 3039-3047) - checked where this package or one it reaches holds the class."""
    t = raw_tables(pkg)
    cdos = {st.cdo for _, st in classes(pkg)}
    for k, e in enumerate(pkg.exports):
        cls, flags = e['cls'], e['flags']
        if cls == 0: yield k, 'null ClassIndex'
        elif t.row(cls) is not None:
            kind = pkg.class_of(cls)
            if not is_class_kind(kind): yield k, 'its class %s is a %s, not a class' % (pkg.path(cls), kind)
        if e['forced']: yield k, 'bForcedExport is set'
        if flags & RF_Public and e['name'] == 'None': yield k, 'RF_Public with the name None'
        if flags & ~RF_Load: yield k, 'ObjectFlags %#x outside RF_Load (%#x)' % (flags, flags & ~RF_Load)
        if flags & RF_ClassDefaultObject:
            c = pkg.exports[cls - 1] if 0 < cls <= len(pkg.exports) else None
            if c is None or e['outer'] or e['name'].lower() != 'default__' + c['name'].lower():
                yield k, 'RF_ClassDefaultObject set, but not Default__<its class> beside a class of this package'
        elif k + 1 in cdos: yield k, "a class's CDO without RF_ClassDefaultObject"
        st = pkg.struct(k)
        if st is not None and hasattr(st, 'class_flags') and pkg.path(cls) not in GENERATED_CLASS_PATHS:
            yield k, 'a Blueprint class whose class is %s' % pkg.path(cls)
        asset = bool(flags & RF_Public and not flags & (RF_Transient | RF_ClassDefaultObject) and e['outer'] == 0)
        if st is not None and hasattr(st, 'class_flags'): asset = asset and not st.class_flags & CLASS_NewerVersionExists
        elif pkg.class_of(k + 1) == 'Class': asset = False                  # UClass::IsAsset, Class.h 2886
        if bool(t.exports[k]['is_asset']) != asset:
            yield k, 'bIsAsset %d, IsAsset() is %s' % (t.exports[k]['is_asset'], asset)
        if not flags & RF_ClassDefaultObject and cls:
            cf = _class_flags(pkg, cls)
            if cf is not None and cf & CLASS_Abstract: yield k, 'an instance of %s, which is Abstract' % pkg.path(cls)


def _class_chain(pkg, t, cls):
    """The class FPackageIndex `cls` of Raw t (a table of pkg's view) names and its supers, as lower-case paths, and
    whether the walk reached a /Script class, past which the chain is native and not on disk. Followed through /Game
    class imports into their packages; cut short (False) where a package or export is not at hand. A null cls is
    exactly UClass."""
    if cls == 0: return ['/script/coreuobject.class'], True
    out = []
    for _ in range(64):
        if cls > 0:
            e = t.row(cls)
            if e is None: return out, False
            out.append((t.package + '.' + t.name(e['obj'])).lower())
            cls = e['super']
            if cls == 0: return out, False
            continue
        root = t.root(cls)
        if root is None: return out, False
        top = t.name(t.row(root)['obj'])
        out.append((t.path(cls) or '').lower())
        if top.startswith('/Script/'): return out, True
        base = find_package(pkg, top) if top.startswith('/Game/') else None
        k = _top_export(header_at(base), t.key(t.row(cls)['obj'])) if base else None
        if k is None: return out, False
        t = header_at(base)
        cls = t.exports[k]['super']
        if cls == 0: return out, False
    return out, False


@rule
def export_outer_within(pkg):
    """Every class has a ClassWithin (StaticAllocateObject check()s it non-null, UObjectGlobals.cpp 2349), and a
    non-CDO export's outer IsA its class's ClassWithin: check()ed when the EDL constructs the export
    (UObjectGlobals.cpp 2365, reached from AsyncLoading.cpp 3039-3047), compiled out in Shipping, where the object is
    made inside an outer its class's code may cast unchecked (DECLARE_WITHIN's GetOuter<Within>, e.g. UFunction's
    GetOuterUClassUnchecked, ScriptCore.cpp 818-821). Checked where the disk decides it: the class, with its
    ClassWithin, is in this package or a /Game one at hand. ClassWithin Object always holds. An export directly in the
    package has its UPackage as the outer, which is only a Package. Otherwise the outer's class chain is followed
    through this package and the /Game ones: meeting the ClassWithin holds; reaching native code without meeting a
    /Game ClassWithin is a finding; a /Script ClassWithin that the on-disk part of the chain does not name is left
    undecided (the native hierarchy is not on disk)."""
    for i, st in classes(pkg):
        if not st.within: yield i, 'a class with no ClassWithin'
    t = raw_tables(pkg)
    for k, e in enumerate(pkg.exports):
        if e['flags'] & RF_ClassDefaultObject or not e['cls'] or pkg.obj(e['cls']) is None: continue
        info = _class_info(pkg, e['cls'])
        if not info or not info[1]: continue
        w = info[1].lower()
        if w == '/script/coreuobject.object': continue
        if e['outer'] == 0:
            if w != '/script/coreuobject.package':
                yield k, 'directly in the package, but its class %s is Within %s' % (pkg.path(e['cls']), info[1])
            continue
        if e['outer'] < 0 or pkg.obj(e['outer']) is None: continue          # an import outer: not in a cooked .uasset
        chain, native = _class_chain(pkg, t, pkg.exports[e['outer'] - 1]['cls'])
        if w in chain: continue
        if native and w.startswith('/game/'):
            yield k, 'its outer %s is a %s, not the %s its class %s is Within' % (
                pkg.path(e['outer']), chain[0], info[1], pkg.path(e['cls']))


@rule
def export_name_unique(pkg):
    """(ObjectName, OuterIndex) is unique over the export table, the name compared as FName compares it - case-folded,
    Number exact. ObjectNameWithOuterToExport keeps the last of two (LinkerLoad.cpp 2396-2403), so another package's
    import finds only that one, and at create time the second export finds the first's object by outer and name
    (AsyncLoading.cpp 2898-2952): the same class makes both exports one object serialized twice, another class fails the
    export. Two DefaultSceneRoot_GEN_VARIABLE under one class is that case."""
    t = raw_tables(pkg)
    seen = {}
    for k, e in enumerate(t.exports):
        key = (t.key(e['obj']), e['outer'])
        if key in seen:
            yield k, '%s under %s repeats export %d' % (t.name(e['obj']), pkg.path(e['outer']) or 'the package', seen[key])
        else: seen[key] = k


def _native_class(pkg, t, cls, depth=0):
    """The /Script class export or import `cls` of table t (of package pkg's view) is or descends from, as its name,
    following /Game class imports into their packages. None where the chain cannot be followed."""
    if depth > 32 or cls == 0: return None
    if cls > 0:
        e = t.row(cls)
        return None if e is None else _native_class(pkg, t, e['super'], depth + 1)
    root = t.root(cls)
    if root is None: return None
    top, me = t.name(t.row(root)['obj']), t.name(t.row(cls)['obj'])
    if top.startswith('/Script/'): return me
    base = find_package(pkg, top) if top.startswith('/Game/') else None
    if base is None: return None
    other = header_at(base)
    k = _top_export(other, t.key(t.row(cls)['obj']))
    return None if k is None else _native_class(pkg, other, other.exports[k]['super'], depth + 1)


# size - (tags + the lazy-object GUID bool) of a non-struct, non-CDO export, by its nearest native class: what each
# class's Serialize writes after UObject::Serialize (StaticMeshComponent's 4 is its empty LODData count). Calibrated on
# all 51,579 game packages (`calib.py game --sample 1 --tails` in the TABLES work package): the 397 classes with at
# least 20 samples, every sample agreeing. Classes whose tail varies (StaticMesh, Texture2D, SoundWave ...) are left out.
NATIVE_TAIL = dict.fromkeys((
    'Actor', 'ActorComponent', 'ActorTrackingComponent', 'AmmoDriveWeaponAggregator', 'AmmoDrivenWeaponUpgrade',
    'AnimNotifyState_SpawnAndReleaseActor', 'AnimNotifyState_SpawnMesh', 'AnimNotifyState_TimedNiagaraEffect',
    'AnimNotifyState_TimedParticleEffect', 'AnimNotify_CameraShake', 'AnimNotify_CopyBoneVisibility',
    'AnimNotify_CycleItemComplete', 'AnimNotify_FootStep', 'AnimNotify_PlayNiagaraEffect',
    'AnimNotify_PlayParticleEffect', 'AnimNotify_PlaySound2D', 'AnimNotify_Shout', 'ArmorHealthDamageComponent',
    'ArmorMaterialVanityItem', 'ArmorStatUpgrade', 'ArmorVanityItem', 'ArrowComponent', 'AudioComponent',
    'BTComposite_Selector', 'BTComposite_Sequence', 'BTComposite_SimpleParallel', 'BTDecorator_AttackInRange',
    'BTDecorator_Blackboard', 'BTDecorator_BlueprintBase', 'BTDecorator_ConditionalGuard', 'BTDecorator_InRange',
    'BTDecorator_LockRotation', 'BTDecorator_Loop', 'BTDecorator_ModifySpeed', 'BTDecorator_RandomChance',
    'BTDecorator_RandomCooldown', 'BTService_CheckPathToTarget', 'BTService_FindAttackable', 'BTTask_Attack',
    'BTTask_BlueprintBase', 'BTTask_MessageAI', 'BTTask_MoveToTarget', 'BTTask_SetCondition',
    'BTTask_TunnelToTarget', 'BTTask_Wait', 'BackgroundBlur', 'BackgroundBlurSlot', 'BallisticProjectileAttack',
    'BeardVanityItem', 'BehaviorTree', 'BillboardComponent', 'BiomeDependentLevelGenerationCarver',
    'BlackboardData', 'BlackboardKeyType_Bool', 'BlackboardKeyType_Vector', 'Border', 'BorderSlot', 'BoxComponent',
    'Button', 'ButtonSlot', 'CamapaignCompletedRequirement', 'CameraComponent', 'CampaignMission', 'CanvasPanel',
    'CanvasPanelSlot', 'CapsuleComponent', 'CaracterLevelCampaignRequirement', 'CarriableComponent',
    'CarriableInstantUsable', 'CheckBox', 'ChildActorComponent', 'ClothConfigNv', 'ClothingAssetCommon',
    'CoilgunUpgrade', 'CombinedUpgrade', 'ComboBoxString', 'ComponentDelegateBinding', 'CrossbowUpgrade',
    'CrosshairAggregator', 'CurveFloat', 'DLCAquisition', 'DamageClass', 'DamageComponent', 'DamageConversionBonus',
    'DamageUpgrade', 'DebrisCarved', 'DebrisDataComponent', 'DebrisItemComponent', 'DebrisMesh',
    'DebrisPositioning', 'DeepPatherFinderCharacterAfflictionComponent', 'DeepPathfinderMovement',
    'DeepPathfinderSceneComponent', 'DetailNoise', 'DialogDataAsset', 'DistributionFloatConstant',
    'DistributionFloatConstantCurve', 'DistributionFloatParticleParameter', 'DistributionFloatUniform',
    'DistributionVectorConstant', 'DistributionVectorConstantCurve', 'DistributionVectorParticleParameter',
    'DistributionVectorUniform', 'DotStatusEffectItem', 'DrinkableDataAsset', 'DropPodCalldownLocationFeature',
    'DropToTerrainComponent', 'EnemyComponent', 'EnemyDescriptor', 'EnemyGroupDescriptor', 'EnemyHealthComponent',
    'EnemyID', 'EnemyMeleeAttackAnimNotify', 'EnemyMinersManualData', 'EnemyPawnAfflictionComponent',
    'EnemyRangedAttackAnimNotify', 'EnemyTemperatureComponent', 'EntranceFeature', 'FSDAchievement',
    'FSDAdvancedLabel', 'FSDAnimNotify_PlaySound', 'FSDAudioComponent', 'FSDLabelWidget', 'FSDPhysicalMaterial',
    'FSDProjectileMovementComponent', 'FSDUserWidget', 'FirstPersonSkeletalMeshComponent',
    'FirstPersonWidgetComponent', 'FloatPerkAsset', 'FloodFillLine', 'FloodFillPillar', 'FloodFillSettings',
    'GemResourceData', 'GridSlot', 'HeadVanityItem', 'HeatSourceStatusEffectItem', 'HitReactionComponent',
    'HitscanBaseUpgrade', 'HitscanComponent', 'HorizontalBox', 'HorizontalBoxSlot', 'Image',
    'InfluencerSpawnComponent', 'InheritableComponentHandler', 'InstantUsable', 'InventoryItemUpgrade', 'ItemData',
    'ItemID', 'ItemRefundList', 'ItemSkin', 'ItemSkinSet', 'ItemUpgradeCategory', 'KnockbackDamageBonus',
    'LevelGenerationCarverComponent', 'LockOnWeaponUpgrade', 'MaterialFunction', 'MaterialSkinEffect',
    'MatineeCameraShakePattern', 'MeleeAttackComponent', 'MilestoneAsset', 'MinersManualData', 'MissionStat',
    'MoustacheVanityItem', 'MovieScene', 'MovieScene2DTransformSection', 'MovieScene2DTransformTrack',
    'MovieScene3DTransformSection', 'MovieScene3DTransformTrack', 'MovieSceneAudioSection', 'MovieSceneAudioTrack',
    'MovieSceneBoolSection', 'MovieSceneBoolTrack', 'MovieSceneBuiltInEasingFunction', 'MovieSceneByteSection',
    'MovieSceneByteTrack', 'MovieSceneColorSection', 'MovieSceneColorTrack', 'MovieSceneCompiledData',
    'MovieSceneFloatSection', 'MovieSceneFloatTrack', 'MovieSceneMarginSection', 'MovieSceneMarginTrack',
    'MovieSceneParticleSection', 'MovieSceneParticleTrack', 'MovieSceneVisibilityTrack', 'NamedSlot',
    'NiagaraComponent', 'NiagaraDataInterfaceArrayColor', 'NiagaraDataInterfaceArrayFloat',
    'NiagaraDataInterfaceArrayFloat3', 'NiagaraDataInterfaceCollisionQuery', 'NiagaraDataInterfaceColorCurve',
    'NiagaraDataInterfaceCurve', 'NiagaraDataInterfaceMeshRendererInfo', 'NiagaraDataInterfaceParticleRead',
    'NiagaraDataInterfaceSkeletalMesh', 'NiagaraDataInterfaceSpline', 'NiagaraDataInterfaceVector2DCurve',
    'NiagaraDataInterfaceVectorCurve', 'NiagaraDataInterfaceVectorField', 'NiagaraEmitter',
    'NiagaraLightRendererProperties', 'NiagaraMeshRendererProperties', 'NiagaraRibbonRendererProperties',
    'NormalProjectileAttack', 'OutlineComponent', 'OverclockBank', 'OverclockShematicItem', 'OverclockUpgrade',
    'Overlay', 'OverlaySlot', 'PanelSlot', 'ParticleLODLevel', 'ParticleModuleAcceleration',
    'ParticleModuleAccelerationConstant', 'ParticleModuleAccelerationDrag', 'ParticleModuleAttractorPoint',
    'ParticleModuleBeamNoise', 'ParticleModuleBeamSource', 'ParticleModuleBeamTarget', 'ParticleModuleCollision',
    'ParticleModuleCollisionGPU', 'ParticleModuleColor', 'ParticleModuleColorOverLife',
    'ParticleModuleColorScaleOverLife', 'ParticleModuleEventGenerator', 'ParticleModuleEventReceiverSpawn',
    'ParticleModuleLifetime', 'ParticleModuleLight', 'ParticleModuleLocation', 'ParticleModuleLocationEmitter',
    'ParticleModuleLocationPrimitiveCylinder', 'ParticleModuleLocationPrimitiveSphere',
    'ParticleModuleMeshMaterial', 'ParticleModuleMeshRotation', 'ParticleModuleMeshRotationRate',
    'ParticleModuleOrbit', 'ParticleModuleOrientationAxisLock', 'ParticleModuleParameterDynamic',
    'ParticleModulePivotOffset', 'ParticleModuleRotation', 'ParticleModuleRotationRate', 'ParticleModuleSize',
    'ParticleModuleSizeMultiplyLife', 'ParticleModuleSizeScale', 'ParticleModuleSizeScaleBySpeed',
    'ParticleModuleSpawn', 'ParticleModuleSubUV', 'ParticleModuleTypeDataBeam2', 'ParticleModuleTypeDataGpu',
    'ParticleModuleTypeDataMesh', 'ParticleModuleTypeDataRibbon', 'ParticleModuleVectorFieldLocal',
    'ParticleModuleVelocity', 'ParticleModuleVelocityInheritParent', 'ParticleSpriteEmitter', 'ParticleSystem',
    'ParticleSystemComponent', 'PawnActionsComponent', 'PawnAffliction', 'PawnAlertComponent',
    'PawnSensingComponent', 'PawnStat', 'PawnStatsComponent', 'PhysicsConstraintTemplate', 'PickaxeBladePart',
    'PickaxeHandlePart', 'PickaxeHeadPart', 'PickaxeMaterialPart', 'PickaxePart', 'PickaxePartReward',
    'PickaxePommelPart', 'PickaxeShaftPart', 'PlayerAfflictionOverlay', 'PlayerRankCampaignRequirement',
    'PointLightComponent', 'ProgressBar', 'ProjectileAttackComponent', 'ProjectileExplosion',
    'ProjectileMovementComponent', 'ProjectileUpgrade', 'PushStatusEffectDamageBonus',
    'RandomLevelGenerationCarverComponent', 'RandomSelector', 'ResourceReward', 'RetainerBox', 'RichTextBlock',
    'RichTextSizable', 'RoomGenerator', 'RotatingMovementComponent', 'SCS_Node', 'STLMeshCarver', 'SafeZone',
    'SafeZoneSlot', 'ScaleBox', 'ScaleBoxSlot', 'SceneComponent', 'Schematic', 'SchematicAquisition',
    'SchematicReward', 'ScrollBox', 'ScrollBoxSlot', 'SeasonTokenReward', 'SideburnsVanityItem',
    'SimpleConstructionScript', 'SimpleHealthComponent', 'SimpleObjectInfoComponent', 'SingleUsableComponent',
    'SizeBox', 'SizeBoxSlot', 'SkeletalMeshComponent', 'SkeletalMeshSkinEffect', 'SkeletalMeshSocket',
    'SkinSchematicItem', 'SkinUnlock', 'SkinnableComponent', 'Slider', 'SoundAttenuation', 'SoundClass',
    'SoundConcurrency', 'SoundMix', 'SoundSubmix', 'Spacer', 'SpawnActorFeature',
    'SpawnActorWithDebrisPosComponent', 'SphereComponent', 'SplineComponent', 'SpotLightComponent',
    'StatChangeStatusEffectItem', 'StaticMeshCarver', 'StaticMeshSocket', 'StatusEffectTriggerComponent',
    'StatusEffectsComponent', 'StoreBoughtAquisition', 'TerrainDetectComponent', 'TerrainMaterial',
    'TerrainPlacementComponent', 'TerrainType', 'TextBlock', 'TextureDynamicIcon', 'TimelineTemplate',
    'TreassureAquisition', 'TriFacetDynamicIcon', 'TunnelEventActorSpawner', 'TunnelEventParentSpawner',
    'TutorialHintComponent', 'TwoFacetDynamicIcon', 'UniformGridSlot', 'UnlockedAquisition',
    'UpgradableGearComponent', 'UpgradableItemComponent', 'UseAnimationSetting', 'UserWidget',
    'VanityCollectionReward', 'VanityReward', 'VanitySchematicItem', 'VerticalBox', 'VerticalBoxSlot',
    'VictoryPose', 'VictoryPoseReward', 'VictoryPoseSchematicItem', 'WeakpointGlowComponent', 'WidgetAnimation',
    'WidgetComponent', 'WidgetSwitcher', 'WidgetSwitcherSlot', 'WidgetTree', 'WindowWidget'), 0)
NATIVE_TAIL.update({
    'AnimMontage': 16, 'AutoCarverComponent': 4, 'BlendSpace1D': 16, 'FileMediaSource': 8,
    'FirstPersonStaticMeshComponent': 4, 'FontFace': 4, 'MeshCarverComponent': 4, 'NiagaraSpriteRendererProperties':
    4, 'ParticleModuleRequired': 8, 'PathfinderCollisionComponent': 4, 'SoundCue': 2, 'SoundNodeAttenuation': 2,
    'SoundNodeBranch': 2, 'SoundNodeDelay': 2, 'SoundNodeDistanceCrossFade': 2, 'SoundNodeDoppler': 2,
    'SoundNodeEnveloper': 2, 'SoundNodeLooping': 2, 'SoundNodeMixer': 2, 'SoundNodeModulator': 2,
    'SoundNodeModulatorContinuous': 2, 'SoundNodeOscillator': 2, 'SoundNodeParamCrossFade': 2, 'SoundNodeRandom': 2,
    'SoundNodeSoundClass': 2, 'SoundNodeWavePlayer': 6, 'StaticMeshCarverComponent': 4, 'StaticMeshComponent': 4,
    'TerrainScannerStaticMesh': 4})
# From the source, where the game has no sample of the class as the nearest native one: UDataAsset::Serialize exists
# only WITH_EDITORONLY_DATA and writes nothing of its own (DataAsset.cpp 13-23), UPrimaryDataAsset declares none -
# so a mod's own data asset (AssetTest's MD_*) ends at its GUID bool, as the game's DialogDataAsset (1,023) do.
NATIVE_TAIL.update({'DataAsset': 0, 'PrimaryDataAsset': 0})


def _ism_tail(b, at, hierarchical):
    """Where an (H)ISMC template's native tail ends, read as its Serialize writes it: UStaticMeshComponent's LODData
    (only an empty one is read; None otherwise), the bCooked UBOOL, PerInstanceSMData and PerInstanceSMCustomData as
    BulkSerialize (element size, count, the elements), the render data's uint64 byte count and bytes when cooked
    (InstancedStaticMesh.cpp 2492-2540, 2394-2406), and a HISMC's cluster tree, bulk too (HierarchicalInstancedStaticMesh.cpp
    1987-2024)."""
    i32 = lambda: struct.unpack_from('<i', b, at)[0]
    if i32(): return None
    at += 4
    cooked = i32(); at += 4
    for _ in range(2):
        size, cnt = struct.unpack_from('<ii', b, at); at += 8 + size * cnt
    if cooked: at += 8 + struct.unpack_from('<Q', b, at)[0]
    if hierarchical:
        size, cnt = struct.unpack_from('<ii', b, at); at += 8 + size * cnt
    return at


ISM_CLASSES = {'InstancedStaticMeshComponent': False, 'HierarchicalInstancedStaticMeshComponent': True}


@rule
def payload_exact(pkg):
    """A non-struct export reads to exactly its SerialSize: the EDL checks the bytes Serialize consumed against it and a
    difference is Fatal 'Serial size mismatch' (AsyncLoading.cpp 3219-3229). The payload is UObject::Serialize - the
    tagged properties, then FLazyObjectPtr::PossiblySerializeObjectGuid's bool and optional GUID (Obj.cpp 1381-1409) -
    then what the nearest native class's Serialize adds, taken from NATIVE_TAIL (the classes Epic's cooks agree on); a
    UserDefinedEnum adds its TArray<TPair<FName, int64>> and CppForm byte (UEnum::Serialize, Enum.cpp 33-90); an
    instanced static mesh component's tail is read field by field (_ism_tail: its instance arrays make it vary; the
    game's 9 ISMC exports, 32 bytes each, agree, and no HISMC is in the game, so that one stands on the source alone).
    CDOs are cdo_body_exact's, structs struct_body_exact's."""
    t = raw_tables(pkg)
    for k, e in enumerate(pkg.exports):
        if e['flags'] & RF_ClassDefaultObject or pkg.is_struct(k): continue
        native = _native_class(pkg, t, e['cls'])
        if native != 'UserDefinedEnum' and native not in NATIVE_TAIL and native not in ISM_CLASSES: continue
        b, end = pkg.blob(k), pkg.tags(k).end
        guid = struct.unpack_from('<i', b, end)[0]
        end += 4 + (16 if guid else 0)
        if native == 'UserDefinedEnum':
            cnt = struct.unpack_from('<i', b, end)[0]
            end += 4 + 16 * cnt + 1
        elif native in ISM_CLASSES:
            try: end = _ism_tail(b, end, ISM_CLASSES[native])
            except struct.error: end = -1                       # ran past the payload: a size mismatch too
            if end is None: continue
        else: end += NATIVE_TAIL[native]
        if end != e['size']: yield k, 'a %s (native %s) reads to %d of %d bytes' % (pkg.class_of(k + 1), native, end, e['size'])


@rule
def cdo_body_exact(pkg):
    """A class's CDO export is exactly its tagged properties up to SerialSize: UClass::SerializeDefaultObject writes the
    tag stream and nothing after None - no lazy-object GUID, and no sparse class data, which no DRG class has
    (Class.cpp 4674-4697); any difference is 'Serial size mismatch', Fatal (AsyncLoading.cpp 3219-3229)."""
    for i, st in classes(pkg):
        if st.cdo <= 0: continue
        c = st.cdo - 1
        end, size = pkg.tags(c).end, pkg.exports[c]['size']
        if end != size: yield c, 'CDO tags end at %d of %d bytes' % (end, size)


@rule
def super_index_matches_payload(pkg):
    """A struct export's SuperIndex in the export table is the SuperStruct its body serializes: the EDL SetSuperStructs
    (and for a class Binds) from SuperIndex before serializing, then UStruct::Serialize overwrites it from the payload
    (AsyncLoading.cpp 3164-3171; Class.cpp 1796-1804), so two values bind against one parent and run against another."""
    for i in range(len(pkg.exports)):
        st = pkg.struct(i)
        if st is not None and st.super != pkg.exports[i]['super']:
            yield i, 'SuperIndex %s, body SuperStruct %s' % (pkg.path(pkg.exports[i]['super']), pkg.path(st.super))


@rule
def struct_super_kinds(pkg):
    """A Blueprint class has a super, and it is a class: the EDL Binds the class to it before UClass::Serialize makes
    the CDO through its ClassConstructor (AsyncLoading.cpp 3149-3171 Fatal 'Could not find SuperStruct'; Class.cpp
    3936-3976). A function's super, when it has one, is the same-named UFunction (FName compare) of a class up the
    owner's parent chain: the compiler takes it from SuperClass->FindFunctionByName (KismetCompiler.cpp 1731-1734), and
    RPC NetFields, callspace and RepLayout walk to the top-most function through it (Class.cpp 4189-4194). The chain is
    followed through this package, the mods beside it and GAME_CONTENT, and stops at a /Script class: past it any
    /Script super is taken on trust, since the native classes - and the native interfaces FindFunctionByName also
    searches - are not on disk. A /Game super off the chain would have to be a Blueprint interface's function the parent
    itself does not define; none of the game's 51,579 packages has one."""
    for i, st in classes(pkg):
        if st.super == 0: yield i, 'a class without a super'
        elif not is_class_kind(pkg.class_of(st.super)):
            yield i, 'super %s is a %s' % (pkg.path(st.super), pkg.class_of(st.super))
    for i, st in functions(pkg):
        if not st or st.super == 0: continue
        e, sup = pkg.exports[i], pkg.obj(st.super)
        if pkg.class_of(st.super) not in Package.FUNCTION_CLASSES:
            yield i, 'super %s is a %s' % (pkg.path(st.super), pkg.class_of(st.super)); continue
        if sup['name'].lower() != e['name'].lower():
            yield i, 'super %s has another name' % pkg.path(st.super); continue
        want = (pkg.path(sup['outer']) or '').lower()
        at, cur, ok = raw_tables(pkg), pkg.exports[e['outer'] - 1]['super'] if e['outer'] > 0 else 0, False
        for _ in range(32):
            if cur == 0 or at.row(cur) is None: break
            if (at.path(cur) or '').lower() == want: ok = True; break
            if cur > 0: cur = at.exports[cur - 1]['super']; continue
            root = at.root(cur)
            top = at.name(at.row(root)['obj']) if root is not None else ''
            if not top.startswith('/Game/'):
                ok = want.startswith('/script/')                     # native ancestry is not on disk
                break
            base = find_package(pkg, top)
            k = _top_export(header_at(base), at.key(at.row(cur)['obj'])) if base else None
            if k is None: ok = True; break                           # a class not at hand: imports_resolve's business
            at = header_at(base)
            cur = at.exports[k]['super']
        if not ok: yield i, 'super %s is not on the class\'s parent chain' % pkg.path(st.super)


@rule
def function_placement(pkg):
    """Every UFunction export sits directly in a Blueprint class (UFunction is DECLARE_WITHIN(UClass); the VM casts the
    outer to UClass, Class.h 1791, ScriptCore.cpp 818-821) that lists it in Children - an RPC missing there gets no
    NetField (Class.cpp 4182-4196) - and no two functions of one class share a name, as FName compares it: the second
    export would bind to the first's object (AsyncLoading.cpp 2901-2941) and FuncMap keeps one (Class.cpp 4413)."""
    seen = {}
    for k, st in functions(pkg):
        o = pkg.exports[k]['outer']
        if o <= 0 or not pkg.class_of(o).endswith('GeneratedClass'):
            yield k, 'a function inside %s' % (pkg.path(o) if o else 'the package'); continue
        owner = pkg.struct(o - 1)
        if owner is not None and k + 1 not in owner.children: yield k, 'not in the Children of %s' % pkg.exports[o - 1]['name']
        key = (o, pkg.exports[k]['name'].lower())
        if key in seen: yield k, 'a second function %s in %s' % (pkg.exports[k]['name'], pkg.exports[o - 1]['name'])
        seen[key] = k


@rule
def public_exports(pkg):
    """A Blueprint class, each of its UFunctions and its CDO are RF_Public: the Kismet compiler always makes them so
    (KismetCompiler.cpp 196, 1764; Class.cpp 3773) and the game keeps it. The cooked event-driven loader never checks
    the flag - it resolves an import through ObjectNameWithOuterToExport, built from every export (LinkerLoad.cpp
    2396-2403) - so what it guards is outside that path: the non-EDL linker (the editor, -NoEDL) refuses an import of a
    private export (LinkerLoad.cpp 3403-3424), and a private class is no asset (BlueprintGeneratedClass.cpp 1619-1625),
    so the cooked AssetRegistry scan does not list it (PackageReader.cpp 300-323). The CDO's flags are
    class_default_object's."""
    for i, st in classes(pkg):
        if not pkg.exports[i]['flags'] & RF_Public: yield i, 'the class is not RF_Public'
    klass = {i + 1 for i, _ in classes(pkg)}
    for k, e in enumerate(pkg.exports):
        if e['outer'] in klass and pkg.class_of(k + 1) in Package.FUNCTION_CLASSES and not e['flags'] & RF_Public:
            yield k, 'function %s is not RF_Public' % e['name']


def _cdo_of(pkg, t, x):
    """Whether FPackageIndex t names the CDO of class x: Default__<x> beside x (same outer), of class x."""
    if not t or not x or pkg.obj(t) is None or pkg.obj(x) is None: return False
    tr, xr = pkg.obj(t), pkg.obj(x)
    if tr['name'].lower() != 'default__' + xr['name'].lower() or tr['outer'] != xr['outer']: return False
    if t > 0: return tr['cls'] == x
    if x > 0: return False
    root = raw_tables(pkg).root(x)
    return (tr['class_name'].lower() == xr['name'].lower() and root is not None
            and tr['class_package'].lower() == pkg.obj(root)['name'].lower())


def _class_path(pkg, idx):
    """The class of the object FPackageIndex idx names, as a path."""
    o = pkg.obj(idx)
    if idx < 0: return (o['class_package'] + '.' + o['class_name']).lower()
    return (pkg.path(o['cls']) or '').lower()


def _export_at(pkg, t, idx):
    """(Raw, export index) of the object FPackageIndex idx of pkg names: an export here, or a /Game import's export
    in its own package; None where that is not at hand."""
    if idx > 0: return t, idx - 1
    return _export_of_import(pkg, t, idx) if idx < 0 else None


def _rule1_twin(pkg, t, k):
    """What GetArchetypeFromRequiredInfo's rule 1 finds for export k (UObjectArchetype.cpp 71-87): an object of k's
    name and class under the archetype of k's outer, as (package path, export index), when that archetype is on disk
    and holds one; else None. Only the same class is taken: FindObjectWithOuter's IsA would also take a subclass, which
    the disk cannot tell."""
    o = t.exports[k]['outer']
    if o <= 0 or not t.exports[o - 1]['tmpl']: return None
    holder = _export_at(pkg, t, t.exports[o - 1]['tmpl'])
    if holder is None: return None
    other, h = holder
    j = _by_key(other).get((t.key(t.exports[k]['obj']), h + 1))
    if j is None or _raw_class_path(other, j) != _raw_class_path(t, k): return None
    return other.package.lower(), j


def _export_key(found):
    return None if found is None else (found[0].package.lower(), found[1])


STATS = collections.Counter()       # how often a rule's rarer branch ran, for the calibration log


@rule
def export_archetype(pkg):
    """TemplateIndex is non-null and names the archetype GetArchetypeFromRequiredInfo finds (UObjectArchetype.cpp
    55-140; SavePackage.cpp 3814-3822 writes it): the EDL check()s it non-null (AsyncLoading.cpp 2954-2962, 3190-3193)
    and constructs the export on it, and GetArchetypeFromLoader hands it to SerializeTaggedProperties as the delta base
    (LinkerLoad.cpp 5530-5537), so a wrong one silently changes every default the export does not write. A CDO's is its
    super class's CDO (Class->GetArchetypeForCDO; DO_CHECK at AsyncLoading.cpp 2975-2982). An export in the package
    itself takes its class's CDO. A subobject takes the object of its own name and class under its outer's archetype
    (rule 1), or a parent Blueprint class's template of that name for an inherited component template (rules 2 and 3,
    BlueprintGeneratedClass.cpp 789-830), else its class's CDO. Rule 1 comes first: where the outer's archetype is on
    disk (this package, or a /Game one at hand) and holds an export of this name and class, that export is the only
    right template - the class CDO or anything else there is the wrong delta base. Where the outer's archetype is
    native (a /Script CDO's subobjects are not on disk), any of the three is accepted."""
    t = raw_tables(pkg)
    for k, e in enumerate(pkg.exports):
        tmpl, cls = e['tmpl'], e['cls']
        if tmpl == 0: yield k, 'null TemplateIndex'; continue
        if t.row(tmpl) is None or t.row(cls) is None: continue              # table_bounds
        if e['flags'] & RF_ClassDefaultObject:
            if cls > 0 and not _cdo_of(pkg, tmpl, pkg.exports[cls - 1]['super']):
                yield k, 'CDO template %s, not the super class\'s CDO' % pkg.path(tmpl)
            continue
        twin = _rule1_twin(pkg, t, k)
        if twin is not None:
            STATS['export_archetype rule-1 twin on disk'] += 1
            if _export_key(_export_at(pkg, t, tmpl)) != twin:
                yield k, 'template %s, but its outer\'s archetype %s holds %s of its class, which rule 1 finds first' % (
                    pkg.path(tmpl), pkg.path(pkg.exports[e['outer'] - 1]['tmpl']), e['name'])
            continue
        if _cdo_of(pkg, tmpl, cls): continue
        if e['outer'] == 0:
            yield k, 'template %s, not the CDO of its class %s' % (pkg.path(tmpl), pkg.path(cls)); continue
        tr = pkg.obj(tmpl)
        same = tr['name'].lower() == e['name'].lower() and _class_path(pkg, tmpl) == _class_path(pkg, k + 1)
        outer_tmpl = pkg.obj(e['outer'])['tmpl']
        if same and tr['outer'] == outer_tmpl and outer_tmpl: continue      # rule 1
        if same and tr['outer'] and pkg.class_of(tr['outer']).endswith('GeneratedClass') and (
                e['flags'] & RF_InheritableComponentTemplate or pkg.class_of(e['outer']).endswith('GeneratedClass')):
            continue                                                         # rules 2, 3
        yield k, 'template %s is neither %s under %s nor the CDO of %s' % (
            pkg.path(tmpl), e['name'], pkg.path(outer_tmpl) if outer_tmpl else 'a null archetype', pkg.path(cls))


PACKAGE_FLAGS = {'.uasset': {0x80000000, 0x80040000}, '.umap': {0x80020000, 0x80060000}}


@rule
def package_flags(pkg):
    """Summary.PackageFlags becomes the loaded UPackage's flags, only PKG_PlayInEditor kept from before (LinkerLoad.cpp
    1239-1242, 1442-1452), so a bit the cook would not write reaches the live package. The rule takes the cook's own
    values as the allowed set: PKG_FilterEditorOnly 0x80000000, optionally PKG_RequiresLocalizationGather 0x40000
    (nothing at run time reads it), and PKG_ContainsMap 0x20000 on a .umap - the only values in the game (all 51,579
    .uasset, whatever their first export: Blueprint, widget, anim, struct, enum or other asset; the .umap pair from a
    survey of its 26 maps, which invariants.packages does not read). Some of the other bits change loading outright:
    PKG_UnversionedProperties reparses every tagged payload as unversioned (LinkerLoad.cpp 1239-1242), PKG_CompiledIn
    makes IsNativeCodePackage() true, so an importer skips the package as a missing native one (AsyncLoading.cpp
    1763-1784); the flag values are ObjectMacros.h 105-136. For the bits no loader reads, the rule is the cook's habit,
    which the game keeps without exception."""
    ext = '.umap' if os.path.exists(pkg.base + '.umap') and not os.path.exists(pkg.base + '.uasset') else '.uasset'
    if pkg.package_flags not in PACKAGE_FLAGS[ext]:
        yield None, 'PackageFlags %#x, a cooked %s has %s' % (pkg.package_flags, ext, ' or '.join('%#x' % f for f in sorted(PACKAGE_FLAGS[ext])))


TABLES_RULES = ['table_bounds', 'names_split', 'import_chains', 'imports_resolve', 'export_rows', 'export_outer_within',
                'export_name_unique',
                'payload_exact', 'cdo_body_exact', 'super_index_matches_payload', 'struct_super_kinds',
                'function_placement', 'public_exports', 'export_archetype', 'package_flags']
