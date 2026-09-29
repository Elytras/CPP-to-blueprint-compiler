import sys
from invariants import *
import os
import struct as _st
from invariants import Rd, TagList, load, GAME_CONTENT, Package, CPF_Parm

# ---- EDITS: what the packages S38 edits (UE_PATCH, UE_ASSET_EDIT) must still hold. AssetGen writes neither component
# instancing data nor timelines nor dynamic bindings, but a patch rewrites game Blueprints that carry them.

BP_CLASSES = ('BlueprintGeneratedClass', 'WidgetBlueprintGeneratedClass', 'AnimBlueprintGeneratedClass')


def _tags_in(pkg, b, at=0):
    """The FPropertyTags of a tag list inside a value's bytes (a struct written as tags), as Package.tags reads an
    export's: name, type, size, index, struct / bool / enum / inner / value type, the value's bytes."""
    r, out = Rd(b, at), TagList()
    while True:
        name = pkg._name(r)
        if name == 'None': break
        t = dict(name=name, type=pkg._name(r))
        t['size'], t['index'] = r.i32(), r.i32()
        if t['type'] == 'StructProperty': t['struct'] = pkg._name(r); r.o += 16
        elif t['type'] == 'BoolProperty': t['bool'] = r.u8()
        elif t['type'] in ('ByteProperty', 'EnumProperty'): t['enum'] = pkg._name(r)
        elif t['type'] in ('ArrayProperty', 'SetProperty'): t['inner'] = pkg._name(r)
        elif t['type'] == 'MapProperty': t['inner'] = pkg._name(r); t['value_type'] = pkg._name(r)
        if r.u8(): r.o += 16
        t['at'] = r.o; t['value'] = b[r.o:r.o + t['size']]; r.o += t['size']
        out.append(t)
    out.end = r.o
    return out


def _struct_elements(pkg, t):
    """An ArrayProperty tag of structs: each element's tag list (after the count and the 49-byte inner tag)."""
    v, out = t['value'], []
    at = 4 + 49
    for _ in range(_st.unpack_from('<i', v)[0]):
        tl = _tags_in(pkg, v, at); out.append(tl); at = tl.end
    return out


def _objects(t):
    """An ArrayProperty tag of object references: its FPackageIndex values."""
    v = t['value']
    return list(_st.unpack_from('<%di' % _st.unpack_from('<i', v)[0], v, 4))


def _fname(pkg, v):
    i, num = _st.unpack_from('<ii', v)
    return pkg.names[i] + ('_%d' % (num - 1) if num else '')


def _field(pkg, tl, name, kind='name'):
    t = next((x for x in tl if x['name'] == name), None)
    if t is None: return None
    if kind == 'name': return _fname(pkg, t['value'])
    if kind == 'obj' or kind == 'int': return _st.unpack_from('<i', t['value'])[0]
    if kind == 'bool': return t['bool']
    return t


def _class_of_export(pkg, i):
    """The Blueprint class export an export sits in (itself, or the first class up its outer chain), or None."""
    k = i
    while k is not None:
        if pkg.class_of(k + 1) in BP_CLASSES: return k
        k = pkg.exports[k]['outer'] - 1 if pkg.exports[k]['outer'] > 0 else None
    return None


def _cooked(pkg, tl):
    """A CookedComponentInstancingData value (FBlueprintCookedComponentInstancingData, written as tags):
    (bHasValidCookedData, [(PropertyName, ArrayIndex, PropertyScope index)])."""
    lst = []
    t = _field(pkg, tl, 'ChangedPropertyList', 'tag')
    if t:
        for el in _struct_elements(pkg, t):
            lst.append((_field(pkg, el, 'PropertyName') or 'None', _field(pkg, el, 'ArrayIndex', 'int') or 0,
                        _field(pkg, el, 'PropertyScope', 'obj') or 0))
    return bool(_field(pkg, tl, 'bHasValidCookedData', 'bool')), lst


def component_data(pkg):
    """Every piece of cooked component instancing data in the package, as (kind, owner export, class export, template
    FPackageIndex, key, valid, list): 'scs' an SCS_Node's CookedComponentInstancingData, 'ich' one of an
    InheritableComponentHandler's Records[], 'ucs' an entry of the class's CookedComponentInstancingData map (keyed by
    the AddComponent template's name)."""
    out = []
    for i, e in enumerate(pkg.exports):
        kind = pkg.class_of(i + 1)
        if kind == 'SCS_Node':
            t = pkg.tag(i, 'CookedComponentInstancingData')
            if t:
                tmpl = pkg.tag(i, 'ComponentTemplate')
                out.append(('scs', i, _class_of_export(pkg, i), _st.unpack_from('<i', tmpl['value'])[0] if tmpl else 0,
                            _field(pkg, pkg.tags(i), 'InternalVariableName'), *_cooked(pkg, _tags_in(pkg, t['value']))))
        elif kind == 'InheritableComponentHandler':
            t = pkg.tag(i, 'Records')
            for rec in (_struct_elements(pkg, t) if t else []):
                c = _field(pkg, rec, 'CookedComponentInstancingData', 'tag')
                if not c: continue
                key = _field(pkg, rec, 'ComponentKey', 'tag')
                out.append(('ich', i, _class_of_export(pkg, i), _field(pkg, rec, 'ComponentTemplate', 'obj') or 0,
                            _field(pkg, _tags_in(pkg, key['value']), 'SCSVariableName') if key else None,
                            *_cooked(pkg, _tags_in(pkg, c['value']))))
        elif kind in BP_CLASSES:
            t = pkg.tag(i, 'CookedComponentInstancingData')
            if not t or t['type'] != 'MapProperty': continue
            v, r = t['value'], Rd(t['value'])
            for _ in range(r.i32()): r.o += 8           # keys to remove: none in a cook
            for _ in range(r.i32()):
                key = pkg._name(r)
                tl = _tags_in(pkg, v, r.o); r.o = tl.end
                tmpl = next((k + 1 for k, x in enumerate(pkg.exports) if x['name'] == key and x['outer'] == i + 1), 0)
                out.append(('ucs', i, i, tmpl, key, *_cooked(pkg, tl)))
    return out


def _class_flag(pkg, c, name):
    t = pkg.tag(c, name) if c is not None else None
    return bool(t and t['type'] == 'BoolProperty' and t['bool'])


@rule
def cooked_instancing_flags(pkg):
    """Cooked component instancing data is valid only in a class that uses it, and a UCS entry is never invalid.
    - The cooker sets bHasValidCookedData only through BuildComponentInstancingData (BlueprintEditorUtils.cpp:10008),
      and each class it does that for gets bHasCookedComponentInstancingData (Blueprint.cpp:1435-1485), or, for an
      override record inherited from a nativized parent, bHasNativizedParent (Blueprint.cpp:1331-1369). The load builds
      the runtime half (ComponentTemplateClass, the cached bytes) only under those flags (BlueprintGeneratedClass.cpp:
      352-392); a valid record anywhere else is data no loader reads, which a cook never writes.
    - An entry of the class's CookedComponentInstancingData map must be valid and keyed by one of its
      ComponentTemplates: AddComponent finds the entry by the template's name and, found but not valid, calls
      CreateComponentFromTemplateData anyway, whose null ComponentTemplateClass makes it return no component
      (ActorConstruction.cpp:1109-1130).
    The survey's other half, "an empty list means not valid", is false: the cooker marks the data valid after gathering
    an empty list (BlueprintEditorUtils.cpp:10005-10008; the header says so, BlueprintGeneratedClass.h:559), and the
    game has two: BP_CombatShotgun_PoisonPlatforms' DefaultSceneRoot node, PRJ_PatrolBotLaser_Flying_Hacked's Trail
    record. The game has no UCS entry at all (no class-level map): that half stands on the source alone."""
    for kind, i, c, tmpl, key, valid, lst in component_data(pkg):
        if valid and not (_class_flag(pkg, c, 'bHasCookedComponentInstancingData')
                          or kind == 'ich' and _class_flag(pkg, c, 'bHasNativizedParent')):
            yield i, '%s data for %s is valid in a class without bHasCookedComponentInstancingData' % (kind, key)
        if kind == 'ucs':
            templates = {pkg.exports[k - 1]['name'].lower() for k in _objects(pkg.tag(c, 'ComponentTemplates'))
                         if k > 0} if pkg.tag(c, 'ComponentTemplates') else set()
            if not valid: yield i, 'the CookedComponentInstancingData entry %s is not valid' % key
            elif (key or '').lower() not in templates: yield i, 'the CookedComponentInstancingData entry %s names no ComponentTemplates object' % key


def _is_struct_scope(pkg, idx):
    return idx != 0 and pkg.class_of(idx) in ('ScriptStruct', 'UserDefinedStruct')


@rule
def cooked_instancing_scopes(pkg):
    """A valid ChangedPropertyList is walked from the component template's class: BuildCachedPropertyList starts at
    ComponentTemplateClass, the class of the template it was built from (BlueprintGeneratedClass.cpp:1900, 1936), and
    stops at the first entry whose PropertyScope is not the scope it is in (1824). The cooker writes the template's
    class as the scope of every root entry and the struct as the scope of a struct's members (BlueprintEditorUtils.cpp:
    9904-9920, 10005). So the first entry's scope is the template's class, and every entry's is that class or a struct:
    any other scope (the class that declares the property, say) ends the walk there, and the rest of the list - the
    properties the fast path copies from the template - is silently dropped."""
    for kind, i, c, tmpl, key, valid, lst in component_data(pkg):
        if not valid or not lst or tmpl <= 0: continue
        cls = pkg.exports[tmpl - 1]['cls']
        if lst[0][2] != cls:
            yield i, '%s %s: the first ChangedPropertyList entry %s has scope %s, the template is a %s' % (
                kind, key, lst[0][0], pkg.path(lst[0][2]), pkg.path(cls))
        bad = [(n, pkg.path(s)) for n, _, s in lst if s != cls and not _is_struct_scope(pkg, s)]
        if bad: yield i, '%s %s: ChangedPropertyList entries scoped outside %s and its structs: %s' % (kind, key, pkg.path(cls), bad[:4])


def _archetype_is_class_default(pkg, tmpl):
    e = pkg.exports[tmpl - 1]
    a = pkg.obj(e['tmpl'])
    return a is not None and a['name'] == 'Default__' + pkg.obj(e['cls'])['name']


@rule
def cooked_template_tags_listed(pkg):
    """An SCS node's template with valid cooked data, in a class with bHasCookedComponentInstancingData, has every
    property it sets named by a root entry of the node's ChangedPropertyList. A spawned actor builds that component on
    the fast path: NewObject of the template's class, then only the listed properties serialized over it from the
    template (SCS_Node.cpp:90-95, ActorConstruction.cpp:1013-1089, BlueprintGeneratedClass.cpp:1912-1953). A property
    the template sets and the list lacks keeps the class default: an edit of it never reaches a spawned component.
    The template's tags are its delta against its archetype, the class's default object, and the list is the cooker's
    delta against that same object (BlueprintEditorUtils.cpp:9986-9988, 10001), so they agree. Narrowed on the game to
    what that makes exact:
    - SCS nodes whose template's archetype is its class's default object. An override record's template is a delta
      against the parent's template, while its list is against the class default, so a tag there may restore the
      class default and rightly go unlisted (15 of the game's 104 records do; every such value is its class's default
      where that is known - null objects, CastShadow and bAutoActivate true, WarmupTime 0): records are
      edited_template_changes_listed's, against the vanilla package.
    - Tags other than StructProperty. The cooker gathers a struct member by member and lists it only if a member
      differs (9922-9933); a struct with a native comparison can be written as a tag with no member listed
      (NiagaraComponent.OverrideParameters, on 5 of the game's 86 nodes)."""
    for kind, i, c, tmpl, key, valid, lst in component_data(pkg):
        if kind != 'scs' or not valid or tmpl <= 0 or not _class_flag(pkg, c, 'bHasCookedComponentInstancingData'): continue
        if not _archetype_is_class_default(pkg, tmpl): continue
        cls = pkg.exports[tmpl - 1]['cls']
        roots = {n.lower() for n, _, s in lst if s == cls}
        missing = [t['name'] for t in pkg.tags(tmpl - 1) if t['type'] != 'StructProperty' and t['name'].lower() not in roots]
        if missing:
            yield i, '%s (%s) sets %s, which its ChangedPropertyList %s does not name: the fast path drops them' % (
                pkg.exports[tmpl - 1]['name'], key, missing, sorted({n for n, _, s in lst if s == cls}))


def vanilla_package(pkg):
    """The game's own copy of an edited package: the package at the same /Game path in a GAME_CONTENT root, when that
    is another file. None for the game's content itself, and for a package the game does not have."""
    rel = pkg.package_name()[len('/Game/'):]
    me = os.path.normcase(os.path.abspath(pkg.base))
    for root in GAME_CONTENT:
        cand = os.path.join(root, *rel.split('/'))
        if os.path.exists(cand + '.uasset') and os.path.normcase(os.path.abspath(cand)) != me:
            return load(cand)
    return None


@rule
def edited_template_changes_listed(pkg):
    """A patch that changes a component template whose cooked instancing data is valid, in a class that uses it, names
    each property it changes in the data's ChangedPropertyList, or clears bHasValidCookedData. Checked against the
    vanilla package (the same /Game path in GAME_CONTENT): a tag the edit adds or rewrites whose name is not a root
    entry of the list.
    At CDO load the engine rebuilds each valid record's cached bytes from the loaded template, restricted to the list
    (BlueprintGeneratedClass.cpp:330-392, 1912-1953), and a spawned actor's component is NewObject of the template's
    class with only those bytes over it (SCS_Node.cpp:90-95 for nodes and override records, GetActualComponentTemplateData
    54-80; ActorConstruction.cpp:1109-1130 for AddComponent). An unlisted change keeps the class default on every
    spawned instance: a silent no-op edit. Invalidating the data makes the node or record take the slow path, which
    duplicates the template (SCS_Node.cpp:97-99); a UCS entry must be removed instead (cooked_instancing_flags).
    A listed root is necessary, not sufficient, for a struct (its members are listed one by one), and an unlisted
    change back to the class default would be harmless: neither is decidable without the native class."""
    old = vanilla_package(pkg)
    if old is None: return
    before = {}
    for kind, i, c, tmpl, key, valid, lst in component_data(old):
        if tmpl > 0: before[old.path(tmpl)] = {(t['name'], t['index']): (t['type'], t['value']) for t in old.tags(tmpl - 1)}
    for kind, i, c, tmpl, key, valid, lst in component_data(pkg):
        if not valid or tmpl <= 0 or not _class_flag(pkg, c, 'bHasCookedComponentInstancingData'): continue
        was = before.get(pkg.path(tmpl))
        if was is None: continue
        cls = pkg.exports[tmpl - 1]['cls']
        roots = {n.lower() for n, _, s in lst if s == cls}
        changed = [t['name'] for t in pkg.tags(tmpl - 1) if was.get((t['name'], t['index'])) != (t['type'], t['value'])]
        missing = [n for n in changed if n.lower() not in roots]
        if missing:
            yield i, '%s (%s): the edit changes %s, which its valid ChangedPropertyList does not name' % (
                pkg.exports[tmpl - 1]['name'], key, missing)


# ---- names a Blueprint binds by name: timelines and dynamic delegate bindings

def _is_script_import(pkg, idx):
    """Whether an FPackageIndex names an object of a /Script (native) package."""
    if idx >= 0: return False
    while pkg.obj(idx)['outer']: idx = pkg.obj(idx)['outer']
    return pkg.obj(idx)['name'].startswith('/Script/')


def readable_ancestry(pkg, c):
    """(Package, Struct) of the class export c and each Blueprint class above it that can be read (this package's
    exports, /Game imports through Package.resolve), and how the chain ended: 'native' at a /Script class (every
    Blueprint class above it was read), 'unread' at a /Game class that could not be read (its package not found)."""
    out, seen = [], set()
    while True:
        st = pkg.struct(c)
        if st is None or (pkg.base, c) in seen: return out, 'unread'
        seen.add((pkg.base, c)); out.append((pkg, st))
        if st.super == 0 or _is_script_import(pkg, st.super): return out, 'native'
        got = pkg.resolve(st.super)
        if got is None: return out, 'unread'
        pkg, c = got


def bound_function(pkg, c, name):
    """The (Package, Struct) of the function FindFunctionByName(name) reaches from class export c among the Blueprint
    classes that can be read, looked up in each one's FuncMap by FName (case-insensitive), or None; and how the chain
    ended (readable_ancestry)."""
    chain, end = readable_ancestry(pkg, c)
    for p, st in chain:
        for k, idx in st.func_map:
            if k.lower() == name.lower() and idx > 0:
                return (p, p.struct(idx - 1)), end
    return None, end


def bound_property(pkg, c, name):
    """(Package, Prop) of the property FindFProperty(Class, name) reaches from class export c among the Blueprint
    classes that can be read, or None; and how the chain ended (readable_ancestry)."""
    chain, end = readable_ancestry(pkg, c)
    for p, st in chain:
        for pr in st.props:
            if pr.name.lower() == name.lower(): return (p, pr), end
    return None, end


def _parms(st):
    return [p for p in st.props if p.flags & CPF_Parm]


def _prop_desc(found):
    if found is None: return 'missing'
    (p, pr) = found
    return '%s %s' % (pr.type, p.path(pr.ref) or '')


@rule
def timeline_names_resolve(pkg):
    """Every timeline a class lists (its Timelines tag) binds by name what its template's cached names say, and the
    runtime reads only those (TimelineTemplate.cpp:43-89 recomputes them only for old package versions):
    - VariableName: an object property of class TimelineComponent, which the new component is assigned to with no
      IsA check (BlueprintGeneratedClass.cpp:1101-1107, FindFProperty<FObjectPropertyBase>; missing, skipped silently);
    - DirectionPropertyName: a byte or enum property (Timeline.cpp:321-345 writes the direction through it);
    - each float track's PropertyName a FloatProperty, each vector / linear color track's a StructProperty of Vector /
      LinearColor: Timeline.cpp:229-314 finds any struct property and writes 12 / 16 bytes at its offset;
    - UpdateFunctionName, FinishedFunctionName and each event track's FunctionName (tracks with a curve): a function of
      the class or a parent (BindUFunction by name, 1111-1166; a missing one never fires), with no parameters - these
      delegates pass none, and ProcessEvent copies the callee's ParmsSize bytes from a null buffer (ScriptCore.cpp:1950).
    The compiler creates every one of these in the class that owns the timeline: the properties (KismetCompiler.cpp:
    821-872) and the functions, as events of its graph (2846-2855). So each must be found among the Blueprint classes
    up to the native one; only a chain broken by a /Game parent that cannot be read is not reported."""
    for c in range(len(pkg.exports)):
        if pkg.class_of(c + 1) not in BP_CLASSES: continue
        tl = pkg.tag(c, 'Timelines')
        if not tl: continue
        for t in _objects(tl):
            if t <= 0:
                yield c, 'Timelines holds %s, not a TimelineTemplate of the package' % pkg.path(t); continue
            tags = pkg.tags(t - 1)
            name = pkg.exports[t - 1]['name']
            var = _field(pkg, tags, 'VariableName')
            if not var or var == 'None':
                yield c, '%s has no VariableName: its component is named at random and bound to nothing' % name; continue
            found, end = bound_property(pkg, c, var)
            if (found is None and end != 'unread' or found is not None and not (
                    found[1].type == 'ObjectProperty' and (found[0].path(found[1].ref) or '').endswith('.TimelineComponent'))):
                yield c, '%s: the timeline component\'s property %s is %s' % (name, var, _prop_desc(found))
            d = _field(pkg, tags, 'DirectionPropertyName')
            if d and d != 'None':
                found, end = bound_property(pkg, c, d)
                if found is None and end != 'unread' or found is not None and found[1].type not in ('ByteProperty', 'EnumProperty'):
                    yield c, '%s: direction property %s is %s' % (name, d, _prop_desc(found))
            for track, want, curve in (('FloatTracks', None, 'CurveFloat'), ('VectorTracks', 'Vector', 'CurveVector'),
                                       ('LinearColorTracks', 'LinearColor', 'CurveLinearColor')):
                tt = _field(pkg, tags, track, 'tag')
                for el in (_struct_elements(pkg, tt) if tt else []):
                    prop = _field(pkg, el, 'PropertyName')
                    if not _field(pkg, el, curve, 'obj') or not prop or prop == 'None': continue
                    found, end = bound_property(pkg, c, prop)
                    if found is None:
                        ok = end == 'unread'
                    elif want is None:
                        ok = found[1].type == 'FloatProperty'
                    else:
                        ok = found[1].type == 'StructProperty' and (found[0].path(found[1].ref) or '').split('.')[-1] == want
                    if not ok:
                        yield c, '%s: %s track property %s is %s' % (name, track, prop, _prop_desc(found))
            fns = [_field(pkg, tags, 'UpdateFunctionName'), _field(pkg, tags, 'FinishedFunctionName')]
            et = _field(pkg, tags, 'EventTracks', 'tag')
            fns += [_field(pkg, el, 'FunctionName') for el in (_struct_elements(pkg, et) if et else [])
                    if _field(pkg, el, 'CurveKeys', 'obj')]
            for fn in fns:
                if not fn or fn == 'None': continue
                got, end = bound_function(pkg, c, fn)
                if got is None:
                    if end != 'unread': yield c, '%s binds %s, which no Blueprint class up to the native one has' % (name, fn)
                elif _parms(got[1]):
                    yield c, '%s binds %s, which takes parameters %s' % (name, fn, [p.name for p in _parms(got[1])])


KEY_BINDINGS = ('InputActionDelegateBinding', 'InputKeyDelegateBinding')   # FInputActionHandlerDynamicSignature(FKey Key)


@rule
def dynamic_binding_names_resolve(pkg):
    """Each of a class's DynamicBindingObjects names what it binds so that it resolves on the class:
    - an entry is not null (BindDynamicDelegates ensure()s and skips one, BlueprintGeneratedClass.cpp:926-991);
    - every FunctionNameToBind is a function of the class or a parent, found by name: a component binding checks with
      FindFunctionByName and skips a missing one (ComponentDelegateBinding.cpp:28), an input binding binds it unchecked
      and it never fires (InputActionDelegateBinding.cpp:23, InputKeyDelegateBinding.cpp:23);
    - a component binding's ComponentPropertyName is an object property of the class (FindFProperty<FObjectProperty>,
      ComponentDelegateBinding.cpp:19: a weak or soft one is not found and the binding is skipped);
    - an action or key binding's function takes no parameter or the one FKey the delegate passes (InputComponent.h:127):
      ProcessEvent hands it the FKey's parms, so a larger parameter block reads past them;
    - at most one input binding object (Input*DelegateBinding) of each class: input is bound through
      GetDynamicBindingObject, the first object of that exact class (InputDelegateBinding.cpp:24-41,
      BlueprintGeneratedClass.cpp:926-940), so a second one's bindings never fire. The compiler makes one object per
      binding class (KismetCompiler.cpp:2622-2630).
    Every FunctionNameToBind is the CustomFunctionName of an event node of the class that owns the binding object
    (K2Node_ComponentBoundEvent.cpp:97, K2Node_InputActionEvent.cpp:29, ...; KismetCompiler.cpp:2608-2635 collects
    them from the class's own nodes), so it must be found among the Blueprint classes up to the native one; only a
    chain broken by a /Game parent that cannot be read is not reported. A component property may be a native one
    (HealthComponent of a native parent): only its type is checked, where it is found."""
    for c in range(len(pkg.exports)):
        if pkg.class_of(c + 1) not in BP_CLASSES: continue
        dbo = pkg.tag(c, 'DynamicBindingObjects')
        if not dbo: continue
        kinds = [pkg.class_of(b) for b in _objects(dbo) if b]
        for k in sorted({k for k in kinds if k.startswith('Input') and k.endswith('DelegateBinding') and kinds.count(k) > 1}):
            yield c, 'DynamicBindingObjects holds %d %s objects: input binds only the first' % (kinds.count(k), k)
        for b in _objects(dbo):
            if b == 0:
                yield c, 'DynamicBindingObjects holds a null entry'; continue
            if b < 0: continue                              # another package's binding object: not read here
            kind = pkg.class_of(b)
            for t in pkg.tags(b - 1):
                if t['type'] != 'ArrayProperty' or t.get('inner') != 'StructProperty': continue
                for el in _struct_elements(pkg, t):
                    fn = _field(pkg, el, 'FunctionNameToBind')
                    if not fn or fn == 'None': continue
                    got, end = bound_function(pkg, c, fn)
                    if got is None:
                        if end != 'unread': yield c, '%s binds %s, which no Blueprint class up to the native one has' % (kind, fn)
                    elif kind in KEY_BINDINGS:
                        ps = _parms(got[1])
                        if len(ps) > 1 or ps and not (ps[0].type == 'StructProperty' and (got[0].path(ps[0].ref) or '').endswith('.Key')):
                            yield c, '%s binds %s, whose parameters %s are not (FKey Key)' % (kind, fn, [(p.type, p.name) for p in ps])
                    comp = _field(pkg, el, 'ComponentPropertyName') if kind == 'ComponentDelegateBinding' else None
                    if comp and comp != 'None':
                        found, end = bound_property(pkg, c, comp)
                        if found is not None and found[1].type not in ('ObjectProperty', 'ClassProperty'):
                            yield c, 'ComponentDelegateBinding: %s is a %s, not an object property' % (comp, found[1].type)
