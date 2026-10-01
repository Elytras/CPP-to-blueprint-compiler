import sys
from invariants import *
import glob, os, re, struct
import invariants

# ---- readers shared by the SCS rules below and the calibration drivers: an actor class's SimpleConstructionScript,
# its SCS_Node exports and InheritableComponentHandler records as the loader reads them off their tags, and a class
# classifier that follows a class up its supers across packages (Package.resolve) and, past the last Blueprint, through
# UeApi's reflected parent chain (the chain AssetGen's own EngineParent lookup walks).

UEAPI = UEAPI_DIR
SCENE = '/Script/Engine.SceneComponent'
ACTOR_COMPONENT = '/Script/Engine.ActorComponent'


def i32(b, o=0):
    return struct.unpack_from('<i', b, o)[0]


# ---- tag values

def obj_tag(pkg, i, name):
    """An ObjectProperty tag's FPackageIndex; 0 when the tag is absent (the property keeps its zeroed default)."""
    t = pkg.tag(i, name)
    return i32(t['value']) if t and t['type'] == 'ObjectProperty' else 0


def objs_tag(pkg, i, name):
    """An ArrayProperty of ObjectProperty: its FPackageIndex elements; [] when absent."""
    t = pkg.tag(i, name)
    if not t or t['type'] != 'ArrayProperty': return []
    v = t['value']
    return [i32(v, 4 + 4 * k) for k in range(i32(v))]


def name_tag(pkg, i, name):
    """A NameProperty tag's FName ('Name' or 'Name_3'); None when absent, which the loader leaves NAME_None."""
    t = pkg.tag(i, name)
    if not t or t['type'] != 'NameProperty': return None
    n = pkg._name(Rd(t['value']))
    return None if n == 'None' else n


def bool_tag(pkg, i, name):
    t = pkg.tag(i, name)
    return bool(t['bool']) if t and t['type'] == 'BoolProperty' else False


def enum_tag(pkg, i, name):
    """An EnumProperty / ByteProperty-enum tag's value name (EComponentCreationMethod::Native), or None."""
    t = pkg.tag(i, name)
    if not t: return None
    if t['type'] == 'EnumProperty' or (t['type'] == 'ByteProperty' and t.get('enum') not in (None, 'None')):
        return pkg._name(Rd(t['value']))
    return t['value'][0] if t['value'] else None


def struct_elements(pkg, i, t):
    """The element TagLists of an ArrayProperty of StructProperty tag t on export i: after the count, the array's one
    inner FPropertyTag (name, type, size, index, struct name, struct guid, has-guid), then each element's tags."""
    at = t['at']
    n = i32(pkg.blob(i), at)
    pos = at + 4 + 8 + 8 + 4 + 4 + 8 + 16 + 1
    out = []
    for _ in range(n):
        tl = pkg.tags(i, pos)
        out.append(tl)
        pos = tl.end
    return out


def sub_tags(pkg, i, t):
    """A StructProperty tag's value read as nested tags (a struct without a native Serialize)."""
    return pkg.tags(i, t['at'])


def by_name(tl):
    return {t['name']: t for t in tl}


# ---- the construction script

class ScsNode:
    """One SCS_Node export (idx is its FPackageIndex), read off its tags as USCS_Node's UPROPERTYs load."""
    def __init__(s, pkg, idx):
        e = idx - 1
        s.idx, s.export = idx, pkg.exports[e]
        s.cls = pkg.class_of(idx)
        s.var = name_tag(pkg, e, 'InternalVariableName')
        s.comp_class = obj_tag(pkg, e, 'ComponentClass')
        s.template = obj_tag(pkg, e, 'ComponentTemplate')
        s.children = objs_tag(pkg, e, 'ChildNodes')
        g = pkg.tag(e, 'VariableGuid')
        s.guid = bytes(g['value']) if g and g['type'] == 'StructProperty' else None
        s.parent_name = name_tag(pkg, e, 'ParentComponentOrVariableName')
        s.parent_owner = name_tag(pkg, e, 'ParentComponentOwnerClassName')
        s.parent_native = bool_tag(pkg, e, 'bIsParentComponentNative')


class Scs:
    """A Blueprint class's SimpleConstructionScript: `cls` the class export's FPackageIndex, `idx` the SCS's (from the
    class's SimpleConstructionScript tag), its RootNodes / AllNodes / DefaultSceneRootNode tags."""
    def __init__(s, pkg, cls, idx):
        s.pkg, s.cls, s.idx = pkg, cls, idx
        s.is_export = idx > 0
        e = idx - 1
        s.roots = objs_tag(pkg, e, 'RootNodes') if s.is_export else []
        s.all = objs_tag(pkg, e, 'AllNodes') if s.is_export else []
        s.dsr = obj_tag(pkg, e, 'DefaultSceneRootNode') if s.is_export else 0
        s._nodes = {}

    def node(s, idx):
        """The ScsNode at FPackageIndex idx, or None for a null, an import or an export that is not an SCS_Node."""
        if idx <= 0 or s.pkg.class_of(idx) != 'SCS_Node': return None
        if idx not in s._nodes: s._nodes[idx] = ScsNode(s.pkg, idx)
        return s._nodes[idx]

    def listed(s):
        """Every FPackageIndex the SCS names as a node: its three tags and every listed node's ChildNodes."""
        seen = list(s.roots) + list(s.all) + ([s.dsr] if s.dsr else [])
        for x in list(seen):
            n = s.node(x)
            if n: seen += n.children
        return seen

    def walk(s):
        """The nodes ExecuteScriptOnActor reaches, root first, depth first (USCS_Node::GetAllNodes' order), and each
        index reached more than once. A cycle stops at the repeat."""
        order, again, seen = [], [], set()
        def go(x):
            if x in seen: again.append(x); return
            seen.add(x); order.append(x)
            n = s.node(x)
            for c in (n.children if n else []): go(c)
        for r in s.roots: go(r)
        return order, again

    def class_name(s):
        return s.pkg.exports[s.cls - 1]['name']


def construction_scripts(pkg):
    """Scs of every Blueprint class export of pkg that names one."""
    for i, st in classes(pkg):
        t = pkg.tag(i, 'SimpleConstructionScript')
        if t and t['type'] == 'ObjectProperty' and i32(t['value']):
            yield Scs(pkg, i + 1, i32(t['value']))


class IchRecord:
    """One FComponentOverrideRecord of an InheritableComponentHandler's Records."""
    def __init__(s, pkg, h, tl):
        d = by_name(tl)
        g = lambda k: i32(d[k]['value']) if k in d and d[k]['type'] == 'ObjectProperty' else 0
        s.comp_class, s.template = g('ComponentClass'), g('ComponentTemplate')
        s.owner, s.var, s.guid = 0, None, None
        if 'ComponentKey' in d:
            k = by_name(sub_tags(pkg, h, d['ComponentKey']))
            s.owner = i32(k['OwnerClass']['value']) if 'OwnerClass' in k else 0
            if 'SCSVariableName' in k:
                n = pkg._name(Rd(k['SCSVariableName']['value']))
                s.var = None if n == 'None' else n
            if 'AssociatedGuid' in k: s.guid = bytes(k['AssociatedGuid']['value'])


def handlers(pkg):
    """(class FPackageIndex, handler FPackageIndex, [IchRecord]) for each Blueprint class with an
    InheritableComponentHandler tag."""
    for i, st in classes(pkg):
        h = obj_tag(pkg, i, 'InheritableComponentHandler')
        if not h: continue
        recs = []
        if h > 0 and pkg.class_of(h) == 'InheritableComponentHandler':
            t = pkg.tag(h - 1, 'Records')
            if t and t['type'] == 'ArrayProperty':
                recs = [IchRecord(pkg, h - 1, tl) for tl in struct_elements(pkg, h - 1, t)]
        yield i + 1, h, recs


# ---- classes across packages

_API = {}


def ueapi():
    """UeApi's reflected classes: {'/Script/Pkg.Name': parent path or None}, and {'/Script/Pkg.Name': {member:
    (subobject name, subobject class path)}} from each class's `<Member>__UeSubobject` markers (its CDO's default
    subobjects, read off the object dump)."""
    if _API: return _API['parent'], _API['subobjects']
    cpp, parent, subs, body = {}, {}, {}, {}
    decl = re.compile(r'^class (\w+)(?: : public (\w+)[^\n{]*)?\s*\{\s*public:\s*UE_CLASS\("([^"]+)",\s*"([^"]+)"\);(.*?)^\};',
                      re.M | re.S)
    for h in glob.glob(os.path.join(UEAPI, '*.h')):
        for m in decl.finditer(open(h, encoding='utf-8', errors='replace').read()):
            path = m.group(3) + '.' + m.group(4)
            cpp[m.group(1)] = path
            parent[path] = m.group(2)
            body[path] = m.group(5)
            subs[path] = {k: tuple(v.split(' ', 1)) for k, v in
                          re.findall(r'(\w+)__UeSubobject = "([^"]+)"', m.group(5))}
    for p, base in list(parent.items()): parent[p] = cpp.get(base) if base else None
    _API.update(parent=parent, subobjects=subs, body=body)
    return parent, subs


def native_member(path, name):
    """Whether UeApi's reflected class at `path` or one of its /Script parents declares a member `name`."""
    parent, _ = ueapi()
    while path:
        if re.search(r'\b%s\s*(?:\[\d+\])?\s*;' % re.escape(name), _API['body'].get(path, ''), re.I): return True
        path = parent.get(path)
    return False


_CHAINS = {}


def class_chain(pkg, idx):
    """The class at FPackageIndex idx and its supers, most derived first, as (package, export index or None, path):
    Blueprint classes by export in whatever package holds them (Package.resolve), then /Script classes by UeApi. The
    list ends with (None, None, None) where a super cannot be found."""
    key = (pkg.base, idx)
    if key in _CHAINS: return _CHAINS[key]
    out = []
    while idx:
        if idx < 0 and pkg.path(idx).startswith('/Script/'):
            parent, _ = ueapi()
            path = pkg.path(idx)
            if path not in parent: out.append((None, None, None)); break
            while path:
                out.append((None, None, path)); path = parent.get(path)
            break
        where = pkg.resolve(idx)
        if not where: out.append((None, None, None)); break
        p, k = where
        st = p.struct(k)
        out.append((p, k, p.path(k + 1)))
        if not st or not hasattr(st, 'class_flags'): out.append((None, None, None)); break
        pkg, idx = p, st.super
    _CHAINS[key] = out
    return out


def derives(pkg, idx, base):
    """True / False whether the class at FPackageIndex idx is `base` (a /Script path) or below it; None when its
    chain cannot be followed that far."""
    paths = [c[2] for c in class_chain(pkg, idx)]
    if base in paths: return True
    return None if paths and paths[-1] is None else False


def nearest_script(pkg, cls):
    """The path of the first /Script class in class export cls's chain, or None."""
    return next((path for p, k, path in class_chain(pkg, cls) if p is None and path), None)


def blueprint_ancestors(pkg, cls):
    """The Blueprint classes strictly above class export `cls`, nearest first, as (package, export index); stops at
    the first /Script class or an unresolvable super (then the list ends with None)."""
    out = []
    for p, k, path in class_chain(pkg, cls)[1:]:
        if p is None:
            if path is None: out.append(None)
            break
        out.append((p, k))
    return out


def own_scs(p, k):
    """The Scs of class export k of package p, or None."""
    return next((s for s in construction_scripts(p) if s.cls == k + 1), None)


def made_class(pkg, n):
    """The FPackageIndex of the class a node constructs: its ComponentTemplate export's class. None when the template
    is null (cooked out, so nothing is constructed) or an import."""
    return pkg.exports[n.template - 1]['cls'] if n.template > 0 else None


def is_scene(pkg, n):
    c = made_class(pkg, n)
    return derives(pkg, c, SCENE) if c else None


RF_Public, RF_Standalone, RF_ClassDefaultObject, RF_ArchetypeObject = 0x1, 0x2, 0x10, 0x20
RF_DefaultSubObject, RF_TextExportTransient, RF_InheritableComponentTemplate = 0x40000, 0x100000, 0x400000
RF_DuplicateTransient, RF_NonPIEDuplicateTransient = 0x800000, 0x2000000
RF_Load = 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x40000 | 0x100000 | 0x400000 | 0x800000 | 0x2000000
OBJECT_PROPERTIES = ('ObjectProperty', 'WeakObjectProperty', 'LazyObjectProperty', 'SoftObjectProperty')


def cdo_subobjects(p, k):
    """{lower-cased name: class FPackageIndex} of the default subobjects exported under class export k's CDO."""
    cdo = getattr(p.struct(k), 'cdo', 0)
    return {e['name'].lower(): e['cls'] for e in p.exports if cdo > 0 and e['outer'] == cdo and e['flags'] & RF_DefaultSubObject}


def native_components(pkg, cls):
    """({lower-cased name: whether it is a SceneComponent (True / False / None)} of the actor's native default
    subobjects - the nearest /Script class's UeApi markers, and those overridden under this class's CDO and its Blueprint
    ancestors' - and whether that set is complete: every class of the chain found)."""
    parent, subs = ueapi()
    out, complete = {}, True
    for p, k, path in class_chain(pkg, cls):
        if p is None:
            if path is None or path not in parent: complete = False
            else:
                for member, (obj, cpath) in subs.get(path, {}).items():
                    q = cpath
                    while q and q != SCENE: q = parent.get(q)
                    out[obj.lower()] = bool(q) if cpath in parent else None
            break
        for name, c in cdo_subobjects(p, k).items():
            out[name] = derives(p, c, SCENE)
    return out, complete


def root_before(pkg, s):
    """Whether the actor already has a RootComponent when this SCS runs: ancestors' SCSs run first
    (ActorConstruction.cpp:754), and before them the first unattached native scene component becomes the root (741).
    True, False, or None when the chain or the native class is unknown."""
    anc = blueprint_ancestors(pkg, s.cls)
    if any(a and own_scs(*a) for a in anc): return True
    if any(s.node(x) and s.node(x).parent_native and s.node(x).parent_name for x in s.roots): return True
    natives, complete = native_components(pkg, s.cls)
    if any(natives.values()): return True
    if None in anc or not complete or None in natives.values(): return None
    return False


def node_label(pkg, x):
    return pkg.exports[x - 1]['name'] if x > 0 else (pkg.path(x) or 'null')


def find_object_property(pkg, cls, name):
    """(package, Prop) of the FObjectPropertyBase FindFProperty<FObjectPropertyBase>(Class, Name) returns - the class's
    own properties first, then its supers', FName compared without case; (None, 'native') for a member a /Script
    ancestor declares; (None, 'none') when no class has it; (None, 'unknown') when the chain cannot be followed."""
    for p, k, path in class_chain(pkg, cls):
        if p is None:
            if path is None: return None, 'unknown'
            return (None, 'native') if native_member(path, name) else (None, 'none')
        for q in p.struct(k).props:
            if q.name.lower() == name.lower() and q.type in OBJECT_PROPERTIES: return p, q
    return None, 'none'


def is_cdo_of(pkg, tmpl, cls):
    """Whether FPackageIndex tmpl is the class default object of the class at FPackageIndex cls."""
    if not tmpl or not cls: return False
    o, c = pkg.obj(tmpl), pkg.obj(cls)
    if o['name'].lower() != ('Default__' + c['name']).lower(): return False
    if tmpl > 0: return o['cls'] == cls
    return cls < 0 and o['outer'] == c['outer'] and o['class_name'].lower() == c['name'].lower()


def ancestor_template(pkg, cls, name, klass):
    """What GetArchetype returns for an RF_InheritableComponentTemplate object `name` of class `klass` outered to class
    export `cls` (UObjectArchetype.cpp:88-108): the first Blueprint super holding an object of that name whose class
    IsA klass (StaticFindObjectFastInternal) - as (package, export index) - else None (then it is the class CDO).
    'unknown' when a super cannot be read."""
    want = pkg.path(klass)
    for a in blueprint_ancestors(pkg, cls):
        if a is None: return 'unknown'
        p, k = a
        for j, e in enumerate(p.exports):
            if e['outer'] == k + 1 and e['name'].lower() == name.lower() and want in [c[2] for c in class_chain(p, e['cls'])]:
                return p, j
    return None


def named_parent(pkg, s, n):
    """What a root node's ParentComponentOrVariableName names, as the engine resolves it: (found, scene, why) with
    found True / False / None (unknown) and scene whether that component is a SceneComponent."""
    if n.parent_native:
        natives, complete = native_components(pkg, s.cls)
        if n.parent_name.lower() in natives: return True, natives[n.parent_name.lower()], ''
        return (False if complete else None), None, (
            'bIsParentComponentNative names %s, no default subobject of %s' % (n.parent_name, nearest_script(pkg, s.cls)))
    own = pkg.exports[s.cls - 1]['name']
    if not n.parent_owner:
        return False, None, 'ParentComponentOrVariableName %s with no ParentComponentOwnerClassName' % n.parent_name
    if n.parent_owner.lower() == own.lower():
        return False, None, 'parent %s is a node of this same SCS (owner %s): only ancestors are searched' % (n.parent_name, own)
    for a in blueprint_ancestors(pkg, s.cls):
        if a is None: return None, None, ''
        p, k = a
        if p.exports[k]['name'].lower() != n.parent_owner.lower(): continue
        s2 = own_scs(p, k)
        hits = [s2.node(y) for y in s2.all if s2.node(y) and (s2.node(y).var or '').lower() == n.parent_name.lower()] if s2 else []
        if not hits: return False, None, '%s has no SCS node %s' % (n.parent_owner, n.parent_name)
        return True, is_scene(p, hits[0]), ''
    return False, None, 'ParentComponentOwnerClassName %s is no Blueprint ancestor of %s' % (n.parent_owner, own)


def reached(s):
    """Every node index an SCS executes or lists, nulls left out: those reachable from RootNodes, then AllNodes."""
    order, _ = s.walk()
    return [x for x in dict.fromkeys(order + s.all) if x]


# ---- the rules

@rule
def scs_ownership(pkg):
    """A Blueprint class's SimpleConstructionScript is an export outered to that class, and every node it lists
    (RootNodes, AllNodes, DefaultSceneRootNode, any listed node's ChildNodes) is an SCS_Node export outered to that SCS:
    USimpleConstructionScript::GetOwnerClass is Cast<UClass>(GetOuter()) (SimpleConstructionScript.cpp:762-777), null
    otherwise, so PostLoad's FixupRootNodeParentReferences skips every parent fixup (522-531) and FComponentKey takes a
    null OwnerClass, so no subclass override ever matches (InheritableComponentHandler.cpp:538-547); USCS_Node::GetSCS is
    CastChecked<USimpleConstructionScript>(GetOuter()) (SCS_Node.h:159-162), called on every instancing
    (SCS_Node.cpp:33, 60). The last clause is editor behaviour, not an engine requirement: an SCS_Node outered to the
    SCS that it does not list is simply never constructed - ExecuteScriptOnActor walks RootNodes
    (SimpleConstructionScript.cpp:645), FindSCSNode AllNodes (976-991) - and nothing fails. The editor never leaves
    one (the game keeps it on every package), so for a writer it means a component the class declares and drops."""
    for s in construction_scripts(pkg):
        if not s.is_export or pkg.class_of(s.idx) != 'SimpleConstructionScript':
            yield s.cls - 1, 'SimpleConstructionScript tag names %s, not a SimpleConstructionScript export' % pkg.path(s.idx)
            continue
        outer = pkg.exports[s.idx - 1]['outer']
        if outer != s.cls:
            yield s.idx - 1, 'the SCS is outered to %s, not to its class %s' % (pkg.path(outer) or 'the package', s.class_name())
        listed = set(s.listed())
        for x in sorted(listed):
            if x == 0: continue
            if x < 0: yield s.idx - 1, 'lists the import %s as a node' % pkg.path(x); continue
            if pkg.class_of(x) != 'SCS_Node': yield s.idx - 1, 'lists %s, a %s, as a node' % (node_label(pkg, x), pkg.class_of(x)); continue
            if pkg.exports[x - 1]['outer'] != s.idx:
                yield x - 1, 'listed by %s but outered to %s' % (pkg.exports[s.idx - 1]['name'], pkg.path(pkg.exports[x - 1]['outer']))
        for i, e in enumerate(pkg.exports):
            if e['outer'] == s.idx and pkg.class_of(i + 1) == 'SCS_Node' and i + 1 not in listed:
                yield i, 'an SCS_Node of %s that it neither lists nor names its DefaultSceneRootNode: never constructed' % s.class_name()


@rule
def scs_forest(pkg):
    """An SCS's nodes form a forest, and AllNodes is exactly the set reachable from RootNodes through ChildNodes, each
    once. A null RootNodes entry is dereferenced by PostLoad's FixupRootNodeParentReferences
    (SimpleConstructionScript.cpp:536-537), a null child fails check(Node) in ExecuteNodeOnActor (SCS_Node.cpp:196-197); a
    node reached twice is executed twice and re-creates a component of the same name under the actor (StaticAllocateObject
    destroys the live one in place), a cycle recurses without end. A cooked GetAllNodes returns AllNodes verbatim - the
    rebuild from RootNodes is for packages older than VER_UE4_SCS_STORES_ALLNODES_ARRAY only (282-296) - and
    FindSCSNode / FindSCSNodeByGuid (976-1003, archetype and override-key lookup) and the class's preload and
    fast-path data (BlueprintGeneratedClass.cpp:1481-1501) read it, so it must list what execution constructs: every
    spawned SCS component's GetArchetype goes through UBlueprintGeneratedClass::FindArchetype, which finds its template
    with FindSCSNode (BlueprintGeneratedClass.cpp:829-856), so a constructed node missing from AllNodes gets the
    component class CDO as its archetype. The DefaultSceneRootNode is special only as a root (skipped when the actor
    has one, 647-648); that it is never a child is editor behaviour (as a child it would simply be constructed), which
    the game keeps."""
    for s in construction_scripts(pkg):
        if not s.is_export: continue
        e = s.idx - 1
        if 0 in s.roots: yield e, 'RootNodes holds a null entry'
        order, again = s.walk()
        for x in order:
            n = s.node(x)
            if n and 0 in n.children: yield x - 1, 'ChildNodes holds a null entry'
            if n and s.dsr and s.dsr in n.children: yield x - 1, 'the DefaultSceneRootNode is one of its ChildNodes'
        for x in sorted(set(again)): yield e, 'node %s is reached twice from RootNodes' % node_label(pkg, x)
        if len(s.all) != len(set(s.all)): yield e, 'AllNodes lists a node twice'
        reach, all_ = set(order) - {0}, set(s.all)
        if reach != all_:
            yield e, 'only in AllNodes %s, only reachable from RootNodes %s' % (
                sorted(node_label(pkg, x) for x in all_ - reach) or '-', sorted(node_label(pkg, x) for x in reach - all_) or '-')


@rule
def scs_children_are_scene(pkg):
    """Only a SceneComponent node has ChildNodes or is named as a parent: ExecuteNodeOnActor hands a node's children
    its own component only when that is a scene component, else its own parent (SCS_Node.cpp:190-199), so the children
    of an ActorComponent node attach to its parent instead; and a root node's ParentComponentOrVariableName resolves to
    Cast<USceneComponent> of the named component (SimpleConstructionScript.cpp:659-686), null for any other, so the node
    attaches to the actor's root instead."""
    for s in construction_scripts(pkg):
        order, _ = s.walk()
        for x in order:
            n = s.node(x)
            if not n: continue
            if n.children and is_scene(pkg, n) is False:
                yield x - 1, '%s is a %s, not a SceneComponent, but has ChildNodes' % (n.var, pkg.path(made_class(pkg, n)))
            if n.parent_name and x in s.roots:
                found, scene, _ = named_parent(pkg, s, n)
                if found and scene is False:
                    yield x - 1, '%s names %s as its parent, which is not a SceneComponent' % (n.var, n.parent_name)


@rule
def scs_single_scene_root(pkg):
    """When an SCS runs on an actor that has no RootComponent yet - no native scene subobject, no Blueprint ancestor
    whose SCS ran first (ActorConstruction.cpp:731-762) - exactly one of its RootNodes is a parentless scene component
    (the DefaultSceneRootNode counts when listed): ExecuteScriptOnActor captures the null root once
    (SimpleConstructionScript.cpp:643), and every parentless scene node then takes ExecuteNodeOnActor's root branch and
    calls SetRootComponent (SCS_Node.cpp:122-140), so a second one replaces the first and leaves it unattached; with
    none, the actor ends construction without a root, since the fallback SceneComponent is made only when RootNodes is
    empty (SimpleConstructionScript.cpp:690-702, "Must have a root component at the end of SCS"). Neither fails
    loudly: the actor spawns with its components unattached or with no transform at all. The editor keeps its
    DefaultSceneRootNode in RootNodes exactly while no other scene node can be the root (ValidateSceneRootNodes,
    1115-1143), but that repair is WITH_EDITOR and never runs on cooked content. A node whose template was cooked out
    constructs nothing and does not count."""
    for s in construction_scripts(pkg):
        if not s.is_export or not s.roots or root_before(pkg, s) is not False: continue
        parentless, unknown = [], False
        for x in s.roots:
            n = s.node(x)
            if not n or not n.template or n.parent_name: continue
            sc = is_scene(pkg, n)
            if sc is None: unknown = True
            elif sc: parentless.append(n.var or node_label(pkg, x))
        if unknown: continue
        if len(parentless) > 1:
            yield s.idx - 1, '%d parentless scene roots %s on an actor with no root: each replaces the last' % (
                len(parentless), parentless)
        elif not parentless:
            yield s.idx - 1, 'RootNodes %s hold no parentless scene component and the actor has no root: it is left without one' % (
                [s.node(x).var if s.node(x) else node_label(pkg, x) for x in s.roots])


@rule
def scs_parent_fields(pkg):
    """A node's ParentComponentOrVariableName is read only on RootNodes (SimpleConstructionScript.cpp:537, 652), and
    names a component that exists, or PostLoad's FixupRootNodeParentReferences clears it with a Warning and the node
    attaches to the root (597-604): with bIsParentComponentNative, one of the actor's native default subobjects (matched
    by object name among the CDO's components at PostLoad, 549-556, and NativeSceneComponents at run time, 659-672);
    otherwise a node of a STRICT Blueprint ancestor named by ParentComponentOwnerClassName whose variable is that name
    (573-592, StackIndex > 0: a parent in the same SCS must be a ChildNodes entry instead)."""
    for s in construction_scripts(pkg):
        order, _ = s.walk()
        for x in order:
            n = s.node(x)
            if not n or not n.parent_name: continue
            if x not in s.roots:
                yield x - 1, '%s sets ParentComponentOrVariableName %s but is a child node' % (n.var, n.parent_name)
                continue
            found, _, why = named_parent(pkg, s, n)
            if found is False: yield x - 1, '%s: %s' % (n.var, why)


@rule
def scs_node_template(pkg):
    """A node's ComponentTemplate - when it has one: an editor-only component's is cooked out to null, and the node
    then constructs nothing (SCS_Node.cpp:97-100) - is a UActorComponent export outered to the SCS's class: the
    template is duplicated into the actor as a component (ActorConstruction.cpp:970-998), and a subclass's override of it
    finds its archetype only among objects outered to this class (FindObjectWithOuter(SuperClass, Class, Name),
    UObjectArchetype.cpp:88-108)."""
    for s in construction_scripts(pkg):
        for x in reached(s):
            n = s.node(x)
            if not n or not n.template: continue
            if n.template < 0:
                yield x - 1, '%s: ComponentTemplate is the import %s' % (n.var, pkg.path(n.template)); continue
            te = pkg.exports[n.template - 1]
            if te['outer'] != s.cls:
                yield n.template - 1, 'template of %s is outered to %s, not %s' % (n.var, pkg.path(te['outer']), s.class_name())
            if derives(pkg, te['cls'], ACTOR_COMPONENT) is False:
                yield n.template - 1, 'template of %s is a %s, not an ActorComponent' % (n.var, pkg.path(te['cls']))


@rule
def scs_component_template_flags(pkg):
    """A component template - an SCS node's, an InheritableComponentHandler record's - carries RF_Public and
    RF_ArchetypeObject: other packages import it (a subclass override's TemplateIndex) and FLinkerLoad refuses a private
    import (LinkerLoad.cpp:3412-3454; 5846-5871 marks BPGC component templates public for that reason), and IsTemplate()
    is RF_ArchetypeObject (UObjectBaseUtility.h:453). It carries none of RF_Standalone, RF_ClassDefaultObject,
    RF_TextExportTransient, RF_DuplicateTransient, RF_NonPIEDuplicateTransient: they survive the load mask
    (ObjectResource.cpp:121-126) and CreateComponentFromTemplate copies them onto every spawned component
    (ActorConstruction.cpp:993-998, 1056-1062). (RF_DefaultSubObject is left alone: DRG's BP_ScrabTank ships five SCS
    templates with it.) An override template also carries RF_InheritableComponentTemplate, the flag that makes
    GetArchetype search the class's supers (UObjectArchetype.cpp:88). That no other object does is editor behaviour
    (it sets the flag only there, InheritableComponentHandler.cpp:133-134, and SCS_Node::Serialize clears it off SCS
    templates, SCS_Node.cpp:462-467), kept because on any other object outered to a class it would silently move the
    delta base to a same-named object of a super. An SCS template's
    TemplateIndex is its class's CDO, what GetArchetype gives an object outered to a class without that flag
    (UObjectArchetype.cpp:120-127): the event-driven loader constructs it from TemplateIndex (AsyncLoading.cpp:2954-2964),
    so any other object would hand it that object's values."""
    bad = RF_Standalone | RF_ClassDefaultObject | RF_TextExportTransient | RF_DuplicateTransient | RF_NonPIEDuplicateTransient
    scs_t, ich_t = set(), set()
    for s in construction_scripts(pkg):
        for x in reached(s) + ([s.dsr] if s.dsr else []):
            n = s.node(x)
            if n and n.template > 0: scs_t.add(n.template)
    for cls, h, recs in handlers(pkg):
        ich_t |= {r.template for r in recs if r.template > 0}
    for t in sorted(scs_t | ich_t):
        e = pkg.exports[t - 1]
        f = e['flags'] & RF_Load
        need = RF_Public | RF_ArchetypeObject | (RF_InheritableComponentTemplate if t in ich_t else 0)
        if f & need != need: yield t - 1, 'flags %#x lack %#x' % (e['flags'], need & ~f)
        if f & bad: yield t - 1, 'flags %#x carry %#x' % (e['flags'], f & bad)
        if t in scs_t and t not in ich_t and not is_cdo_of(pkg, e['tmpl'], e['cls']):
            yield t - 1, 'SCS template archetype %s, not the CDO of %s' % (pkg.path(e['tmpl']), pkg.path(e['cls']))
    for i, e in enumerate(pkg.exports):
        if e['flags'] & RF_InheritableComponentTemplate and i + 1 not in scs_t | ich_t:
            yield i, 'RF_InheritableComponentTemplate on a %s no handler record names' % pkg.class_of(i + 1)


@rule
def scs_node_variable(pkg):
    """Each node that constructs a component has an InternalVariableName, unique among the actor's components - this
    SCS's nodes, its Blueprint ancestors' nodes, its native subobjects: CreateComponentFromTemplate names the instance
    after it (SCS_Node.cpp:99, ActorConstruction.cpp:985-998), and a second object of that name and outer replaces the
    first (StaticAllocateObject, UObjectGlobals.cpp:2396-2417, 2481-2492). When the actor class or one of its supers
    declares an object property of that name (FindFProperty<FObjectPropertyBase>, SCS_Node.cpp:159-179), the template's
    class is or derives from the property's class: otherwise the IsA check (167) skips the store and the variable
    reads None. That such a property exists at all is editor behaviour, not an engine requirement - without one the
    component is built and the store skipped with a Log line (176-178) - but the editor gives every node one
    (KismetCompiler.cpp:874-901) and the game keeps it on every node, so a missing one means a declared component whose
    variable never gets it. The DefaultSceneRootNode, which no source declares, is exempt from that clause, and does not
    construct at all when the actor already has a root (SimpleConstructionScript.cpp:647-648)."""
    def skipped(p, s, x):
        return x == s.dsr and x in s.roots and root_before(p, s) is True

    for s in construction_scripts(pkg):
        anc_names = {}
        for a in blueprint_ancestors(pkg, s.cls):
            if a is None: break
            s2 = own_scs(*a)
            for y in (reached(s2) if s2 else []):
                n2 = s2.node(y)
                if n2 and n2.var and n2.template and not skipped(a[0], s2, y): anc_names[n2.var.lower()] = a[0].exports[a[1]]['name']
        natives, _ = native_components(pkg, s.cls)
        seen = {}
        for x in reached(s):
            n = s.node(x)
            if not n or not n.template or skipped(pkg, s, x): continue
            if not n.var: yield x - 1, 'constructs a component but has no InternalVariableName'; continue
            key = n.var.lower()
            if key in seen: yield x - 1, 'variable %s is also node %s\'s' % (n.var, node_label(pkg, seen[key]))
            seen[key] = x
            if key in anc_names: yield x - 1, 'variable %s is also a node of %s' % (n.var, anc_names[key])
            if key in natives: yield x - 1, 'variable %s is also a native subobject of the actor' % n.var
            p, q = find_object_property(pkg, s.cls, n.var)
            if p is None:
                if q == 'none' and x != s.dsr: yield x - 1, 'no object property %s on %s or its supers' % (n.var, s.class_name())
                continue
            tc = made_class(pkg, n)
            if tc is None: continue
            chain = class_chain(pkg, tc)
            want = p.path(q.ref)
            if want and want.lower() not in [c[2].lower() for c in chain if c[2]] and chain[-1][2] is not None:
                yield x - 1, 'property %s holds a %s; the template is a %s' % (n.var, want, pkg.path(tc))


@rule
def scs_node_guid(pkg):
    """Every node an SCS constructs (AllNodes, and the DefaultSceneRootNode when it is a root) has a non-zero
    VariableGuid unique within that SCS: a subclass override record matches a node by (OwnerClass, AssociatedGuid)
    (FComponentKey::Match, InheritableComponentHandler.cpp:557-560), FindSCSNodeByGuid returns the first node of a guid
    (SimpleConstructionScript.cpp:993-1003), and a zero guid makes the key invalid for FindArchetype. The editor's
    ValidateGuid, which would fill one in, is WITH_EDITOR (SCS_Node.cpp:472-479)."""
    for s in construction_scripts(pkg):
        seen = {}
        for x in dict.fromkeys(s.all + ([s.dsr] if s.dsr in s.roots else [])):
            n = s.node(x)
            if not n: continue
            if not n.guid or n.guid == bytes(16): yield x - 1, '%s has no VariableGuid' % n.var; continue
            if n.guid in seen: yield x - 1, '%s shares its VariableGuid with %s' % (n.var, node_label(pkg, seen[n.guid]))
            seen[n.guid] = x


@rule
def scs_ich_records(pkg):
    """Each InheritableComponentHandler record overrides an existing node of a STRICT Blueprint ancestor: its
    ComponentKey (OwnerClass, AssociatedGuid) is what GetActualComponentTemplate matches while it walks the actor's
    classes from the most derived up to the owner (SCS_Node.cpp:27-52, FComponentKey::Match), so a key that names no
    ancestor's node, or a second record of one key (FindRecord returns the first, InheritableComponentHandler.cpp:495-505),
    silently drops the override; SCSVariableName is that node's variable, what FindKey matches by name (471-481). A
    non-null ComponentTemplate is outered to the class, named as the owner node's template (ICH- prefixed for an override
    of the owner's DefaultSceneRootNode, the editor's naming at 104-112): GetArchetype finds an override's delta base
    only by that name among the supers' objects (UObjectArchetype.cpp:88-108), and any other name makes it the class
    CDO. Its TemplateIndex is what GetArchetype gives it - the nearest Blueprint super's object of that name and class,
    else the class CDO - as the cooker writes it (SavePackage.cpp:3815-3819). The event-driven loader constructs it from
    TemplateIndex (AsyncLoading.cpp:2954-2964), so any other object gives it the wrong inherited values - a
    grandparent's template skips the parent's own override of the component."""
    for cls, h, recs in handlers(pkg):
        if h <= 0 or pkg.class_of(h) != 'InheritableComponentHandler':
            yield cls - 1, 'InheritableComponentHandler tag names %s' % pkg.path(h); continue
        anc = blueprint_ancestors(pkg, cls)
        known = None not in anc
        keys, names = set(), set()
        for r in recs:
            if not r.owner: yield h - 1, 'a record without an OwnerClass'; continue
            opath = pkg.path(r.owner)
            owner = next(((p, k) for p, k in (a for a in anc if a) if p.path(k + 1).lower() == opath.lower()), None)
            if owner is None and known:
                yield h - 1, 'record %s: %s is not a Blueprint ancestor of %s' % (r.var, opath, pkg.exports[cls - 1]['name'])
            k1, k2 = (opath.lower(), r.guid), (opath.lower(), (r.var or '').lower())
            if k1 in keys or k2 in names: yield h - 1, 'two records for %s.%s' % (opath, r.var)
            keys.add(k1); names.add(k2)
            node = s2 = None
            if owner:
                s2 = own_scs(*owner)
                cands = [s2.node(y) for y in dict.fromkeys(reached(s2) + ([s2.dsr] if s2.dsr else []))] if s2 else []
                node = next((c for c in cands if c and c.guid == r.guid), None)
                if node is None:
                    yield h - 1, 'record %s: no node of %s has its AssociatedGuid' % (r.var, opath)
                elif (node.var or '').lower() != (r.var or '').lower():
                    yield h - 1, 'record SCSVariableName %s, the node of that guid is %s' % (r.var, node.var)
            if not r.template: continue
            if r.template < 0: yield h - 1, 'record %s: ComponentTemplate is the import %s' % (r.var, pkg.path(r.template)); continue
            te = pkg.exports[r.template - 1]
            if te['outer'] != cls: yield r.template - 1, 'override template outered to %s' % pkg.path(te['outer'])
            if node and node.template:
                expect = ('ICH-' if node.idx == s2.dsr else '') + node_label(owner[0], node.template)
                if te['name'].lower() != expect.lower():
                    yield r.template - 1, 'override of %s is named %s, the owner\'s template %s' % (r.var, te['name'], expect)
            arch = ancestor_template(pkg, cls, te['name'], te['cls'])
            if arch == 'unknown': continue
            got = pkg.path(te['tmpl']) if te['tmpl'] else None
            if arch is None:
                if not is_cdo_of(pkg, te['tmpl'], te['cls']):
                    yield r.template - 1, 'TemplateIndex %s; no super holds a %s, so its archetype is the class CDO' % (got, te['name'])
            elif (got or '').lower() != arch[0].path(arch[1] + 1).lower():
                yield r.template - 1, 'TemplateIndex %s; GetArchetype gives %s' % (got, arch[0].path(arch[1] + 1))


@rule
def scs_template_creation_method(pkg):
    """No component template (an SCS node's or an override record's) serializes CreationMethod Instance, and no
    ActorComponent default subobject of a CDO serializes any CreationMethod but Native. A component is constructed
    from its archetype before its own values are copied in (StaticDuplicateObject, UObjectGlobals.cpp:2055; a native
    subobject from the CDO's, UObjectGlobals.cpp:2820-2830), and UActorComponent::PostInitProperties skips
    AddOwnedComponent for Instance (ActorComponent.cpp:282-287): a component built on such an archetype - an actor's
    native subobject, a subclass override's instance - is missing from GetComponents. A template's own spawned instance
    is unaffected, since it is built on the template's archetype and ExecuteNodeOnActor then overwrites CreationMethod
    (SCS_Node.cpp:104); but any SCS template can become a subclass override's archetype, in this package or another.
    A native subobject keeps what the CDO's has: UserConstructionScript makes it unaddressable by name over the network
    (IsNameStableForNetworking, ActorComponent.cpp:1912), and any non-Native value drops it from the native scene
    components an SCS node can name as its parent or that can become the root (ActorConstruction.cpp:737-747). The
    game's editor writes no CreationMethod on any CDO subobject."""
    targets, dsos = set(), set()
    for s in construction_scripts(pkg):
        for x in reached(s) + ([s.dsr] if s.dsr else []):
            n = s.node(x)
            if n and n.template > 0: targets.add(n.template)
    for cls, h, recs in handlers(pkg):
        targets |= {r.template for r in recs if r.template > 0}
    for i, e in enumerate(pkg.exports):
        if (e['flags'] & RF_DefaultSubObject and e['outer'] > 0 and pkg.exports[e['outer'] - 1]['flags'] & RF_ClassDefaultObject
                and derives(pkg, e['cls'], ACTOR_COMPONENT)):
            dsos.add(i + 1)
    for t in sorted(targets | dsos):
        cm = enum_tag(pkg, t - 1, 'CreationMethod')
        if cm is None: continue
        if cm == 3 or str(cm).endswith('::Instance'):
            yield t - 1, 'CreationMethod %s' % cm
        elif t in dsos and cm != 0 and not str(cm).endswith('::Native'):
            yield t - 1, 'CreationMethod %s on a default subobject of %s' % (cm, pkg.exports[pkg.exports[t - 1]['outer'] - 1]['name'])


