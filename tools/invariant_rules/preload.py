import sys
from invariants import *

# ---- PRELOAD: the event-driven loader's preload dependencies, and what a payload names being created in time.
#
# Every cooked package carries, per export, four lists of FPackageIndex (dumpexp.preload order): serialize-before-
# serialize, create-before-serialize, serialize-before-create, create-before-create. FAsyncPackage::SetupExports_Event
# turns each entry into an arc of the load graph (AsyncLoading.cpp 2459-2499), next to the implicit arc from an
# object's create to its serialize (1498-1538); nothing else orders one export against another within a package. So
# the rules below ask the graph, not the lists: "X is serialized before Y is created" holds when some chain of arcs
# leads there. The cook states each need directly (SavePackage.cpp 3962-4140), which is how the game's own packages
# pass, but a need met through another export's arcs is met at load too.
#
# Only objects the loader has to load are asked about: an export, or an import from a package that is not compiled in.
# A /Script/... import is native: SetupImports_Event finds it in memory and sets its XObject before any export is
# created (FindExistingImport, AsyncLoading.cpp 1696-1712), and IsFullyLoadedObj holds for it (AsyncPackageLoader.cpp
# 487-511), so a bCheckSerialized fetch of it never fails (AsyncLoading.cpp 2742); the cook drops /Script/CoreUObject
# ones altogether (SavePackage.cpp 3899, 3947).

RF_ClassDefaultObject, RF_ArchetypeObject, RF_DefaultSubObject = 0x10, 0x20, 0x40000
EDL_BPGC = ('BlueprintGeneratedClass', 'WidgetBlueprintGeneratedClass', 'AnimBlueprintGeneratedClass')


def edl_in_range(pkg, idx):
    return -pkg.import_count <= idx <= pkg.export_count


def edl_root(pkg, idx):
    """The package the object an FPackageIndex names lives in: this one for an export, else the name of the import
    chain's outermost import."""
    if idx > 0: return pkg.package_name()
    while pkg.obj(idx)['outer']: idx = pkg.obj(idx)['outer']
    return pkg.obj(idx)['name']


def edl_loadable(pkg, idx):
    """Whether the object an FPackageIndex names has to be loaded: an export, or an import out of a package that is
    not compiled in. A native (/Script) import is found in memory when the linker is set up."""
    return idx > 0 or (idx < 0 and not edl_root(pkg, idx).startswith('/Script/'))


def edl_deps(pkg, k):
    """Export k's four preload lists (serBeforeSer, createBeforeSer, serBeforeCreate, createBeforeCreate)."""
    return pkg.exports[k]['deps']


def edl_graph(pkg):
    """The package's own load graph as {node: its prerequisites}, a node being ('C', index) for an object's create
    and ('S', index) for its serialize (Export_StartIO folded into serialize: only the export's own serialize waits on
    it). Cached on the package."""
    g = pkg.__dict__.get('_edl_graph')
    if g is None:
        preds = {}
        for x in list(range(-pkg.import_count, 0)) + list(range(1, pkg.export_count + 1)):
            preds.setdefault(('S', x), set()).add(('C', x))
        for k, e in enumerate(pkg.exports):
            K = k + 1
            for (phase_dep, phase_me), lst in zip((('S', 'S'), ('C', 'S'), ('S', 'C'), ('C', 'C')), e['deps']):
                for d in lst: preds.setdefault((phase_me, K), set()).add((phase_dep, d))
        g = pkg._edl_graph = (preds, {})
    return g


def edl_before(pkg, a, b):
    """Whether node a is done before node b can start, by some chain of the package's arcs."""
    preds, ancestors = edl_graph(pkg)
    direct = preds.get(b, ())
    if a in direct: return True
    if a[0] == 'C' and ('S', a[1]) in direct: return True
    if b[0] == 'S' and (a in preds.get(('C', b[1]), ()) or (a[0] == 'C' and ('S', a[1]) in preds.get(('C', b[1]), ()))):
        return True
    if b not in ancestors:
        seen, todo = set(), [b]
        while todo:
            for p in preds.get(todo.pop(), ()):
                if p not in seen: seen.add(p); todo.append(p)
        ancestors[b] = seen
    return a in ancestors[b]


def edl_needs(pkg, dep, phase_dep, k, phase_me, what):
    """A finding (or None) when loadable `dep` is not done (phase_dep) before export k's phase_me."""
    if not dep or not edl_in_range(pkg, dep) or not edl_loadable(pkg, dep): return None
    if edl_before(pkg, (phase_dep, dep), (phase_me, k + 1)): return None
    listed = [n for n, lst in zip(('serBeforeSer', 'createBeforeSer', 'serBeforeCreate', 'createBeforeCreate'),
                                  edl_deps(pkg, k)) if dep in lst]
    return '%s %s is not %s before this export is %s (%s)' % (
        what, pkg.path(dep), {'S': 'serialized', 'C': 'created'}[phase_dep], {'S': 'serialized', 'C': 'created'}[phase_me],
        'listed in its ' + ', '.join(listed) + ' only' if listed else 'in none of its lists')


def edl_summary(pkg):
    """(PreloadDependencyCount, ExportOffset) off a cooked package summary, walked as dumpexp.preload walks it."""
    r = dumpexp.R(pkg.ua, 8)
    def skip(size):
        n = r.i32(); r.o += size * n
    r.o += 12
    skip(20)
    r.i32(); r.fstr(); r.u32()
    r.o += 16
    ecount, eoff = r.i32(), r.i32()
    r.o += 8 + 4 + 8 + 4 + 4 + 16
    skip(8)
    for _ in range(2):
        r.o += 10; r.fstr()
    r.u32(); skip(16)
    r.u32()
    for _ in range(r.i32()): r.fstr()
    r.i32(); r.i64(); r.i32()
    skip(4)
    return r.i32(), eoff


@rule
def edl_preload_arcs(pkg):
    """The preload dependency table is sound and the load graph it makes has no cycle. Every export's header indices
    (class, super, template, outer) are null or in range (Linker.h 110-159, Imp/Exp check the index; LinkerLoad.cpp
    5152-5227), and every import's outer is null or an import: the loader walks an import's outers with Linker->Imp,
    check()ing each is one (AsyncLoading.cpp 2004-2016). An export's run of dependencies lies inside
    PreloadDependencyCount and each entry is a non-null index in range (check(!Dep.IsNull()), and the arc's event node
    is looked up by it, AsyncLoading.cpp 2459-2499, 772-780). A cycle
    - an export waiting on itself through its own lists, or a loop through several - leaves those nodes' prerequisite
    counts above zero forever, so the package never finishes loading (FEventLoadGraph, AsyncLoading.cpp 772; the cook
    refuses one, EDLCookChecker, SavePackageUtilities.cpp 1387-1403)."""
    if not pkg.package_flags & 0x80000000: return            # no EDL table in an uncooked package
    for j, imp in enumerate(pkg.imports):
        if not edl_in_range(pkg, imp['outer']) or imp['outer'] > 0:
            yield None, 'import %s: outer %d is not an import' % (imp['name'], imp['outer'])
    count, eoff = edl_summary(pkg)
    b = pkg.ua
    for k, e in enumerate(pkg.exports):
        for f in ('cls', 'super', 'tmpl', 'outer'):
            if not edl_in_range(pkg, e[f]): yield k, '%s index %d out of range' % (f, e[f])
        first, *counts = struct.unpack_from('<5i', b, eoff + 104 * k + 84)
        if first >= 0 and first + sum(counts) > count:
            yield k, 'dependencies %d..%d run past PreloadDependencyCount %d' % (first, first + sum(counts), count)
            continue
        for name, lst in zip(('serBeforeSer', 'createBeforeSer', 'serBeforeCreate', 'createBeforeCreate'), e['deps']):
            for d in lst:
                if d == 0 or not edl_in_range(pkg, d): yield k, '%s entry %d is null or out of range' % (name, d)
    # Kahn's algorithm over the whole graph; whatever cannot be scheduled sits on or behind a cycle.
    preds, _ = edl_graph(pkg)
    nodes = set(preds) | {p for ps in preds.values() for p in ps}
    succ, indeg = {}, {n: 0 for n in nodes}
    for n, ps in preds.items():
        for p in ps: succ.setdefault(p, []).append(n); indeg[n] += 1
    todo = [n for n in nodes if not indeg[n]]
    while todo:
        for s in succ.get(todo.pop(), ()):
            indeg[s] -= 1
            if not indeg[s]: todo.append(s)
    stuck = {n for n in nodes if indeg[n]}
    if stuck:
        n, path = min(stuck, key=lambda x: (x[1] < 0, abs(x[1]), x[0])), []
        while n not in path:                                # walk prerequisites inside the stuck set until one repeats
            path.append(n)
            n = next(p for p in sorted(preds.get(n, ())) if p in stuck)
        loop = path[path.index(n):]
        k = next((x[1] - 1 for x in loop if x[1] > 0), None)
        yield k, 'load graph cycle: %s' % ' <- '.join('%s(%s)' % (ph, pkg.path(x) if edl_in_range(pkg, x) else x)
                                                      for ph, x in loop + [loop[0]])


@rule
def edl_create_prereqs(pkg):
    """What creating and serializing an export needs is ready first. Its ClassIndex is non-null: SetupExports_Event
    marks an export with a null one failed and never loads it (AsyncLoading.cpp 2443, 2503-2505). Its class is fully
    serialized before it is created (the ClassIndex is fetched with bCheckSerialized, 2867, a Fatal 'Missing
    Dependency' at 2742-2770 otherwise). Its TemplateIndex is non-null and names an object fully serialized before the
    export is serialized: EventDrivenSerializeExport check()s it is set and fetches it with bCheckSerialized for every
    export (3191-3193). An export the create step constructs needs it before it is created (2955-2956); the ones it
    finds in memory instead (2901-2941) - a CDO, which its class makes while it is serialized (Class.cpp 4632-4634), and
    a default subobject, which its outer's constructor makes - only before they are serialized, which is all the rule
    asks of those. Those check()s are compiled out of Shipping, where a null template makes an export that has to be
    constructed fail ('could not find its template', 2957-2962), and one found in memory fall back on GetArchetype
    (Obj.cpp 1450-1454). Its outer is created before it is (2878, the Fatal 'still waiting for creation' at
    2725-2739). The cook lists class and template in serialize-before-create for every export and the outer in
    create-before-create (SavePackage.cpp 3969-3972, 4070-4073); no game export has a null class or template."""
    if not pkg.package_flags & 0x80000000: return
    for k, e in enumerate(pkg.exports):
        if e['cls'] == 0: yield k, 'ClassIndex is null'
        if e['tmpl'] == 0: yield k, 'TemplateIndex is null'
        in_memory = e['flags'] & (RF_ClassDefaultObject | RF_DefaultSubObject)
        for dep, ph_dep, ph_me, what in ((e['cls'], 'S', 'C', 'class'),
                                         (e['tmpl'], 'S', 'S' if in_memory else 'C', 'template'),
                                         (e['outer'], 'C', 'C', 'outer')):
            msg = edl_needs(pkg, dep, ph_dep, k, ph_me, what)
            if msg: yield k, msg


@rule
def edl_super_serialized(pkg):
    """A struct export's super is fully serialized before the export is: EventDrivenSerializeExport fetches SuperIndex
    with bCheckSerialized (AsyncLoading.cpp 3149-3162), a Fatal 'Missing Dependency ... still waiting for
    serialization' (2742-2770) when it is not, and UClass::Serialize checks the super has no RF_NeedLoad (Class.cpp
    4555). The cook lists it in serialize-before-serialize through UStruct::GetPreloadDependencies (Class.cpp 719-722,
    SavePackage.cpp 3997-4004): for a function that overrides a Blueprint parent's, the parent's function
    (KismetCompiler.cpp 1774)."""
    if not pkg.package_flags & 0x80000000: return
    for k, e in enumerate(pkg.exports):
        if e['super'] and pkg.is_struct(k):
            msg = edl_needs(pkg, e['super'], 'S', k, 'S', 'super')
            if msg: yield k, msg


def edl_parent_cdo(pkg, sup):
    """The FPackageIndex of Default__<Parent> for a class whose super is `sup`: an import or export of that name
    beside the parent (same outer), or None."""
    o = pkg.obj(sup)
    want = 'Default__' + o['name']
    rows = pkg.imports if sup < 0 else pkg.exports
    for j, r in enumerate(rows):
        if r['name'].lower() == want.lower() and r['outer'] == o['outer']:
            return -j - 1 if sup < 0 else j + 1
    return None


@rule
def edl_class_cdo(pkg):
    """A class export has exactly one CDO, loaded after its parent's. Exactly one export is RF_ClassDefaultObject of
    this class, the one the class tail names: on the event-driven path the CDO the class makes while it is serialized
    (Class.cpp 4632-4634, allocated in the class's outer, 3773) is matched to its export by name, outer and class
    (AsyncLoading.cpp 2901-2941), and FindCDOExportIndex keys on the flag and the ClassIndex (BlueprintSupport.cpp
    1261-1277), so it sits beside the class. Its flags less RF_Transactional being exactly RF_Public |
    RF_ClassDefaultObject | RF_ArchetypeObject is what the cook writes - the flags the CDO is allocated with (Class.cpp
    3773) under the RF_Load mask - and every game CDO keeps it; the loader reads only the flag above, and the matched
    object keeps its own flags, so that part guards the writer, not a load path. Its
    TemplateIndex is the parent's CDO (UObjectArchetype.cpp 55-62; Class.cpp 4751-4755). The parent CDO is serialized
    before the class is (UBlueprintGeneratedClass::GetPreloadDependencies, BlueprintGeneratedClass.cpp 1430; under EDL
    UClass::GetDefaultObject checks it has no RF_NeedLoad, Class.cpp 3715-3725, and a Shipping build silently builds
    the child CDO from unloaded parent defaults), and the class is serialized before its CDO is created (the cook's
    serialize-before-create, SavePackage.cpp 3969-3972; AsyncLoading.cpp 2867)."""
    if not pkg.package_flags & 0x80000000: return
    for i, st in classes(pkg):
        e = pkg.exports[i]
        cdos = [k for k, x in enumerate(pkg.exports) if x['flags'] & RF_ClassDefaultObject and x['cls'] == i + 1]
        if cdos != [st.cdo - 1]:
            yield i, 'CDO exports %s, the class names %d' % ([pkg.exports[k]['name'] for k in cdos], st.cdo); continue
        c = pkg.exports[st.cdo - 1]
        if c['outer'] != e['outer'] or c['flags'] & ~0x8 != 0x31:
            yield i, 'CDO outer %d (class %d) flags %#x' % (c['outer'], e['outer'], c['flags'])
        if not e['super'] or not edl_in_range(pkg, e['super']): continue
        parent_cdo = edl_parent_cdo(pkg, e['super'])
        sup = pkg.path(e['super'])
        want = sup.rpartition('.')[0] + '.Default__' + sup.rpartition('.')[2] if '.' in sup else None
        if not edl_in_range(pkg, c['tmpl']) or not c['tmpl'] or (pkg.path(c['tmpl']) or '').lower() != (want or '').lower():
            yield i, 'CDO template %s, not the parent CDO %s' % (pkg.path(c['tmpl']) if edl_in_range(pkg, c['tmpl']) else c['tmpl'], want)
        if parent_cdo is None:
            if edl_loadable(pkg, e['super']): yield i, 'no Default__ of the parent %s in the linker table' % sup
        else:
            msg = edl_needs(pkg, parent_cdo, 'S', i, 'S', 'parent CDO')
            if msg: yield i, msg
        if not edl_before(pkg, ('S', i + 1), ('C', st.cdo)):
            yield i, 'the CDO can be created before the class is serialized'


def edl_obj_tag(pkg, k, name):
    """The FPackageIndex an export's ObjectProperty tag `name` holds, or 0."""
    t = pkg.tag(k, name)
    return struct.unpack_from('<i', t['value'])[0] if t and t['type'] == 'ObjectProperty' and t['size'] == 4 else 0


@rule
def edl_class_closure(pkg):
    """A Blueprint class is serialized only after the objects its Link and CDO construction read, and the ones an
    archetype lookup on the class reads: its ubergraph function, SimpleConstructionScript and
    InheritableComponentHandler, and the class and archetype of every default subobject of its CDO
    (UBlueprintGeneratedClass::GetPreloadDependencies, BlueprintGeneratedClass.cpp 1425-1459, and the cook's subobject
    archetypes, SavePackage.cpp 4013-4040). Otherwise Link's Preload of the ubergraph (BlueprintGeneratedClass.cpp
    1636) hits the EDL check (LinkerLoad.cpp 3982; Shipping serializes it out of order), the frame Serialize makes for
    the CDO (1768) finds the function without RF_LoadCompleted and is left out (1358-1375), the CDO the class makes
    while it is serialized (Class.cpp 4632-4634) builds each default subobject from an archetype still holding its
    constructor defaults, and a component archetype lookup on the class - by anything already holding the class as
    serialized - preloads a still unloaded SimpleConstructionScript out of order (BlueprintGeneratedClass.cpp 831-838)
    and meets RF_NeedLoad on the handler or its override: a Fatal (858-866, 892-898)."""
    if not pkg.package_flags & 0x80000000: return
    for i, st in classes(pkg):
        if st.kind not in EDL_BPGC: continue
        for name in ('UberGraphFunction', 'SimpleConstructionScript', 'InheritableComponentHandler'):
            msg = edl_needs(pkg, edl_obj_tag(pkg, i, name), 'S', i, 'S', name)
            if msg: yield i, msg
        for k, x in enumerate(pkg.exports):
            if x['outer'] == st.cdo and x['flags'] & RF_DefaultSubObject:
                for dep, what in ((x['cls'], 'class of CDO subobject %s' % x['name']),
                                  (x['tmpl'], 'archetype of CDO subobject %s' % x['name'])):
                    msg = edl_needs(pkg, dep, 'S', i, 'S', what)
                    if msg: yield i, msg


@rule
def edl_parent_subobjects_serialized(pkg):
    """A class whose parent is a loadable (Blueprint) class is serialized only after every default subobject its
    parent's CDO exports - whether or not this package restates it. The class makes its CDO while it is serialized
    (Class.cpp 4632-4634); the native constructor's CreateDefaultSubobject then finds that the outer's archetype is not
    native and copies each subobject's properties from the parent CDO's subobject of the same name
    (UObjectGlobals.cpp 3822-3859, InitSubobjectProperties 2932-2949), in whatever state it is in: one not serialized
    yet hands over its constructor defaults, and a child that does not restate the subobject keeps them. So
    UBlueprintGeneratedClass::GetPreloadDependencies adds the archetype of every default subobject of the CDO
    (BlueprintGeneratedClass.cpp 1437-1446), and the cook maps each into the linker table (call site 5,
    SavePackage.cpp 4013-4040, not only-if-in-table) and lists it in serialize-before-serialize. edl_class_closure asks
    the same of the subobjects this package exports, through their TemplateIndex; this rule follows the parent class
    into its own package (Package.resolve) and asks it of every subobject there, so one this package does not export,
    or exports with the wrong TemplateIndex, is asked about too. A parent package that cannot be found is skipped.
    Nested ones as well: the cook's walk takes every object under the CDO, at any depth, that is a default subobject or
    an archetype (GetObjectsWithOuter includes nested objects, UObjectHash.h 55), such as the instanced bonus under
    WPN_Pickaxe's Damage, Damage:BreakIceBonus_0, which the child's own Damage gets a copy of; each of the game's
    child classes lists every one of its parent's."""
    if not pkg.package_flags & 0x80000000: return
    for i, st in classes(pkg):
        sup = pkg.exports[i]['super']
        if not sup or not edl_in_range(pkg, sup) or not edl_loadable(pkg, sup): continue
        got = pkg.resolve(sup)
        if not got: continue
        ppkg, pk = got
        pst = ppkg.struct(pk)
        if not pst or not hasattr(pst, 'class_flags') or not 0 < pst.cdo <= ppkg.export_count: continue

        def under_cdo(px):
            up = px['outer']
            while 0 < up <= ppkg.export_count and up != pst.cdo: up = ppkg.exports[up - 1]['outer']
            return up == pst.cdo
        for x, px in enumerate(ppkg.exports):
            if not px['flags'] & (RF_DefaultSubObject | RF_ArchetypeObject) or not under_cdo(px): continue
            want = ppkg.path(x + 1).lower()
            here = next((j for j in list(range(1, pkg.export_count + 1)) + list(range(-1, -pkg.import_count - 1, -1))
                         if (pkg.path(j) or '').lower() == want), None)
            if here is None:
                yield i, 'parent CDO subobject %s is not in the linker table, so nothing orders it before this class ' \
                         'is serialized' % ppkg.path(x + 1)
                continue
            msg = edl_needs(pkg, here, 'S', i, 'S', 'parent CDO subobject')
            if msg: yield i, msg


def edl_type_refs(props):
    """(property name, struct or enum index) of every StructProperty, ByteProperty and EnumProperty in a property
    chain, through array, set and map inners but not an EnumProperty's underlying property (the
    FProperty::GetPreloadDependencies overrides: PropertyStruct.cpp 183-187, PropertyByte.cpp 33-37, EnumProperty.cpp
    366-371, PropertyArray.cpp 41-48, PropertySet.cpp 248-255, PropertyMap.cpp 289-300)."""
    for p in props:
        if p.type in ('StructProperty', 'ByteProperty') and p.ref: yield p.name, p.ref
        elif p.type == 'EnumProperty' and p.enum: yield p.name, p.enum
        if p.type in ('ArrayProperty', 'SetProperty', 'MapProperty'):
            yield from ((p.name + '.' + n, r) for n, r in edl_type_refs(p.subs))


@rule
def edl_property_types(pkg):
    """A class, function or struct is serialized only after every user-defined struct and enum its properties are typed
    by - parameters and locals, and array, set and map elements, included. UStruct::GetPreloadDependencies adds each
    property's type (Class.cpp 732-735, the overrides named in edl_type_refs) and the cook lists them in serialize-
    before-serialize (SavePackage.cpp 3997-4004). The owner reads the type while it is serialized: the struct links
    there (Class.cpp 1884, 4491) and FStructProperty::LinkInternal preloads the struct and takes its PropertiesSize as
    ElementSize (PropertyStruct.cpp 91-103), and FByteProperty / FEnumProperty::Serialize preload the enum
    (PropertyByte.cpp 112-120, EnumProperty.cpp 196-203); an unserialized one trips the EDL check in
    FLinkerLoad::Preload (LinkerLoad.cpp 3980-3982) and a Shipping build serializes it out of order."""
    if not pkg.package_flags & 0x80000000: return
    for k in range(len(pkg.exports)):
        st = pkg.struct(k)
        if not st: continue
        seen = set()
        for name, ref in edl_type_refs(st.props):
            if ref in seen: continue
            seen.add(ref)
            msg = edl_needs(pkg, ref, 'S', k, 'S', 'type of %s,' % name)
            if msg: yield k, msg


# -- payload references

EDL_OBJ_TAGS = ('ObjectProperty', 'ClassProperty', 'InterfaceProperty', 'WeakObjectProperty')
EDL_FIXED = {'IntProperty': 4, 'UInt32Property': 4, 'Int16Property': 2, 'UInt16Property': 2, 'Int8Property': 1,
             'FloatProperty': 4, 'DoubleProperty': 8, 'Int64Property': 8, 'UInt64Property': 8, 'NameProperty': 8,
             'EnumProperty': 8, 'BoolProperty': 1}


class EdlOpaque(Exception):
    """A value the walk cannot read as tags: a natively serialized struct, or an element type of unknown size."""


def edl_tags(pkg, b, o, end, path, out):
    """Walk FPropertyTags from o to None (PropertyTag.cpp; UStruct::SerializeTaggedProperties), appending
    (property path, FPackageIndex) for every object reference in their values; returns the offset after None. Raises
    EdlOpaque, IndexError or struct.error where the bytes are not tags."""
    def name(at):
        i, num = struct.unpack_from('<ii', b, at)
        if not 0 <= i < len(pkg.names) or num < 0: raise EdlOpaque('not a name')
        return pkg.names[i], at + 8
    while True:
        if o + 8 > end: raise EdlOpaque('ran past the end')
        pname, o = name(o)
        if pname == 'None': return o
        ptype, o = name(o)
        size, index = struct.unpack_from('<ii', b, o); o += 8
        extra = ()
        if ptype == 'StructProperty': sname, o = name(o); o += 16; extra = (sname,)
        elif ptype == 'BoolProperty': o += 1
        elif ptype in ('ByteProperty', 'EnumProperty'): o += 8
        elif ptype in ('ArrayProperty', 'SetProperty'): inner, o = name(o); extra = (inner,)
        elif ptype == 'MapProperty': kt, o = name(o); vt, o = name(o); extra = (kt, vt)
        if b[o]: o += 16
        o += 1
        if size < 0 or o + size > end: raise EdlOpaque('value past the end')
        edl_value(pkg, b, o, o + size, ptype, extra, '%s%s[%d]' % (path, pname, index), out)
        o += size


def edl_value(pkg, b, o, end, ptype, extra, path, out):
    """The object references in one tagged value of type ptype spanning [o, end). An unreadable value adds nothing."""
    try:
        got = []
        edl_item(pkg, b, o, end, ptype, extra, path, got, whole=True)
        out.extend(got)
    except (EdlOpaque, IndexError, struct.error):
        pass


def edl_item(pkg, b, o, end, ptype, extra, path, out, whole=False):
    """One value of type ptype at o; returns the offset after it. `whole` says it must span exactly to end."""
    if ptype in EDL_OBJ_TAGS:
        out.append((path, struct.unpack_from('<i', b, o)[0])); o += 4
    elif ptype == 'DelegateProperty':
        out.append((path, struct.unpack_from('<i', b, o)[0])); o += 12
    elif ptype in ('MulticastInlineDelegateProperty', 'MulticastSparseDelegateProperty', 'MulticastDelegateProperty'):
        n = struct.unpack_from('<i', b, o)[0]; o += 4
        for j in range(n): out.append(('%s.%d' % (path, j), struct.unpack_from('<i', b, o)[0])); o += 12
    elif ptype == 'StructProperty':
        o = edl_tags(pkg, b, o, end, path + '.', out)
    elif ptype == 'ArrayProperty':
        inner = extra[0]
        n = struct.unpack_from('<i', b, o)[0]; o += 4
        if inner == 'StructProperty':                   # the inner tag, then each element (FArrayProperty::SerializeItem)
            sub = []
            o2 = edl_tags_header(pkg, b, o)
            stop = o2[1] + o2[0]
            o = o2[1]
            for j in range(n): o = edl_tags(pkg, b, o, stop, '%s.%d.' % (path, j), sub)
            if o != stop: raise EdlOpaque('struct array elements')
            out.extend(sub)
        else:
            for j in range(n): o = edl_item(pkg, b, o, end, inner, (), '%s.%d' % (path, j), out)
    elif ptype == 'SetProperty':
        inner = extra[0]
        for part in ('removed', 'elements'):
            n = struct.unpack_from('<i', b, o)[0]; o += 4
            for j in range(n): o = edl_item(pkg, b, o, end, inner, (), '%s.%s%d' % (path, part[0], j), out)
    elif ptype == 'MapProperty':
        kt, vt = extra
        n = struct.unpack_from('<i', b, o)[0]; o += 4
        for j in range(n): o = edl_item(pkg, b, o, end, kt, (), '%s.r%d' % (path, j), out)
        n = struct.unpack_from('<i', b, o)[0]; o += 4
        for j in range(n):
            o = edl_item(pkg, b, o, end, kt, (), '%s.k%d' % (path, j), out)
            o = edl_item(pkg, b, o, end, vt, (), '%s.v%d' % (path, j), out)
    elif ptype in ('SoftObjectProperty', 'SoftClassProperty'):      # FSoftObjectPath: an FName and an FString
        o += 8
        n = struct.unpack_from('<i', b, o)[0]; o += 4 + (n if n >= 0 else -2 * n)
    elif ptype == 'StrProperty':
        n = struct.unpack_from('<i', b, o)[0]; o += 4 + (n if n >= 0 else -2 * n)
    elif ptype in EDL_FIXED and not whole:
        o += EDL_FIXED[ptype]
    elif whole:
        return end                                      # a scalar the tag's size already spans
    else:
        raise EdlOpaque('element type %s' % ptype)
    if whole and o != end: raise EdlOpaque('value size')
    return o


def edl_tags_header(pkg, b, o):
    """An array's inner FPropertyTag (a struct array's): (the elements' byte size, the offset after the tag)."""
    def name(at):
        i, num = struct.unpack_from('<ii', b, at)
        if not 0 <= i < len(pkg.names): raise EdlOpaque('not a name')
        return pkg.names[i], at + 8
    _, o = name(o)
    t, o = name(o)
    if t != 'StructProperty': raise EdlOpaque('inner tag type %s' % t)
    size = struct.unpack_from('<i', b, o)[0]; o += 8
    _, o = name(o); o += 16
    if b[o]: o += 16
    return size, o + 1


def edl_payload_refs(pkg, k):
    """(where, FPackageIndex) of every object export k's payload names, as far as it can be read: the leading tagged
    properties (a CDO's too), a struct's body (super, children, each property's class / struct / enum / signature,
    FuncMap, within, interfaces, CDO), a user-defined struct's default instance, and every object operand and property
    owner of its bytecode. A class's native tail beyond the UClass body, and component tails, are not read."""
    out, b = [], pkg.blob(k)
    try: edl_tags(pkg, b, 0, len(b), '', out)
    except (EdlOpaque, IndexError, struct.error): pass
    st = pkg.struct(k)
    if st:
        out.append(('SuperStruct', st.super))
        out += [('Children', c) for c in st.children]

        def props(ps, at):
            for p in ps:
                out.append((at + p.name, p.ref))
                if p.meta: out.append((at + p.name + '.meta', p.meta))
                if p.enum: out.append((at + p.name + '.enum', p.enum))
                props(p.subs, at + p.name + '.')
        props(st.props, 'prop ')
        for name, v in getattr(st, 'func_map', ()): out.append(('FuncMap ' + name, v))
        if hasattr(st, 'class_flags'):
            out += [('ClassWithin', st.within), ('ClassGeneratedBy', st.generated_by), ('ClassDefaultObject', st.cdo)]
            out += [('Interfaces', c) for c, _, _ in st.interfaces]
        if getattr(st, 'defaults', None) is not None:     # a user-defined struct's default instance
            start = next(at for (j, at), v in pkg._tags.items() if j == k and v is st.defaults)
            try: edl_tags(pkg, b, start, len(b), 'default ', out)
            except (EdlOpaque, IndexError, struct.error): pass
        for n in (x for t in pkg.script(k) for x in t.walk()):
            for op in n.ops:
                if op[0] == 'obj': out.append(('script op %02x at mem %d' % (n.op, n.mem), op[1]))
                elif op[0] == 'prop' and op[2]: out.append(('script %s owner' % '.'.join(op[1]), op[2]))
    return [(w, r) for w, r in out if r]


@rule
def edl_payload_created(pkg):
    """Every object an export's payload names is in range and created before the export is serialized. While an export
    is serialized the linker resolves each FPackageIndex as Exp(i).Object / Imp(i).XObject, no loading and no check
    (bForceSimpleIndexToObject, AsyncLoading.cpp 3188; LinkerLoad.cpp 5397-5421): an index out of range reads past
    the table, and an object not created yet reads as a silent null - a call target, a component template, a property's
    class. The cook lists every reference the save recorded (DependsMap) in create-before-serialize unless another list
    already orders it, leaving out only the class's own CDO, which the class makes while it is serialized
    (SavePackage.cpp 4043-4068, 4104-4127). An export naming itself is created before it is serialized anyway."""
    if not pkg.package_flags & 0x80000000: return
    for k, e in enumerate(pkg.exports):
        st = pkg.struct(k)
        own_cdo = st.cdo if st is not None and hasattr(st, 'class_flags') else None
        seen = set()
        for where, r in edl_payload_refs(pkg, k):
            if not edl_in_range(pkg, r):
                yield k, '%s: index %d out of range' % (where, r); continue
            if r == k + 1 or r == own_cdo or r in seen: continue
            seen.add(r)
            msg = edl_needs(pkg, r, 'C', k, 'S', where + ':')
            if msg: yield k, msg


def edl_tag_refs(pkg, k):
    """(property path, FPackageIndex) of every object reference in export k's leading tags; [] if they do not read."""
    out = []
    try: edl_tags(pkg, pkg.blob(k), 0, pkg.exports[k]['size'], '', out)
    except (EdlOpaque, IndexError, struct.error): return []
    return out


@rule
def edl_cdo_components(pkg):
    """A Blueprint class that carries cooked component instancing data has its CDO serialized after the component
    templates that data is built from: SerializeDefaultObject builds each SCS node's and each ICH record's cached
    property data from its template, ensure()s the template has no RF_NeedLoad and otherwise logs a Warning and drops
    the node to the slow path (BlueprintGeneratedClass.cpp 330-393, only when bHasCookedComponentInstancingData). The
    cook lists, for the CDO, every node of the class's own SCS AllNodes, each node's template, each ICH override
    template, every ComponentTemplates entry and every ComponentClassOverrides class
    (GetDefaultObjectPreloadDependencies, BlueprintGeneratedClass.cpp 1461-1520, via Obj.cpp 1247-1257). Templates an
    ancestor's SCS supplies unchanged are not checked here."""
    if not pkg.package_flags & 0x80000000: return
    for i, st in classes(pkg):
        if st.kind not in EDL_BPGC: continue
        flag = pkg.tag(i, 'bHasCookedComponentInstancingData')
        if not flag or not flag.get('bool'): continue
        want = []
        for path, r in edl_tag_refs(pkg, i):
            if path.startswith('ComponentTemplates[') or (path.startswith('ComponentClassOverrides[')
                                                          and path.endswith('.ComponentClass[0]')):
                want.append((path, r))
        scs = edl_obj_tag(pkg, i, 'SimpleConstructionScript')
        if scs > 0:
            for path, node in edl_tag_refs(pkg, scs - 1):
                if not path.startswith('AllNodes['): continue
                want.append(('SCS node', node))
                if node > 0: want += [('template of ' + pkg.exports[node - 1]['name'], edl_obj_tag(pkg, node - 1, 'ComponentTemplate'))]
        ich = edl_obj_tag(pkg, i, 'InheritableComponentHandler')
        if ich > 0:
            want += [('ICH record template', r) for path, r in edl_tag_refs(pkg, ich - 1)
                     if path.startswith('Records[') and path.endswith('.ComponentTemplate[0]')]
        for what, r in want:
            msg = edl_needs(pkg, r, 'S', st.cdo - 1, 'S', what)
            if msg: yield st.cdo - 1, msg
