import sys
from invariants import *
# COMP: components, the construction script and ticking, as the engine builds an actor of a cooked Blueprint class.
#
# The SCS (SimpleConstructionScript) and ICH (InheritableComponentHandler) rules follow a class's /Game parents through
# Package.resolve, so a package whose parent is not found - neither beside it nor in GAME_CONTENT - is checked on its own
# nodes only. Native default subobjects are read off UeApi's `<Member>__UeSubobject` markers (the object dump's names),
# when a UeApi folder is found; without one the native half of those checks is skipped rather than guessed.
#
# For the integrator: four of these overlap the SCS package's rules on the same engine code - scs_names_distinct with
# scs_node_variable, scs_parent_reference_resolves with scs_parent_fields, scs_node_guids_unique with scs_node_guid,
# ich_records_match_ancestor_nodes + ich_template_archetype_nearest with scs_ich_records. Keep one of each pair; these
# add timelines and CDO overrides to the name clash, and the scene-component test to a native parent.
import os, re, struct

RF_Public, RF_ArchetypeObject, RF_DefaultSubObject, RF_InheritableComponentTemplate = 0x1, 0x20, 0x40000, 0x400000
FUNC_Native, FUNC_Event, FUNC_BlueprintEvent = 0x400, 0x800, 0x08000000


# ---- reading tags

def _i32(v, o=0): return struct.unpack_from('<i', v, o)[0]


def tag_name(pkg, t):
    """A NameProperty tag's value, with its number as the engine prints it (Timeline#1 -> Timeline_0)."""
    i, n = struct.unpack_from('<ii', t['value'])
    return pkg.names[i] + ('_%d' % (n - 1) if n else '')


def tag_objects(t):
    """An ArrayProperty of ObjectProperty: its FPackageIndexes."""
    v = t['value'] if t else b'\0\0\0\0'
    return [_i32(v, 4 + 4 * k) for k in range(_i32(v))]


def tags_at(pkg, i, start=0):
    """{name: tag} of the tag list at `start` of export i (a struct value's tags sit at its tag's 'at')."""
    return {t['name']: t for t in pkg.tags(i, start)}


def struct_elements(pkg, i, t):
    """The elements of an ArrayProperty of StructProperty, each as {name: tag}: after the count comes one inner tag
    header (name, type, size, index, struct name, struct guid, HasPropertyGuid [+ guid]), then the elements."""
    b, n = pkg.blob(i), _i32(t['value'])
    off = t['at'] + 4 + 8 + 8 + 4 + 4 + 8 + 16
    off += 1 + (16 if b[off] else 0)
    out = []
    for _ in range(n):
        tl = pkg.tags(i, off)
        out.append({x['name']: x for x in tl})
        off = tl.end
    return out


def map_name_keys(pkg, i, t):
    """The FName keys of a MapProperty keyed by NameProperty (CookedComponentInstancingData): NumKeysToRemove (and
    those keys), Num, then each key and its value - a tagged struct here, walked over to reach the next key."""
    b, off = pkg.blob(i), t['at']
    off += 4 + 8 * _i32(b, off)
    count = _i32(b, off); off += 4
    keys = []
    for _ in range(count):
        idx, num = struct.unpack_from('<ii', b, off); off += 8
        keys.append(pkg.names[idx] + ('_%d' % (num - 1) if num else ''))
        off = pkg.tags(i, off).end
    return keys


def bool_in(pkg, i, t, member):
    """A BoolProperty inside a StructProperty tag's value, or None if the struct writes no tag for it."""
    x = tags_at(pkg, i, t['at']).get(member)
    return None if x is None else x['bool']


# ---- UeApi: the native classes' bases and default subobjects

def _ueapi_dirs():
    here = os.path.dirname(os.path.abspath(sys.modules['invariants'].__file__))
    return [UEAPI_DIR]


_NATIVE = None


def native_classes():
    """{'/Script/Engine.Character': (base path or None, {member: (subobject name, class path)})} from UeApi, the
    first folder holding Engine.h; {} without one."""
    global _NATIVE
    if _NATIVE is not None: return _NATIVE
    _NATIVE = {}
    folder = next((d for d in _ueapi_dirs() if os.path.exists(os.path.join(d, 'Engine.h'))), None)
    if not folder: return _NATIVE
    decl = re.compile(r'^class (\w+)(?: : public (\w+))?\s*\{\s*public:\s*UE_CLASS\("(/Script/[^"]+)", "([^"]+)"\);', re.M)
    sub = re.compile(r'static constexpr const char\* (\w+)__UeSubobject = "(\S+) (\S+)";')
    cpp = {}
    for f in sorted(os.listdir(folder)):
        if not f.endswith('.h'): continue
        text = open(os.path.join(folder, f), encoding='utf-8', errors='replace').read()
        heads = list(decl.finditer(text))
        for k, m in enumerate(heads):
            body = text[m.end(): heads[k + 1].start() if k + 1 < len(heads) else len(text)]
            body = body[:body.find('\n};')] if '\n};' in body else body
            cpp[m.group(1)] = (m.group(2), m.group(3) + '.' + m.group(4),
                               {s.group(1): (s.group(2), s.group(3)) for s in sub.finditer(body)})
    for name, (base, path, subs) in cpp.items():
        _NATIVE[path] = (cpp[base][1] if base in cpp else None, subs)
    return _NATIVE


def native_subobjects(path):
    """{subobject name: class path} of a native class and its native supers, per UeApi; None if UeApi does not know
    the class (then nothing can be said about its subobjects)."""
    table = native_classes()
    if path not in table: return None
    out = {}
    while path in table:
        base, subs = table[path]
        for member, (name, cls) in subs.items(): out.setdefault(name, cls)
        path = base
    return out


def native_is_scene(path):
    """Whether a native component class is a USceneComponent, per UeApi's bases; None when UeApi does not know it."""
    table, seen = native_classes(), set()
    while path is not None and path not in seen:
        if path == '/Script/Engine.SceneComponent': return True
        if path == '/Script/Engine.ActorComponent': return False
        if path not in table: return None
        seen.add(path)
        path = table[path][0]
    return None


def native_root(path):
    """The name of the native class's RootComponent default subobject (UeApi's RootComponent__UeSubobject), or None."""
    table = native_classes()
    while path in table:
        base, subs = table[path]
        if 'RootComponent' in subs: return subs['RootComponent'][0]
        path = base
    return None


# ---- a Blueprint class, its parents and its SCS

def bp_class(pkg):
    """The export index of the package's Blueprint class, or None."""
    return next((i for i, st in classes(pkg)), None)


def resolve(pkg, idx):
    """Package.resolve, None for a package outside any Content folder (a scratch build)."""
    try: return pkg.resolve(idx)
    except ValueError: return None


def chain(pkg, ci):
    """[(pkg, class export index), ...] from this class up through every /Game parent found, and the path of the
    first native class above them (None when a /Game parent is missing, so the native end is unknown)."""
    out, seen = [(pkg, ci)], set()
    p, i = pkg, ci
    while True:
        sup = p.struct(i).super
        if sup == 0: return out, None
        path = p.path(sup)
        if sup < 0 and path.startswith('/Script/'): return out, path
        r = resolve(p, sup)
        if not r or path in seen: return out, None
        seen.add(path)
        p, i = r
        if not p.struct(i) or not hasattr(p.struct(i), 'class_flags'): return out, None
        out.append((p, i))


class ScsNode:
    def __repr__(s): return 'ScsNode(%s)' % s.name


def scs_node(pkg, ni):
    """One SCS_Node export's tags: name (InternalVariableName), guid (16 bytes, zeros when absent), children, template,
    cls, parent (ParentComponentOrVariableName), owner (ParentComponentOwnerClassName), native."""
    nt, n = tags_at(pkg, ni), ScsNode()
    n.index = ni
    n.name = tag_name(pkg, nt['InternalVariableName']) if 'InternalVariableName' in nt else 'None'
    n.guid = nt['VariableGuid']['value'] if 'VariableGuid' in nt else bytes(16)
    n.children = [x - 1 for x in tag_objects(nt.get('ChildNodes')) if x > 0]
    n.template = _i32(nt['ComponentTemplate']['value']) if 'ComponentTemplate' in nt else 0
    n.cls = _i32(nt['ComponentClass']['value']) if 'ComponentClass' in nt else 0
    n.parent = tag_name(pkg, nt['ParentComponentOrVariableName']) if 'ParentComponentOrVariableName' in nt else 'None'
    n.owner = tag_name(pkg, nt['ParentComponentOwnerClassName']) if 'ParentComponentOwnerClassName' in nt else 'None'
    n.native = bool(nt['bIsParentComponentNative']['bool']) if 'bIsParentComponentNative' in nt else False
    return n


def scs(pkg, ci):
    """The class's SCS: (export index, {node export index: ScsNode} for every node it lists - RootNodes, AllNodes,
    DefaultSceneRootNode and their ChildNodes -, root node indexes, DefaultSceneRootNode index or None), or None."""
    t = pkg.tag(ci, 'SimpleConstructionScript')
    if not t or _i32(t['value']) <= 0: return None
    si = _i32(t['value']) - 1
    top = tags_at(pkg, si)
    roots = [x - 1 for x in tag_objects(top.get('RootNodes')) if x > 0]
    allnodes = [x - 1 for x in tag_objects(top.get('AllNodes')) if x > 0]
    dsr = _i32(top['DefaultSceneRootNode']['value']) - 1 if 'DefaultSceneRootNode' in top else None
    if dsr is not None and dsr < 0: dsr = None
    nodes, todo = {}, roots + allnodes + ([dsr] if dsr is not None else [])
    while todo:
        k = todo.pop()
        if k in nodes: continue
        nodes[k] = scs_node(pkg, k)
        todo += nodes[k].children
    return si, nodes, roots, dsr


def executed(pkg, ci, root_exists):
    """The SCS nodes ExecuteScriptOnActor builds for this class: RootNodes and their ChildNodes, recursively, minus the
    DefaultSceneRootNode when the actor already has a root (SimpleConstructionScript.cpp 648). [] without an SCS."""
    s = scs(pkg, ci)
    if not s: return []
    si, nodes, roots, dsr = s
    out, todo, seen = [], [r for r in roots if not (r == dsr and root_exists)], set()
    while todo:
        k = todo.pop(0)
        if k in seen or k not in nodes: continue
        seen.add(k)
        out.append(nodes[k])
        todo += nodes[k].children
    return out


def cdo_subobjects(pkg, ci):
    """Names of the default subobjects exported under the class's CDO (a native parent's component this class overrides)."""
    cdo = pkg.struct(ci).cdo
    return {e['name'] for e in pkg.exports if e['outer'] == cdo and e['flags'] & RF_DefaultSubObject}


def timelines(pkg, ci):
    """VariableName of each TimelineTemplate the class lists (BlueprintGeneratedClass.cpp 1087: its component's name)."""
    out = []
    for x in tag_objects(pkg.tag(ci, 'Timelines')):
        if x > 0:
            t = tags_at(pkg, x - 1).get('VariableName')
            if t: out.append(tag_name(pkg, t))
    return out


def hierarchy(pkg, ci):
    """Everything construction names on an actor of this class, oldest class first: [(pkg, class index, [executed
    ScsNode], [timeline names])], the native end's path and its default subobject names (None if unknown)."""
    links, native = chain(pkg, ci)
    subs = native_subobjects(native) if native else None
    root = bool(native and native_root(native))
    out = []
    for p, i in reversed(links):
        nodes = executed(p, i, root)
        out.append((p, i, nodes, timelines(p, i)))
        root = root or bool(nodes)     # a class that built nodes built a root (ponytail: assumes one is a scene component)
    return out, native, subs


# ---- the rules

@rule
def scs_names_distinct(pkg):
    """Each SCS node this class's construction runs has an InternalVariableName, and no other component the actor gets
    has that name: another executed node of this class or an ancestor Blueprint, a timeline (NewObject<UTimelineComponent>
    (Actor, VariableName), BlueprintGeneratedClass.cpp 1087), a default subobject of the native parent, or one this class
    overrides under its CDO. The instance is duplicated from its template under that name (AActor::
    CreateComponentFromTemplate, ActorConstruction.cpp 970), and CheckComponentInstanceName (1214) renames only an
    Instance-created clash: StaticAllocateObject (UObjectGlobals.cpp 2396-2417) then Fatals on an object of another
    class, or destroys and reconstructs the earlier component in place. A None name gets no variable and no ICH key
    (SCS_Node.cpp 159). Names compare as FNames do, ignoring case."""
    ci = bp_class(pkg)
    if ci is None: return
    levels, native, subs = hierarchy(pkg, ci)
    taken = {}                                          # lower-cased name -> what holds it
    for name in (subs or {}): taken[name.lower()] = 'the native %s\'s default subobject' % native
    for p, i, nodes, tls in levels:
        for name in cdo_subobjects(p, i): taken.setdefault(name.lower(), 'a default subobject under %s\'s CDO' % p.exports[i]['name'])
        mine = p is pkg and i == ci
        where = 'this class' if mine else p.exports[i]['name']
        for n in nodes:
            label = p.exports[n.index]['name']
            if mine and n.name == 'None': yield n.index, 'SCS node %s has no InternalVariableName' % label
            elif mine and n.name.lower() in taken: yield n.index, 'SCS node %s is named %s, as %s' % (label, n.name, taken[n.name.lower()])
            taken.setdefault(n.name.lower(), 'an SCS node of ' + where)
        for name in tls:
            if mine and name.lower() in taken: yield ci, 'timeline %s has the name of %s' % (name, taken[name.lower()])
            taken.setdefault(name.lower(), 'a timeline of ' + where)


@rule
def scs_parent_reference_resolves(pkg):
    """A root SCS node that names a parent component names one the loaded class has. bIsParentComponentNative: the
    name of a native default subobject that is a scene component - SCS PostLoad (FixupRootNodeParentReferences,
    SimpleConstructionScript.cpp 542-563) looks the name up among the CDO's components, and ExecuteScriptOnActor
    (659-672) among the actor's native SCENE components only (ActorConstruction.cpp 735-748), so a DSO that is no
    scene component passes the first and still leaves the node without a parent. Otherwise ParentComponentOwnerClassName
    is a STRICT ancestor Blueprint class whose SCS has a node of that variable name (566-593: StackIndex > 0 skips the
    class itself; a parent in the same SCS is a ChildNodes entry). An unmatched reference is cleared with a Warning
    (596-604), and an unmatched native one at run time finds no parent: either way the component hangs off the actor's
    root instead (686)."""
    ci = bp_class(pkg)
    if ci is None: return
    s = scs(pkg, ci)
    if not s: return
    si, nodes, roots, dsr = s
    links, native = chain(pkg, ci)
    for r in roots:
        n = nodes.get(r)
        if not n or n.parent == 'None': continue
        label = pkg.exports[r]['name']
        if n.native:
            subs = native_subobjects(native) if native else None
            known = {x.lower() for x in set(subs or ()) | cdo_subobjects(pkg, ci)}
            if subs is None and n.parent.lower() not in known: continue     # UeApi does not know the native parent
            if n.parent.lower() not in known:
                yield r, 'SCS node %s hangs off native %s, which %s does not have' % (label, n.parent, native)
            else:
                cls = next((c for x, c in (subs or {}).items() if x.lower() == n.parent.lower()), None)
                if cls and native_is_scene(cls) is False:
                    yield r, 'SCS node %s hangs off native %s, a %s: no scene component' % (label, n.parent, cls)
            continue
        owners = [(p, i) for p, i in links[1:] if p.exports[i]['name'].lower() == n.owner.lower()]
        if n.owner.lower() == pkg.exports[ci]['name'].lower():
            yield r, 'SCS node %s names its own class %s as its parent\'s owner (a ChildNodes entry is meant)' % (label, n.owner)
        elif not owners:
            if native: yield r, 'SCS node %s: parent owner %s is not an ancestor Blueprint' % (label, n.owner)
        else:
            p, i = owners[0]
            other = scs(p, i)
            if not other or not any(x.name.lower() == n.parent.lower() for x in other[1].values()):
                yield r, 'SCS node %s: %s has no SCS node %s' % (label, n.owner, n.parent)


@rule
def scs_node_guids_unique(pkg):
    """Every SCS node construction runs has a VariableGuid unique in its SCS: a subclass's override record is matched to
    a node by (OwnerClass, AssociatedGuid) alone (FComponentKey::Match, InheritableComponentHandler.cpp 557-560, called
    for every node by GetActualComponentTemplate, SCS_Node.cpp 27-52), so two nodes of one guid are BOTH built from the
    record meant for one of them; FindSCSNodeByGuid takes the first (SimpleConstructionScript.cpp 993-1003). And it is
    non-zero: the editor always assigns one (ValidateGuid, WITH_EDITOR), and a zero guid makes the key !IsValid
    (InheritableComponentHandler.h 44-47), which FindArchetype's handler path skips for a level-placed instance of a
    subclass. The game keeps both on every node."""
    ci = bp_class(pkg)
    if ci is None: return
    seen = {}
    for n in executed(pkg, ci, False):
        label = pkg.exports[n.index]['name']
        if n.guid == bytes(16): yield n.index, 'SCS node %s (%s) has no VariableGuid' % (label, n.name)
        elif n.guid in seen: yield n.index, 'SCS node %s has the VariableGuid of %s' % (label, seen[n.guid])
        else: seen[n.guid] = label


def ich_records(pkg, ci):
    """[(record index, {tag: ...}, {ComponentKey tag: ...})] of the class's InheritableComponentHandler, and its export."""
    t = pkg.tag(ci, 'InheritableComponentHandler')
    if not t or _i32(t['value']) <= 0: return None, []
    hi = _i32(t['value']) - 1
    rec = pkg.tag(hi, 'Records')
    if not rec: return hi, []
    out = []
    for k, el in enumerate(struct_elements(pkg, hi, rec)):
        key = tags_at(pkg, hi, el['ComponentKey']['at']) if 'ComponentKey' in el else {}
        out.append((k, el, key))
    return hi, out


@rule
def ich_records_match_ancestor_nodes(pkg):
    """Each InheritableComponentHandler record's key names a node of a STRICT ancestor Blueprint: OwnerClass is that
    class, AssociatedGuid its node's VariableGuid (what USCS_Node::GetActualComponentTemplate matches, FComponentKey::
    Match, InheritableComponentHandler.cpp 538-560) and SCSVariableName its InternalVariableName (what FindArchetype's
    FindKey matches, 471-505). A key that matches nothing is never found - the parent's template is used, the override
    silently lost - and of two records with one key the first shadows the other (FindRecord)."""
    ci = bp_class(pkg)
    if ci is None: return
    hi, records = ich_records(pkg, ci)
    if hi is None: return
    links, native = chain(pkg, ci)
    seen = set()
    for k, el, key in records:
        owner = _i32(key['OwnerClass']['value']) if 'OwnerClass' in key else 0
        guid = key['AssociatedGuid']['value'] if 'AssociatedGuid' in key else bytes(16)
        var = tag_name(pkg, key['SCSVariableName']) if 'SCSVariableName' in key else 'None'
        opath = pkg.path(owner) if owner else None
        if (opath, guid) in seen: yield hi, 'record %d repeats the key (%s, %s)' % (k, opath, guid.hex())
        seen.add((opath, guid))
        if owner == 0 or guid == bytes(16):
            yield hi, 'record %d has an invalid key (OwnerClass %s, guid %s)' % (k, opath, guid.hex()); continue
        if owner > 0 and owner - 1 == ci:
            yield hi, 'record %d is keyed on this class itself' % k; continue
        r = resolve(pkg, owner)
        if not r: continue                                      # the owner's package is not here: nothing to compare
        p, i = r
        if not any(p is lp and i == li for lp, li in links[1:]):
            yield hi, 'record %d: OwnerClass %s is not an ancestor' % (k, opath); continue
        other = scs(p, i)
        node = next((n for n in (other[1].values() if other else ()) if n.guid == guid), None)
        if not node: yield hi, 'record %d: %s has no SCS node with guid %s' % (k, opath, guid.hex())
        elif node.name.lower() != var.lower():          # FName ==, case-insensitive (ENE_Shredder's SpawnPArticles)
            yield hi, 'record %d: SCSVariableName %s, the node is %s' % (k, var, node.name)


@rule
def ich_template_archetype_nearest(pkg):
    """An override record's ComponentTemplate is an export of this class (outer = the BPGC) flagged
    RF_InheritableComponentTemplate - the flag that makes GetArchetype search the class's Blueprint supers
    (UObjectArchetype.cpp 88) -, RF_ArchetypeObject (IsTemplate, UObjectBaseUtility.h 453) and RF_Public (a grandchild
    imports it as its own TemplateIndex, and the linker refuses a private import, LinkerLoad.cpp 3412-3454). Its name,
    class and TemplateIndex are the NEAREST ancestor's template of that name: GetArchetype searches the supers in order
    for FindObjectWithOuter(super, Class, Name) (UObjectArchetype.cpp 83-108; the editor creates the override with its
    archetype's class and name, InheritableComponentHandler.cpp 96-104), and the event-driven loader constructs the
    export from its TemplateIndex (AsyncLoading.cpp 2954-2964), copying that object's values. A TemplateIndex that
    skips a parent's own override loses that parent's values wherever this class writes no tag. An inherited
    DefaultSceneRoot's override is ICH-<name>, which no ancestor has, so its archetype is the component class's CDO
    (InheritableComponentHandler.cpp 104-112)."""
    ci = bp_class(pkg)
    if ci is None: return
    hi, records = ich_records(pkg, ci)
    if hi is None: return
    links, native = chain(pkg, ci)
    for k, el, key in records:
        ti = _i32(el['ComponentTemplate']['value']) if 'ComponentTemplate' in el else 0
        if ti == 0: continue        # an editor-only component the cook stripped (BP_Azure_MagicCrystal_A's PreviewMesh)
        if ti < 0: yield hi, 'record %d\'s template is an import' % k; continue
        e = pkg.exports[ti - 1]
        if e['outer'] != ci + 1: yield ti - 1, 'override template %s is not outered to the class' % e['name']
        want_flags = RF_Public | RF_ArchetypeObject | RF_InheritableComponentTemplate
        if e['flags'] & want_flags != want_flags:
            yield ti - 1, 'override template %s flags %#x lack Public|ArchetypeObject|InheritableComponentTemplate' % (e['name'], e['flags'])
        want = None
        for p, i in links[1:]:
            j = next((j for j, x in enumerate(p.exports) if x['outer'] == i + 1 and x['name'].lower() == e['name'].lower()), None)
            if j is not None: want = (p, j); break
        got = pkg.path(e['tmpl']) if e['tmpl'] else None
        if want:
            p, j = want
            if got is None or got.lower() != p.path(j + 1).lower():
                yield ti - 1, 'override template %s is archetyped on %s; the nearest ancestor template is %s' % (e['name'], got, p.path(j + 1))
            elif pkg.class_of(e['cls']) != p.class_of(p.exports[j]['cls']):
                yield ti - 1, 'override template %s is a %s, its archetype a %s' % (e['name'], pkg.class_of(e['cls']), p.class_of(p.exports[j]['cls']))
        elif len(links) > 1 and all(p.struct(i) for p, i in links[1:]) and native is not None:
            if not e['name'].startswith('ICH-'):
                yield ti - 1, 'override template %s: no ancestor has a template of that name' % e['name']


def ucs_function(pkg, ci):
    return next((i for i, st in functions(pkg) if pkg.exports[i]['name'] == 'UserConstructionScript'
                 and pkg.exports[i]['outer'] == ci + 1), None)


@rule
def ucs_takes_no_parameters(pkg):
    """A Blueprint class's UserConstructionScript takes no parameters and is not FUNC_Native. AActor::
    ProcessUserConstructionScript calls AActor's BlueprintImplementableEvent (Actor.h 1676, ActorConstruction.cpp
    884-889), whose thunk finds the most derived function of that name and ProcessEvents it with no parameter buffer:
    ProcessEvent copies ParmsSize bytes from that null pointer (ScriptCore.cpp 1958). A function loaded with FUNC_Native
    is bound to the class's native lookup table, which a Blueprint class has no entry in, instead of ProcessInternal
    (UFunction::Bind, Class.cpp 5762-5781), so the call reaches no script. (The editor also gives it AActor's Event |
    BlueprintEvent flags, and the game keeps them, but dispatch by name reads neither, so they are not checked.)"""
    for i, st in functions(pkg):
        e = pkg.exports[i]
        if e['name'] != 'UserConstructionScript' or e['outer'] <= 0: continue
        if not hasattr(pkg.struct(e['outer'] - 1), 'class_flags'): continue
        parms = [p.name for p in st.props if p.flags & CPF_Parm]
        if parms: yield i, 'UserConstructionScript takes %s' % parms
        if st.function_flags & FUNC_Native:
            yield i, 'UserConstructionScript FunctionFlags %#x: FUNC_Native, bound to no script' % st.function_flags


UNSAFE_DURING_CONSTRUCTION = {                  # GameplayStatics.h 58-71: meta UnsafeDuringActorConstruction
    '/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass',
    '/Script/Engine.GameplayStatics:BeginSpawningActorFromClass',
    '/Script/Engine.GameplayStatics:BeginSpawningActorFromBlueprint',
    '/Script/Engine.GameplayStatics:FinishSpawningActor',
}


def calls(pkg, i):
    """(node, function path or name) of every call in export i's script."""
    for n in statements(pkg, i)[0]:
        if n.op in (0x1B, 0x1C, 0x45, 0x46, 0x68) and n.ops:
            kind, v = n.ops[0][:2]
            yield n, (pkg.path(v) if kind == 'obj' else v)


FUNC_UbergraphFunction = 0x8000


def reached_function(p, f, n, mine, links):
    """(package, export index) of the Blueprint function a call in p's function f runs, or None: a final call the
    export or /Game import it names (Package.resolve), a virtual call by name on self the most derived function of that
    name up the actor's class chain `links` (own class first, as FindFunction looks it up). A call by name on another
    object, a native function and a package not found give None."""
    kind, v = n.ops[0][:2]
    if n.op in (0x1C, 0x46, 0x68) and kind == 'obj':
        return (p, v - 1) if v > 0 else resolve(p, v)
    if n.op in (0x1B, 0x45) and kind == 'name' and mine:
        for q, qi in links:
            k = next((k for k, e in enumerate(q.exports) if e['name'] == v and e['outer'] == qi + 1
                      and q.class_of(k + 1) in Package.FUNCTION_CLASSES), None)
            if k is not None: return q, k
    return None


@rule
def ucs_never_spawns_actors(pkg):
    """The construction script spawns no actor, nor does any Blueprint function it calls - of its class, of a Blueprint
    parent, or final on another object: SCS execution and ProcessUserConstructionScript run with the WORLD's
    bIsRunningConstructionScript set (ActorConstruction.cpp 752-800), and SpawnActor then returns null with 'SpawnActor
    failed because we are running a ConstructionScript' (LevelActor.cpp 356-360). The spawn functions are the ones
    marked UnsafeDuringActorConstruction (GameplayStatics.h 58-71). (The editor keeps the spawn node out of the
    construction script graph itself, K2Node_SpawnActorFromClass.cpp 305-309, and only warns about a call to one there,
    K2Node_CallFunction.cpp 2057-2085; a helper the construction script calls is not checked, but on that path its
    spawn fails all the same.) The walk stops at an ubergraph: an event the construction script calls enters it at one
    entry point, and the rest of it never runs from here (BP_EscortMule, BP_HackingPod and PRJ_LineCutter2 spawn in
    theirs, on other events)."""
    ci = bp_class(pkg)
    if ci is None: return
    u = ucs_function(pkg, ci)
    if u is None: return
    links = chain(pkg, ci)[0]
    todo, seen = [(pkg, u)], set()
    while todo:
        p, f = todo.pop()
        if (p.base, f) in seen: continue
        seen.add((p.base, f))
        st = p.struct(f)
        uber = p.tag(p.exports[f]['outer'] - 1, 'UberGraphFunction') if p.exports[f]['outer'] > 0 else None
        if not st or st.function_flags & FUNC_UbergraphFunction or (uber and _i32(uber['value']) == f + 1): continue
        via = '' if (p, f) == (pkg, u) else ' (through %s)' % (
            p.exports[f]['name'] if p is pkg else p.path(f + 1).rsplit('.', 1)[-1])
        # Self is the actor in a function of its class chain; in another class's function (a final call on another
        # object, a library's static) it is an object of that class.
        oc = p.exports[f]['outer'] - 1
        if any(q is p and qi == oc for q, qi in links): self_links = links
        elif oc >= 0 and hasattr(p.struct(oc), 'class_flags'): self_links = chain(p, oc)[0]
        else: self_links = []
        for top in p.script(f):
            for n, mine in top.on_self():
                if n.op not in (0x1B, 0x1C, 0x45, 0x46, 0x68) or not n.ops: continue
                kind, v = n.ops[0][:2]
                if kind == 'obj' and p.path(v) in UNSAFE_DURING_CONSTRUCTION:
                    yield (f if p is pkg else u), 'UserConstructionScript%s calls %s' % (via, p.path(v).rsplit(':', 1)[1])
                r = reached_function(p, f, n, mine, self_links)
                if r: todo.append(r)


def component_template_names(p, i):
    """The names AActor::AddComponent can find on class i: its ComponentTemplates exports and the keys of its
    CookedComponentInstancingData (BlueprintGeneratedClass.cpp 1063-1075; ActorConstruction.cpp 1100-1125)."""
    names = {p.exports[x - 1]['name'] if x > 0 else p.path(x).rsplit(':', 1)[-1]
             for x in tag_objects(p.tag(i, 'ComponentTemplates')) if x}
    t = p.tag(i, 'CookedComponentInstancingData')
    if t: names |= set(map_name_keys(p, i, t))
    return names


@rule
def add_component_names_a_template(pkg):
    """A call AActor::AddComponent(TemplateName, ..., ComponentTemplateContext = self) - the editor's templated Add
    Component node - names a component template of the class or a Blueprint above it: the engine walks the context's
    class chain through CookedComponentInstancingData and FindComponentTemplateByName (ActorConstruction.cpp 1100-1125,
    BlueprintGeneratedClass.cpp 1063-1075). No match makes CreateComponentFromTemplate(nullptr) return None, with no
    log, and every use of the result reads None."""
    ci = bp_class(pkg)
    if ci is None: return
    names = None
    for i, st in functions(pkg):
        for top in pkg.script(i):
            for n, mine in top.on_self():
                if n.op not in (0x1B, 0x1C, 0x45, 0x46) or not n.ops or not mine: continue
                kind, v = n.ops[0][:2]
                target = pkg.path(v) if kind == 'obj' else v
                if target != '/Script/Engine.Actor:AddComponent' and not (kind == 'name' and v == 'AddComponent'): continue
                args = [k for k in n.kids if k.op != 0x16]
                # A literal name, and the context self or null (then GetClass(), ActorConstruction.cpp 1101).
                if len(args) < 4 or args[0].op != 0x21 or args[3].op not in (0x17, 0x2A): continue
                want = args[0].ops[0][1]
                if names is None:
                    links, native = chain(pkg, ci)
                    if native is None: return                   # a /Game parent is missing: its templates are unknown
                    names = set()
                    for p, j in links: names |= {x.lower() for x in component_template_names(p, j)}
                if want.lower() not in names:
                    yield i, 'AddComponent at mem %d names template %s, which the class chain does not have' % (n.mem, want)


TICKERS = {'/Script/Engine.Actor': 'PrimaryActorTick', '/Script/Engine.ActorComponent': 'PrimaryComponentTick'}


@rule
def receive_tick_sets_can_ever_tick(pkg):
    """A Blueprint whose parent is AActor or UActorComponent itself, with its own ReceiveTick event, can ever tick: its
    CDO's PrimaryActorTick (an actor's) or PrimaryComponentTick (a component's) sets bCanEverTick. This is what the
    editor's compiler always writes - it resets the flag to the parent CDO's, false (Actor.cpp 99, ActorComponent.cpp
    260), then sets it for a non-native Event|BlueprintEvent ReceiveTick of the class itself, which a direct AActor /
    UActorComponent child is always allowed (KismetCompiler.cpp 4738-4839, KismetCompilerMisc.cpp 634-644) - and the
    game keeps it on every such class. What it guards is the runtime's: only a set bCanEverTick registers the tick
    function (Actor.cpp 914-925, ActorComponent.cpp 1038-1046), so without it the ReceiveTick the class ships never
    runs. (Deeper parents are left out: whether they may tick depends on native class metadata and an engine ini,
    neither in a cooked package. A stray tag for the other kind's struct is not checked: the loader skips a tag the class
    has no property for, Class.cpp 1403-1497.)"""
    for ci, st in classes(pkg):
        sup = pkg.path(st.super) if st.super else None
        if sup not in TICKERS: continue
        tick = next((i for i, f in functions(pkg) if pkg.exports[i]['name'] == 'ReceiveTick' and pkg.exports[i]['outer'] == ci + 1
                     and f.function_flags & (FUNC_Event | FUNC_BlueprintEvent | FUNC_Native) == FUNC_Event | FUNC_BlueprintEvent), None)
        if st.cdo <= 0: continue
        t = pkg.tag(st.cdo - 1, TICKERS[sup])
        can = bool(bool_in(pkg, st.cdo - 1, t, 'bCanEverTick')) if t else False
        if tick is not None and not can:
            yield st.cdo - 1, 'the class has its own ReceiveTick, and its CDO\'s %s.bCanEverTick is not set' % TICKERS[sup]
