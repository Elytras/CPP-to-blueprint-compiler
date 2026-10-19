#!/usr/bin/env python3
"""bpdecomp's writer and entry point: each asset as C++ declarations and bodies, the umbrella header, the Assets.h of
UE_ASSET_AT lines, the raw packages copied out, NOTES.md. Usage: see bpdecomp.py."""
import argparse, os, re, shutil, struct, sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import invariants
import bpdecomp_flow as F
import bpdecomp_body as B
from bpdecomp_model import *
from bpdecomp import Run, Ctx, Values, CallInfo
from bpdecomp_emit_util import split_top

HEADER_BANNER = '// Decompiled from %s by AssetGen/tools/bpdecomp.py; edit freely, it is source now.\n'


def ns_open(ns):
    return 'namespace %s {\n' % '::'.join(ns) if ns else ''


def ns_close(ns):
    return '}   // namespace %s\n' % '::'.join(ns) if ns else ''


REP_CONDS = ['None', 'InitialOnly', 'OwnerOnly', 'SkipOwner', 'SimulatedOnly', 'AutonomousOnly', 'SimulatedOrPhysics',
             'InitialOrOwner', 'Custom', 'ReplayOrOwner', 'ReplayOnly', 'SimulatedOnlyNoReplay',
             'SimulatedOrPhysicsNoReplay', 'SkipReplay', 'Never']     # ELifetimeCondition
ZERO = ('0', 'false', 'FName()', '""', '0.0f', '{}', 'nullptr', 'FDateTime(0)', '0ULL', '(int64)0')


def designated(v):
    """(type, [(field, value)]) of a `T{.a = x, ...}` initializer, or None."""
    m = re.fullmatch(r'([\w:]+)\{(\..*)\}', v, re.S)
    if not m: return None
    return m.group(1), [tuple(p[1:].split(' = ', 1)) for p in split_top(m.group(2))]


def is_zero(v):
    d = designated(v)
    return v in ZERO or (d is not None and all(is_zero(x) for _f, x in d[1]))


def field_assigns(lhs, v, partial=False, skip_zero=False):
    """`lhs = v;` as statements; a native struct's designated initializer (an aggregate to the compiler only for a
    mod struct) becomes one assignment per field. `partial`: a mod struct's too - a value read over an archetype's
    (an inherited default, a component template) holds only the members that differ from it, which UE_DEFAULTS says
    member by member; braces would give the others the struct's own defaults instead."""
    d = designated(v)
    if d is None or ('::' in d[0] and not partial): return [] if skip_zero and is_zero(v) else ['%s = %s;' % (lhs, v)]
    return [x for f, val in d[1] for x in field_assigns('%s.%s' % (lhs, f), val, partial, skip_zero)]   # a tag holds only what differs


class Out:
    """One asset's output: its header text, its .cpp text (or None), the mod assets its declaration needs complete."""
    def __init__(s, a):
        s.a, s.header, s.cpp, s.deps, s.fwd = a, '', None, set(), set()
        s.cooked = None         # why the class ships as its cooked package (UE_COOKED), or None


def emit_enum(run, a):
    o = Out(a)
    lines = [HEADER_BANNER % a.path, '#pragma once\n', ns_open(a.ns)]
    lines.append('enum class %s : uint8 {\n' % a.cpp)
    raw = run.enum_raw(a)
    disp = run.model.display_names(a)
    keep = []
    for (name, v), r in zip(run.enum_entries(a), raw):
        lines.append('    %s = %d,\n' % (name, v))
        if name != r or disp.get(r, r) != name:
            keep.append('%s=%s' % (name, r) + ('=' + disp[r] if r in disp and disp[r] != name else ''))
    lines.append('};\nUE_ENUM(%s);\n' % a.cpp)
    if keep:      # the names the editor cooked it with, which kept assets and saved games hold, and its display names
        lines.append('static constexpr const char* %s__UeEnumNames = %s;\n' % (a.cpp, B.cstring('|'.join(keep))))
    lines.append(ns_close(a.ns))
    o.header = ''.join(lines)
    return o


def prop_decl(run, a, p, cur=None):
    return '%s %s' % (run.types.of(a.pkg, p, cur or a), ident(authored(p.name)))


def deps_of_type(run, pkg, p, out):
    """Mod assets a member of property p needs declared in full first: by-value structs, enums, inside containers."""
    if p.type == 'StructProperty' or (p.type == 'ByteProperty' and p.ref) or p.type == 'EnumProperty':
        idx = p.ref if p.type != 'EnumProperty' else p.enum
        a = run.model.asset_of_object(pkg.path(idx))
        if a: out.add(a.path)
    for q in p.subs: deps_of_type(run, pkg, q, out)


def emit_struct(run, a):
    o = Out(a)
    st = a.pkg.struct(a.export)
    vals = Values(Ctx(run, a))
    defaults = {t['name']: t for t in (getattr(st, 'defaults', None) or [])}
    lines = [HEADER_BANNER % a.path, '#pragma once\n', ns_open(a.ns), 'struct %s {\n    UE_STRUCT;\n' % a.cpp]
    for p in st.props:
        deps_of_type(run, a.pkg, p, o.deps)
        init = ''
        t = defaults.get(p.name)
        if t is not None:
            v = vals.tag(a.pkg, t, p, a)
            if v is not None and not is_zero(v): init = ' = ' + v
        lines.append('    %s%s;\n' % (prop_decl(run, a, p), init))
    # the members' editor names, `Kills_3_<guid>`, which kept assets and saved games hold
    lines.append('private:\n    struct UeMarkers {\n%s    };\n' % ''.join(
        '        static constexpr const char* %s__UeName = "%s";\n' % (ident(authored(p.name)), p.name) for p in st.props))
    lines.append('};\n')
    lines.append(ns_close(a.ns))
    o.header = ''.join(lines)
    return o


def func_exports(a):
    pkg = a.pkg
    out = []
    for i, e in enumerate(pkg.exports):
        if e['outer'] != a.export + 1: continue
        k = pkg.class_of(i + 1)
        if k in ('Function', 'DelegateFunction') and not e['name'].endswith('__DelegateSignature'):
            out.append((i, k))        # a dispatcher's signature is its UE_DISPATCHER line
    return out


def signature(run, a, i, st, name=None, cls_prefix=None, static=None):
    """`Ret Name(params)` of function export i, and the return property's name."""
    ret, ps, retname = 'void', [], None
    for p in st.props:
        if not p.flags & CPF_Parm: continue
        ty = run.types.of(a.pkg, p, a)
        if p.flags & CPF_ReturnParm: ret = ty; retname = p.name; continue
        if p.flags & CPF_OutParm and not p.flags & CPF_ConstParm: ty += '&'
        elif p.flags & CPF_ReferenceParm or (p.flags & CPF_OutParm and p.flags & CPF_ConstParm):
            ty = '%s const&' % ty if ty.endswith('*') else 'const %s&' % ty
        pname = 'WorldContextObject' if p.name == '__WorldContext' else ident(p.name)
        ps.append('%s %s' % (ty, pname))
    fname = (cls_prefix + '::' if cls_prefix else '') + ident(name or a.pkg.exports[i]['name'])
    return '%s %s(%s)' % (ret, fname, ', '.join(ps)), retname, ret


class ClassEmit:
    def __init__(s, run, a):
        s.run, s.a, s.pkg = run, a, a.pkg
        s.cx = Ctx(run, a)
        s.st = a.pkg.struct(a.export)
        s.out = Out(a)
        s.methods = []        # (access, decl, body lines or None)
        s.members = []        # (access, text)
        s.extra_members = []
        s.ucs = []            # statements for UserConstructionScript (components the mod declares)
        s.defaults = []       # UE_DEFAULTS lines
        s.markers = []        # UeMarkers lines
        s.const_dropped = []  # Const functions declared without const
        s.notes = run.notes
        s.notes_from = len(run.notes)

    def run_all(s):
        a, pkg, st = s.a, s.pkg, s.st
        funcs = func_exports(a)
        ug = next((i for i, k in funcs if pkg.exports[i]['name'].startswith('ExecuteUbergraph_')), None)
        s.cx.ubergraph = ug
        if ug is not None:
            s.cx.frame_props = {p.name: p for p in pkg.struct(ug).props}
        s.ug_stmts = pkg.script(ug) if ug is not None else None
        dispatchers = {}
        for p in st.props:
            if p.type in ('MulticastInlineDelegateProperty', 'MulticastDelegateProperty'): dispatchers[p.name] = p
        s.components()
        s.member_props(dispatchers)
        s.functions(funcs, ug, dispatchers)
        s.class_defaults()
        return s.write()

    # -- members

    def member_props(s, dispatchers):
        own_defaults = s.cdo_tags()
        vals = Values(s.cx)
        for p in s.st.props:
            if p.name == 'UberGraphFrame': continue
            if p.name in s.component_vars: continue
            if p.name in dispatchers:
                sig = s.run.types.params(s.pkg, p.ref, s.a)
                ps = ', '.join('%s %s' % (t, ident(n)) for t, n, _ in sig[1]) if sig else ''
                s.members.append(('public', 'UE_DISPATCHER(%s%s);' % (ident(p.name), ', ' + ps if ps else '')))
                continue
            deps_of_type(s.run, s.pkg, p, s.out.deps)
            init = ''
            t = own_defaults.pop(p.name, None)
            if t is not None:
                v = vals.tag(s.pkg, t, p, s.a)
                d = designated(v) if v is not None else None
                if v is None or is_zero(v): pass
                elif d is not None and d[0].split('::')[-1] not in {x.cpp for x in s.run.model.assets.values()
                                                                    if x.kind == 'struct'}:
                    # a native struct is no aggregate, and AssetGen takes a variable's own default as its initializer
                    # only: what the cooked default sets past zero is the note's to check against the struct's own
                    left = field_assigns(ident(p.name), v, skip_zero=True)
                    if left:
                        s.notes.append('%s: variable %s has a native struct default C++ cannot initialize, dropped: %s'
                                       % (s.a.path, p.name, ' '.join(left)))
                else: init = ' = ' + v
            cpp = ident(p.name)
            if cpp != p.name: s.markers.append('static constexpr const char* %s__UeName = %s;' % (cpp, B.cstring(p.name)))
            ty = s.run.types.of(s.pkg, p, s.a)
            if p.flags & CPF_Net:
                notify = p.notify if p.notify and p.notify != 'None' else None
                cond = REP_CONDS[p.cond] if 0 < p.cond < len(REP_CONDS) else None
                macro = 'UE_REPLICATED' + ('_USING' if notify else '') + ('_IF' if cond else '')
                args = [ty, cpp] + ([ident(notify)] if notify else []) + ([cond] if cond else [])
                s.members.append(('public', '%s(%s)%s;' % (macro, ', '.join(args), init)))
            else:
                s.members.append(('public', '%s %s%s;' % (ty, cpp, init)))
        subs = {}                # a default subobject's export -> the inherited member that points at it
        for name, t in own_defaults.items():
            if name == 'UberGraphFrame': continue
            sub = s.subobject_of(t)
            if sub is not None:
                subs.setdefault(sub, name)
                continue
            v = vals.tag(s.pkg, t, s.inherited_prop(name), s.a)
            if v is None: continue
            cpp = s.member_cpp(name)
            if cpp is None:     # the engine skips a tag of a property the class no longer has
                s.notes.append('%s: inherited default %s, which no parent declares (to AssetGen) any more; dropped'
                               % (s.a.path, name))
                continue
            s.defaults += field_assigns(cpp, v, partial=True)
        s.inherited_components(subs)

    def member_cpp(s, name):
        """An inherited member as the declaring class spells it: a tag carries the engine's name, which a UeApi header
        may spell in another case (Temperature, `temperature`) or rename (`X__UeName`)."""
        low = name.lower()
        for cn in s.run.model.ancestors(s.a.cpp)[1:]:
            mine = [x for x in s.run.model.assets.values() if x.cpp == cn and x.kind in ('class', 'widget')]
            if mine:
                x = mine[0]
                st = x.pkg.struct(x.export)
                names = [p.name for p in st.props] if st else []
                for i, e in enumerate(x.pkg.exports):
                    if x.pkg.class_of(i + 1) == 'SCS_Node':
                        t = {tt['name']: tt for tt in x.pkg.tags(i)}.get('InternalVariableName')
                        if t: names.append(x.pkg.names[struct.unpack('<i', bytes(t['value'])[:4])[0]])
                hit = next((n for n in names if n.lower() == low), None)
                if hit: return ident(hit)
                continue
            hit = s.run.api.members(cn).get(low)
            if hit: return hit
        return None

    def subobject_of(s, t):
        """The export a CDO's object tag points at when it is one of this class's own subobjects (a default subobject
        under the CDO, a component template under the class), else None. The engine sets such a pointer itself as it
        makes the object, so it is no default to restate."""
        if t['type'] != 'ObjectProperty' or len(bytes(t['value'])) != 4: return None
        idx = struct.unpack('<i', bytes(t['value']))[0]
        if idx <= 0: return None
        outer = s.pkg.exports[idx - 1]['outer']
        return idx - 1 if outer in (s.st.cdo, s.a.export + 1) and outer else None

    def inherited_components(s, subs):
        """The defaults of components this class inherits, as `Comp->Field = value;`: a native parent's default
        subobject, under the CDO, named by the member that points at it; and a parent Blueprint's SCS component, an
        override template `<Var>_GEN_VARIABLE` under the class (UInheritableComponentHandler's record)."""
        pkg, cdo = s.pkg, s.st.cdo
        for i, e in enumerate(pkg.exports):
            if cdo and e['outer'] == cdo:
                name = subs.get(i)
                if name is None:
                    if pkg.tags(i):
                        s.notes.append('%s: default subobject %s has defaults but no member points at it; dropped'
                                       % (s.a.path, e['name']))
                    continue
                s.object_defaults(i, s.member_cpp(name) or ident(name))
            elif e['outer'] == s.a.export + 1 and e['name'].endswith('_GEN_VARIABLE') and i + 1 not in s.own_templates \
                    and pkg.class_of(i + 1) not in ('SCS_Node', 'SimpleConstructionScript'):
                var = e['name'][:-len('_GEN_VARIABLE')]
                cpp = s.member_cpp(var)
                if cpp is None:     # the engine skips a record whose parent node is gone: a game update removed it
                    s.notes.append('%s: overrides inherited component %s, which no parent declares any more; dropped'
                                   % (s.a.path, var))
                    continue
                s.object_defaults(i, cpp)

    def object_defaults(s, i, cpp):
        """Export i's tags, a component's values over its archetype, as `cpp->Field = value;` lines. A pointer to one
        of the class's own subobjects (AttachParent) is the engine's, and CreationMethod is set as it makes the
        component; neither is restated."""
        vals = Values(s.cx)
        cn = s.pkg.class_of(i + 1)
        h = s.run.api.header_of(cn) if not any(x.cpp == cn[:-2] for x in s.run.model.assets.values()) else None
        if h: s.run.extra_includes.setdefault(s.a.mod.name, set()).add(h)
        for t in s.pkg.tags(i):
            if t['name'] in ('CreationMethod', 'UCSSerializationIndex', 'UCSModifiedProperties'): continue
            if t['type'] == 'ObjectProperty' and len(bytes(t['value'])) == 4:
                idx = struct.unpack('<i', bytes(t['value']))[0]
                if idx > 0 and s.pkg.exports[idx - 1]['outer'] in (s.st.cdo, s.a.export + 1): continue
            v = vals.tag(s.pkg, t, None, s.a)
            if v is not None: s.defaults += field_assigns('%s->%s' % (cpp, ident(t['name'])), v, partial=True)

    def inherited_prop(s, name):
        st = s.st
        pkg = s.pkg
        while st is not None and st.super:
            at = pkg.resolve(st.super)
            if not at: return None
            pkg, k = at
            st = pkg.struct(k)
            p = next((q for q in st.props if q.name == name), None)
            if p: return p
        return None

    def cdo_tags(s):
        cdo = s.st.cdo
        if not cdo: return {}
        return {t['name']: t for t in s.pkg.tags(cdo - 1)} if cdo > 0 else {}

    def components(s):
        s.component_vars = set()
        s.own_templates = set()
        pkg = s.pkg
        scs = next((i for i, e in enumerate(pkg.exports) if pkg.class_of(i + 1) == 'SimpleConstructionScript'
                    and e['outer'] == s.a.export + 1), None)
        if scs is None: return
        nodes = [i for i, e in enumerate(pkg.exports) if pkg.class_of(i + 1) == 'SCS_Node']
        for i in nodes:
            tags = {t['name']: t for t in pkg.tags(i)}
            var = s.name_tag(tags.get('InternalVariableName'))
            cls_idx = struct.unpack('<i', bytes(tags['ComponentClass']['value'])[:4])[0] if 'ComponentClass' in tags else 0
            tmpl = struct.unpack('<i', bytes(tags['ComponentTemplate']['value'])[:4])[0] if 'ComponentTemplate' in tags else 0
            cls = s.run.types.cls(pkg, cls_idx, s.a)
            s.component_vars.add(var)
            cpp = ident(var)
            is_mod = s.run.model.asset_of_object(pkg.path(cls_idx)) is not None
            if var == 'DefaultSceneRoot' and cls == 'USceneComponent':
                continue                       # AssetGen gives an actor with no scene component this same root
            if cpp != var: s.markers.append('static constexpr const char* %s__UeName = %s;' % (cpp, B.cstring(var)))
            if 'VariableGuid' in tags:      # the node's guid, which a subclass's override record names it by
                s.markers.append('static constexpr const char* %s__UeScsNode = "%s";'
                                 % (cpp, bytes(tags['VariableGuid']['value'])[:16].hex()))
            s.members.append(('public', 'UE_COMPONENT(%s, %s);' % (cls, cpp)))
            if tmpl > 0:
                s.own_templates.add(tmpl)
                s.object_defaults(tmpl - 1, cpp)

    def name_tag(s, t):
        if t is None: return None
        b = bytes(t['value'])
        i, n = struct.unpack('<ii', b[:8])
        return s.pkg.names[i] + ('_%d' % (n - 1) if n else '')

    # -- functions

    def event_entry(s, i, ug):
        """(entry mem, frame var -> param) when function i is an event stub that runs part of the ubergraph."""
        ugname = s.pkg.exports[ug]['name']
        entry, copies = None, {}
        for n in s.pkg.script(i):
            if n.op in (0x1C, 0x46):
                f = s.pkg.obj(next(o[1] for o in n.ops if o[0] == 'obj'))
                if f['name'] == ugname:
                    arg = n.kids[0]
                    entry = next(o[1] for o in arg.ops if o[0] == 'int') if arg.op == 0x1D else 0
            elif n.op == 0x64 and n.kids and n.kids[0].op in (0x00, 0x48):
                copies[B.var_name(n)[0]] = B.var_name(n.kids[0])[0]
        return (entry, copies) if entry is not None else None

    def latent_splits(s, stmts):
        out = {}
        for k, n in enumerate(stmts):
            for x in B.walk(n):
                if x.op == 0x2F and s.pkg.path(next(o[1] for o in x.ops if o[0] == 'obj')).endswith('.LatentActionInfo'):
                    link = x.kids[0]
                    out[k] = ('latent', next(o[1] for o in link.ops if o[0] == 'int'))
        return out

    def frame_vars_reached(s, entry, split):
        g = F.Graph(s.ug_stmts, entry, split)
        names = set()
        for k, (kind, pay, _) in g.nodes.items():
            if pay is None: continue
            for x in B.walk(pay):
                if x.op in (0x00, 0x48, 0x64):
                    nm, owner = B.var_name(x)
                    if owner and owner - 1 == s.cx.ubergraph: names.add(nm)
        return names

    def functions(s, funcs, ug, dispatchers):
        pkg = s.pkg
        events = {}
        split = s.latent_splits(s.ug_stmts) if ug is not None else {}
        for i, kind in funcs:
            if kind != 'Function' or i == ug: continue
            if ug is not None:
                ev = s.event_entry(i, ug)
                if ev: events[i] = ev
        reach = {i: s.frame_vars_reached(ev[0], split) for i, ev in events.items()}
        count = defaultdict(int)
        for i, names in reach.items():
            for n in names: count[n] += 1
        shared = {n for n, c in count.items() if c > 1}
        s.shared_frame = shared
        for n in sorted(shared):
            p = s.cx.frame_props[n]
            s.members.append(('private', '%s %s;   // shared by several events' % (s.run.types.of(pkg, p, s.a), ident(n))))
        for i, kind in funcs:
            if kind != 'Function' or i == ug: continue
            e = pkg.exports[i]
            st = pkg.struct(i)
            if i in events:
                s.event_method(i, st, events[i], split, reach[i], shared)
            else:
                s.plain_method(i, st)

    def flags_decl(s, st, decl):
        f = st.function_flags
        if s.run.types.cls(s.pkg, s.st.super, s.a) == 'UBlueprintFunctionLibrary':
            f |= FUNC_Static              # a library's private helper is cooked without Static; nothing instances one
        pre = ''
        if f & FUNC_BlueprintPure and not decl.startswith('void '): pre += 'UE_PURE '
        if f & FUNC_Net and not st.super and not {'AActor', 'UActorComponent'} & set(s.run.model.ancestors(s.a.cpp)):
            s.notes.append('%s.%s: an RPC on a class that does not replicate, which runs where it is called: '
                           'declared a plain function' % (s.a.path, decl.split('(')[0].split()[-1]))
        elif f & FUNC_Net and not st.super:      # an override takes its parent's replication
            pre += 'UE_SERVER ' if f & FUNC_NetServer else 'UE_CLIENT ' if f & FUNC_NetClient else 'UE_MULTICAST '
            if f & FUNC_NetReliable: pre += 'UE_RELIABLE '
        if f & FUNC_Static: pre += 'static '
        post = ' const' if f & FUNC_Const and not f & FUNC_Static and st.super else ''
        if f & FUNC_Const and not st.super and not f & FUNC_Static: s.const_dropped.append(decl.split('(')[0])
        access = 'private' if f & FUNC_Private else 'protected' if f & FUNC_Protected else 'public'
        return access, pre, post

    def body_text(s, body, retname, rettype, local_props, extra_first=()):
        try:
            tree = body.build()
        except Exception as ex:
            if os.environ.get('BPDECOMP_TRACE'): import traceback; traceback.print_exc()
            s.notes.append('%s.%s: not decompiled (%s: %s)' % (s.a.path, body.method, type(ex).__name__, ex))
            return ['    // bpdecomp: not decompiled (%s)' % ex] + (['    return {};'] if rettype != 'void' else [])
        rv = ident(retname) if retname else None
        tree = B.tidy(tree, rv)
        if rv:
            def fix(xs):
                for x in xs:
                    if isinstance(x, B.CReturn) and x.text is None: x.text = rv
                    elif isinstance(x, B.CIf): fix(x.then); fix(x.other)
                    elif isinstance(x, B.CWhile): fix(x.body)
            fix(tree)
        decls = []
        for name in sorted(body.locals_used):
            if name in body.frame: continue
            p = local_props.get(name)
            if p is None or (p.flags & CPF_Parm and not p.flags & CPF_ReturnParm): continue
            ty = s.run.types.of(s.pkg, p, s.a)
            for d in [set()]:
                deps_of_type(s.run, s.pkg, p, d)
            decls.append('    %s %s;' % (ty, s.cx.local_ident(body, name)))
        for n in body.notes: s.notes.append('%s: %s' % (s.a.path, n))
        for name, lines in body.latent_helpers:
            s.methods.append(('private', 'void %s();' % name, B.render(B.tidy(lines))))
        return list(extra_first) + decls + B.render(tree)

    def plain_method(s, i, st):
        decl, retname, rettype = signature(s.run, s.a, i, st)
        access, pre, post = s.flags_decl(st, decl)
        stmts = s.pkg.script(i)
        body = B.Body(s.cx, i, stmts)
        lines = s.body_text(body, retname, rettype, body.props) if stmts else []
        s.methods.append((access, pre + decl + post + ';', lines, decl, post))

    def event_method(s, i, st, ev, split, reached, shared):
        entry, copies = ev
        decl, retname, rettype = signature(s.run, s.a, i, st)
        access, pre, post = s.flags_decl(st, decl)
        frame, first = {}, []
        for fv, param in copies.items():
            pid = 'WorldContextObject' if param == '__WorldContext' else ident(param)
            if fv in shared: first.append('    %s = %s;' % (ident(fv), pid))
            else: frame[fv] = pid
        for fv in shared: frame.setdefault(fv, ident(fv))
        body = B.Body(s.cx, s.cx.ubergraph, s.ug_stmts, entry, frame, split, s.pkg.exports[i]['name'])
        body.props = dict(s.cx.frame_props)
        lines = s.body_text(body, None, rettype, s.cx.frame_props, first)
        s.methods.append((access, pre + decl + post + ';', lines, decl, post))

    def class_defaults(s):
        if s.ucs:
            has = any(m[1].split('(')[0].endswith(' UserConstructionScript') for m in s.methods)
            if has:
                s.notes.append('%s: has a UserConstructionScript of its own; the component adds were put first' % s.a.path)
                for k, m in enumerate(s.methods):
                    if m[1].split('(')[0].endswith(' UserConstructionScript'):
                        s.methods[k] = (m[0], m[1], ['    ' + l for l in s.ucs] + m[2], m[3], m[4])
            else:
                s.methods.insert(0, ('public', 'void UserConstructionScript();', ['    ' + l for l in s.ucs],
                                     'void UserConstructionScript()', ''))

    # -- text

    def write(s):
        a, st, pkg = s.a, s.st, s.pkg
        o = s.out
        mine = s.notes[s.notes_from:]
        if any('an instanced subobject' in n or 'has defaults but no member points at it' in n for n in mine):
            o.cooked = 'its defaults hold instanced subobjects, which AssetGen cannot write yet'
        elif a.path in s.run.cook:      # --cook: what its source needs that AssetGen cannot compile yet
            o.cooked = s.run.cook[a.path]
        if o.cooked: s.notes.append('%s: shipped as its cooked package (UE_COOKED): %s' % (a.path, o.cooked))
        parent = s.run.types.cls(pkg, st.super, a)
        pa = s.run.model.asset_of_object(pkg.path(st.super))
        if pa: o.deps.add(pa.path)
        bases = [parent]
        for (ci, _, k2) in st.interfaces:
            ia = s.run.model.asset_of_object(pkg.path(ci))
            if ia: o.deps.add(ia.path)
            bases.append(s.run.types.cls(pkg, ci, a))
        is_iface = a.kind == 'interface'
        lines = [HEADER_BANNER % a.path, '#pragma once\n', ns_open(a.ns)]
        if is_iface:
            iparent = [b for b in bases if b not in ('UInterface', 'IInterface', 'UObject')]
            lines.append('class %s%s {\npublic:\n    UE_INTERFACE;\n' % (a.cpp, ' : public ' + iparent[0] if iparent else ''))
        else:
            lines.append('class %s : %s {\n' % (a.cpp, ', '.join('public ' + b for b in bases)))
        if s.defaults and not is_iface:
            defaults = s.defaults
            if o.cooked:    # nothing reads them; one naming another mod class would order the umbrella around it
                own = {x.cpp for x in s.run.model.assets.values()}
                defaults = ['// %s   (UE_COOKED: the cooked package holds it)' % d
                            if any(n.split('::')[-1] in own for n in re.findall(r'([\w:]+)::StaticClass\(\)', d)) else d
                            for d in defaults]
            lines.append('    UE_DEFAULTS {\n%s    }\n' % ''.join('        %s\n' % d for d in defaults))
        for access in ('public', 'protected', 'private'):
            ms = [m for acc, m in s.members if acc == access]
            fs = [f for f in s.methods if f[0] == access]
            if not ms and not fs: continue
            if not is_iface or access != 'public': lines.append('%s:\n' % access)
            for m in ms: lines.append('    %s\n' % m)
            for f in fs:
                if is_iface:
                    lines.append('    %s\n' % f[1])
                else:
                    lines.append('    %s\n' % f[1])
        if s.markers:
            lines.append('private:\n    struct UeMarkers {\n%s    };\n' % ''.join('        %s\n' % m for m in s.markers))
        lines.append('};\n')
        lines.append(ns_close(a.ns))
        o.header = ''.join(lines)
        # A class a default names (X::StaticClass(), a UE_COMPONENT's type, whose members a `Comp->` default reaches)
        # must be complete before this one: its declaration comes first in the umbrella.
        by_cpp = s.run.__dict__.setdefault('by_cpp', {})
        if not by_cpp:
            for x in s.run.model.assets.values(): by_cpp.setdefault(x.cpp, []).append(x.path)
        code = re.sub(r'//[^\n]*', '', o.header)
        for name in set(re.findall(r'([\w:]+)::StaticClass\(\)', code)) | \
                set(re.findall(r'UE_COMPONENT\(([\w:]+),', code)) | set(re.findall(r'([\w:]+)\* \w+;', code)):
            hits = by_cpp.get(name.split('::')[-1], [])
            if len(hits) == 1 and hits[0] != a.path: o.deps.add(hits[0])
        if not is_iface:
            body = []
            for f in s.methods:
                if len(f) < 5: continue
                access, d, blines, decl, post = f
                if blines is None: continue
                ret, rest = decl.split(' ', 1) if not decl.startswith('const ') else (decl, '')
                m = re.match(r'^(.*?\b)(\w+)(\(.*)$', decl)
                full = '%s%s::%s%s%s' % (m.group(1), a.cpp, m.group(2), m.group(3), post)
                body.append(full + ' {\n' + ''.join(l + '\n' for l in blines) + '}\n\n')
            for access, d, blines in [m for m in s.methods if len(m) == 3]:
                mm = re.match(r'^(.*?\b)(\w+)(\(.*\));$', d)
                body.append('%s%s::%s%s {\n%s}\n\n' % (mm.group(1), a.cpp, mm.group(2), mm.group(3), ''.join(l + '\n' for l in blines)))
            if body:
                o.cpp = (HEADER_BANNER % a.path) + '#include "%s"\n\n' % '{UMBRELLA}' + ns_open(a.ns) + '\n' + ''.join(body) + ns_close(a.ns)
        return o


def emit_class(run, a):
    return ClassEmit(run, a).run_all()


# ---- the umbrella, Assets.h, ordering, files

def order(outs):
    by = {o.a.path: o for o in outs}
    done, seq = set(), []
    def visit(o, stack=()):
        if o.a.path in done: return
        if o.a.path in stack: return
        for d in sorted(o.deps):
            if d in by and d != o.a.path: visit(by[d], stack + (o.a.path,))
        done.add(o.a.path); seq.append(o)
    rank = {'enum': 0, 'struct': 1, 'interface': 2, 'class': 3, 'widget': 3}
    for o in sorted(outs, key=lambda o: (rank.get(o.a.kind, 9), o.a.path)): visit(o)
    return seq


def rel_file(mod, a, ext):
    if not a.path.startswith(mod.root + '/'):      # a second root's asset, in its Game:: namespace
        return os.path.join('Game', *(a.path[len('/Game/'):].split('/'))) + ext
    return os.path.join(*(a.path[len(mod.root) + 1:].split('/'))) + ext


def link_mods(run, model, per_mod):
    """A mod whose code names another's classes includes that mod's umbrella; every class says with UE_CLASS where
    it is cooked, or the other mod would cook it a second time and a kept asset of it would load as null."""
    run.mod_needs = defaultdict(list)
    texts = {m.name: ''.join(o.header + (o.cpp or '') for o in per_mod[m.name]) for m in model.mods}
    for m in model.mods:
        for n in model.mods:
            if n is m: continue
            classes = [o for o in per_mod[n.name] if o.a.kind == 'class']
            if not any(re.search(r'(?<![\w:])(?:::)?%s\b' % re.escape('::'.join(o.a.ns + [o.a.cpp])), texts[m.name])
                       for o in classes): continue
            run.mod_needs[m.name].append(n.name)
    for m in model.mods:            # every class says where it is cooked: kept assets of it and other mods name it so
        for o in [o for o in per_mod[m.name] if o.a.kind == 'class']:
            name = o.a.pkg.exports[o.a.export]['name']
            line = '    UE_CLASS("%s", "%s");\n' % (o.a.path, name)
            if o.cooked:
                line += ('    UE_COOKED;   // bpdecomp: %s. The package ships as it was cooked (mods.yaml kept:),\n'
                         '                 // and this class is imported; its .cpp is not compiled. Drop UE_COOKED then.\n' % o.cooked)
            o.header, n = re.subn(r'(\nclass %s\b[^\n]*\{\n(?:.*\n)*?public:\n)' % re.escape(o.a.cpp),
                                  lambda mm: mm.group(1) + line, o.header, count=1)
            if not n:               # a class with no members has no public: section yet
                o.header = re.sub(r'(\nclass %s\b[^\n]*\{\n(?:.*\n)*?)(\};\n)' % re.escape(o.a.cpp),
                                  lambda mm: mm.group(1) + 'public:\n' + line + mm.group(2), o.header, count=1)


def write_mod(run, mod, outs, out_dir):
    root = os.path.join(out_dir)
    os.makedirs(root, exist_ok=True)
    umbrella = mod.name + '.h'
    seq = order(outs)
    fwd = defaultdict(list)
    for o in seq:
        if o.a.kind in ('class', 'interface', 'widget'): fwd[tuple(o.a.ns)].append('class %s;' % o.a.cpp)
        if o.a.kind == 'struct': fwd[tuple(o.a.ns)].append('struct %s;' % o.a.cpp)
    lines = ['// %s: the umbrella header. Every source of the mod includes it; it includes the declarations in the\n'
             '// order C++ needs them. Generated by AssetGen/tools/bpdecomp.py.\n' % mod.name, '#pragma once\n',
             '#include "../UeApi/Types.h"\n', '#include "../UeApi/UeMeta.h"\n', '#include "../UeApi/Engine.h"\n',
             '#include "../UeApi/FSD.h"\n', '#include "../UeApi/UMG.h"\n', '#include "../../AssetGen/include/Objects.h"\n']
    base = {'Types.h', 'UeMeta.h', 'Engine.h', 'FSD.h', 'UMG.h', 'CoreUObject.h'}
    text = ''.join(o.header + (o.cpp or '') for o in seq)
    # _C: a game Blueprint's. A dump taken with the mod loaded has headers for the mod's own classes too: not those.
    own = {a.cpp for a in run.model.assets.values()} | {a.pkg.exports[a.export]['name'] for a in run.model.assets.values()
                                                        if a.export is not None}
    words = set(re.findall(r'\b[UAFIE][A-Z]\w+\b', text)) | {w for w in re.findall(r'\b\w+_C\b', text) if w not in own and w[:-2] not in own}
    used = {run.api.header_of(w) for w in words} - base - {None}
    for h in sorted(used | run.extra_includes.get(mod.name, set())): lines.append('#include "../UeApi/%s"\n' % h)
    for n in run.mod_needs.get(mod.name, ()): lines.append('#include "../%s/%s.h"     // another mod\n' % (n, n))
    lines.append('\n')
    if getattr(run, 'class_defaults_used', False):
        lines.append('// The Get Class Defaults node, which AssetGen does not compile yet (EX_ClassContext): the class default object,\n'
                     '// found by its path, /Game/A/B.B_C -> /Game/A/B.Default__B_C. bpdecomp writes it.\n'
                     '#ifndef BPDECOMP_CLASS_DEFAULTS\n#define BPDECOMP_CLASS_DEFAULTS\n'
                     'inline UObject* ClassDefaults(UClass* Class) {\n'
                     '    return UKismetSystemLibrary::LoadAsset_Blocking(UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(\n'
                     '        UKismetSystemLibrary::MakeSoftObjectPath(UKismetStringLibrary::Replace(\n'
                     '            UKismetSystemLibrary::GetPathName(Class), ".", ".Default__", ESearchCase::CaseSensitive))));\n'
                     '}\n#endif\n\n')
    for ns, items in sorted(fwd.items()):
        lines.append('%s%s\n%s' % (ns_open(list(ns)), '\n'.join(items), ns_close(list(ns))))
    lines.append('\n#include "Assets.h"\n\n')
    for o in seq:
        rel = rel_file(mod, o.a, '.h')
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'w', encoding='utf-8-sig', newline='\n').write(o.header)
        lines.append('#include "%s"\n' % rel.replace(os.sep, '/'))
        if o.cpp:
            cpath = os.path.join(root, rel_file(mod, o.a, '.cpp'))
            depth = rel.count(os.sep)
            inc = '../' * depth + umbrella
            open(cpath, 'w', encoding='utf-8-sig', newline='\n').write(o.cpp.replace('{UMBRELLA}', inc))
    open(os.path.join(root, umbrella), 'w', encoding='utf-8-sig', newline='\n').write(''.join(lines))
    used = run.assets_used.get(mod.name, {})
    al = ['// The packages the mod points at that AssetGen does not cook (textures, materials, fonts, sounds, kept data\n'
          '// assets): each a UE_ASSET_AT. Generated by AssetGen/tools/bpdecomp.py.\n', '#pragma once\n\n']
    by_ns = defaultdict(list)
    for path, (cls, ns, name) in sorted(used.items()):
        cls = cls if cls.startswith('::') or '::' in cls else '::' + cls
        by_ns[tuple(ns)].append('UE_ASSET_AT(%s, %s, "%s");' % (cls, name, path))
    for ns, items in sorted(by_ns.items()):
        al.append('%s%s\n%s' % (ns_open(list(ns)), '\n'.join(items), ns_close(list(ns))))
    open(os.path.join(root, 'Assets.h'), 'w', encoding='utf-8-sig', newline='\n').write(''.join(al))
    cpps = sorted(rel_file(mod, o.a, '.cpp').replace(os.sep, '/') for o in seq if o.cpp and not o.cooked)
    open(os.path.join(root, mod.name + '.cpp'), 'w', encoding='utf-8-sig', newline='\n').write(
        '// The mod\'s package and its one translation unit: mods.yaml lists this file, which takes in every other source.\n'
        '#include "%s"\n\nUE_MOD_PACKAGE("%s");\n\n%s' % (umbrella, mod.root, ''.join('#include "%s"\n' % c for c in cpps)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('content'); ap.add_argument('out')
    ap.add_argument('--mod', action='append', required=True)
    ap.add_argument('--names'); ap.add_argument('--raw'); ap.add_argument('--keep', action='append', default=[])
    ap.add_argument('--cook', action='append', default=[], help='<asset path>=<why>: ship that class cooked (UE_COOKED)')
    args = ap.parse_args()
    mods = [tuple(m.split('=', 1)) for m in args.mod]
    model = Model(args.content, mods, args.names)
    run = Run(model, args.raw, args.keep)
    run.extra_includes = defaultdict(set)
    run.cook = dict(c.split('=', 1) for c in args.cook)
    per_mod = defaultdict(list)
    raw = []
    for a in model.assets.values():
        if a.path in run.keep or a.kind in ('raw', 'data', 'widget'):
            raw.append(a); continue
        try:
            o = {'enum': emit_enum, 'struct': emit_struct}.get(a.kind, emit_class)(run, a)
        except Exception as ex:
            import traceback; traceback.print_exc()
            run.notes.append('%s: not decompiled (%s: %s)' % (a.path, type(ex).__name__, ex))
            continue
        per_mod[a.mod.name].append(o)
        if o.cooked: raw.append(a)
    link_mods(run, model, per_mod)
    for mod in model.mods:
        write_mod(run, mod, per_mod[mod.name], os.path.join(args.out, mod.name))
    if args.raw:
        for a in raw:
            src = args.content + a.path[len('/Game'):]
            for ext in ('.uasset', '.uexp', '.ubulk'):
                if os.path.exists(src + ext):
                    dst = os.path.join(args.raw, 'Content', *a.path[len('/Game/'):].split('/')) + ext
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copy2(src + ext, dst)
    notes = sorted(set(run.notes)) + sorted('UeApi lacks ' + m for m in run.types.missing)
    open(os.path.join(args.out, 'NOTES.md'), 'w', encoding='utf-8', newline='\n').write(
        '# bpdecomp notes\n\nKept as cooked: %d packages (%s).\n\n' % (len(raw), ', '.join(sorted({a.kind for a in raw}))) +
        ''.join('- %s\n' % n for n in notes))
    print('decompiled %d assets, kept %d as cooked, %d notes' % (sum(len(v) for v in per_mod.values()), len(raw), len(notes)))


if __name__ == '__main__':
    main()
