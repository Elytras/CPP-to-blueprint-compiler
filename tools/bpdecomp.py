#!/usr/bin/env python3
"""usage: bpdecomp.py <Content dir> <out dir> --mod </Game/root>=<ModName> [--mod ...] [--names <uncooked Content dir>]
                      [--raw <dir>] [--keep <package path>]... [--only <package path prefix>]

A decompiler from cooked UE 4.27 Blueprint packages back to AssetGen C++. Every UserDefinedEnum, UserDefinedStruct,
Blueprint interface and Blueprint class under each --mod root becomes a header (its declaration) and, for a class with
code, a .cpp (its bodies), in folders that mirror the package paths, so bpbuild cooks them back to the same paths. Each
mod gets an umbrella header that includes everything in dependency order and a <ModName>.cpp holding UE_MOD_PACKAGE.

Packages AssetGen cannot write (textures, materials, fonts, sounds, and any --keep) are copied unchanged under --raw,
for bpbuild's `raw` key to pack beside the cooked ones.

The model (packages, names, types) is bpdecomp_model, the control flow bpdecomp_flow, the bodies bpdecomp_body. What
is not decompiled is written as a `bpdecomp:` comment where it was and listed in NOTES.md, so the output always says
what it is missing rather than compiling into something else."""
import os, re, shutil, struct, sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import invariants
import bpdecomp_flow as F
import bpdecomp_body as B
from bpdecomp_model import *


# ---- tagged values (class defaults, struct defaults, component templates, data assets)

IMMUTABLE = {'Vector': ('<fff', 'FVector'), 'Vector2D': ('<ff', 'FVector2D'), 'Rotator': ('<fff', 'FRotator'),
             'Vector4': ('<ffff', 'FVector4'), 'Quat': ('<ffff', 'FQuat'), 'IntPoint': ('<ii', 'FIntPoint'),
             'IntVector': ('<iii', 'FIntVector'), 'LinearColor': ('<ffff', 'FLinearColor'), 'Guid': ('<IIII', 'FGuid')}


def tpkg(prop, pkg):
    """The package whose tables a property's type references (prop.ref, prop.enum) index: the one it was read from. A
    struct member read off another package (struct_members) is not the package whose tag bytes hold the value."""
    return getattr(prop, '_tpkg', None) or pkg


def fmt_num(v):
    return B.cfloat(v) if isinstance(v, float) else str(v)


class Values:
    """Renders FPropertyTag values as C++ initializers."""
    def __init__(s, cx):
        s.cx = cx

    def name_at(s, pkg, b, o):
        i, n = struct.unpack_from('<ii', b, o)
        return pkg.names[i] + ('_%d' % (n - 1) if n else '')

    def tag(s, pkg, t, prop, cur):
        """The C++ value of tag t of property prop (of package `pkg`'s point of view for types), or None."""
        try:
            return s.value(pkg, t['type'], bytes(t['value']), prop, cur, t)
        except Exception as e:
            if os.environ.get('BPDECOMP_TRACE'): import traceback; traceback.print_exc()
            s.cx.notes.append('%s: default %s not rendered (%s)' % (cur.path, t['name'], e))
            return None

    def value(s, pkg, ty, b, prop, cur, t=None):
        if ty == 'IntProperty': return str(struct.unpack('<i', b)[0])
        if ty == 'Int64Property': return '(int64)%d' % struct.unpack('<q', b)[0]
        if ty == 'UInt64Property': return '%dULL' % struct.unpack('<Q', b)[0]
        if ty == 'FloatProperty': return B.cfloat(struct.unpack('<f', b)[0])
        if ty == 'BoolProperty': return 'true' if (t['bool'] if t else b[0]) else 'false'
        if ty == 'StrProperty': return B.cstring(read_fstring(b, 0)[0])
        if ty == 'NameProperty':
            n = s.name_at(pkg, b, 0)
            return 'FName()' if n == 'None' else B.cstring(n)
        if ty == 'TextProperty': return B.cstring(read_ftext(b, 0)[0])
        if ty in ('ByteProperty', 'EnumProperty'):
            if len(b) == 1: return str(b[0])
            entry = s.name_at(pkg, b, 0)
            if prop is None and t and t.get('enum', 'None') != 'None':     # a widget's or a slot's: the tag names its enum
                return s.cx.enum_of_tag(t['enum'], entry)
            return s.cx.enum_by_name(pkg, prop, entry)
        if ty in ('ObjectProperty', 'ClassProperty', 'InterfaceProperty'):
            idx = struct.unpack('<i', b[:4])[0]
            return 'nullptr' if idx == 0 else s.object(pkg, idx, cur)
        if ty in ('SoftObjectProperty', 'SoftClassProperty'):
            path = s.name_at(pkg, b, 0)
            sub = read_fstring(b, 8)[0]
            return B.cstring(path + (':' + sub if sub else '')) if path != 'None' else '{}'
        if ty == 'StructProperty':
            sname = t['struct'] if t else None
            return s.struct_value(pkg, sname, b, prop, cur)
        if ty in ('ArrayProperty', 'SetProperty'):
            inner = prop.subs[0] if prop is not None and prop.subs else None
            n = struct.unpack_from('<i', b, 4 if ty == 'SetProperty' else 0)[0]
            o = 8 if ty == 'SetProperty' else 4
            itype = t['inner'] if t else inner.type
            items, o = s.elements(pkg, itype, b, o, n, inner, cur)
            return '{%s}' % ', '.join(items)
        if ty == 'MapProperty':
            kp, vp = (prop.subs + [None, None])[:2] if prop is not None else (None, None)
            n = struct.unpack_from('<i', b, 4)[0]
            o = 8
            out = []
            for _ in range(n):
                k, o = s.elements(pkg, t['inner'], b, o, 1, kp, cur)
                v, o = s.elements(pkg, t['value_type'], b, o, 1, vp, cur)
                out.append('{%s, %s}' % (k[0], v[0]))
            return '{%s}' % ', '.join(out)
        raise ValueError('a %s' % ty)

    def elements(s, pkg, ty, b, o, n, prop, cur):
        out = []
        if ty == 'StructProperty' and n and prop is not None:
            sname = s.cx.struct_short_name(tpkg(prop, pkg), prop.ref)
            if sname not in IMMUTABLE and n and o + 8 <= len(b) and s._looks_tag(pkg, b, o):
                # an array of structs leads with one tag for its inner property
                tl = s.read_tag(pkg, b, o)
                o = tl['next'] - tl['size']     # its size spans the elements, which follow it
            for _ in range(n):
                if sname in IMMUTABLE:
                    fmt, cpp = IMMUTABLE[sname]
                    size = struct.calcsize(fmt)
                    out.append(s.struct_value(pkg, sname, b[o:o + size], prop, cur)); o += size
                else:
                    tags, o = s.read_tags(pkg, b, o)
                    out.append(s.struct_from_tags(pkg, prop.ref, tags, cur, tp=tpkg(prop, pkg)))
            return out, o
        for _ in range(n):
            if ty == 'IntProperty': out.append(str(struct.unpack_from('<i', b, o)[0])); o += 4
            elif ty == 'FloatProperty': out.append(B.cfloat(struct.unpack_from('<f', b, o)[0])); o += 4
            elif ty == 'Int64Property': out.append(str(struct.unpack_from('<q', b, o)[0])); o += 8
            elif ty == 'BoolProperty': out.append('true' if b[o] else 'false'); o += 1
            elif ty in ('ByteProperty', 'EnumProperty'):
                if prop is not None and (prop.type == 'EnumProperty' or prop.ref) and ty != 'ByteProperty' or \
                        (prop is not None and prop.type == 'ByteProperty' and prop.ref and len(b) - o >= 8):
                    out.append(s.cx.enum_by_name(pkg, prop, s.name_at(pkg, b, o))); o += 8
                else: out.append(str(b[o])); o += 1
            elif ty == 'NameProperty':
                nm = s.name_at(pkg, b, o); o += 8
                out.append('FName()' if nm == 'None' else B.cstring(nm))
            elif ty == 'StrProperty':
                v, o = read_fstring(b, o); out.append(B.cstring(v))
            elif ty == 'TextProperty':
                v, o = read_ftext(b, o); out.append(B.cstring(v))
            elif ty in ('ObjectProperty', 'ClassProperty', 'InterfaceProperty'):
                idx = struct.unpack_from('<i', b, o)[0]; o += 4
                out.append('nullptr' if not idx else s.object(pkg, idx, cur))
            elif ty in ('SoftObjectProperty', 'SoftClassProperty'):
                path = s.name_at(pkg, b, o); o += 8
                sub, o = read_fstring(b, o)
                out.append(B.cstring(path + (':' + sub if sub else '')))
            else: raise ValueError('an element of %s' % ty)
        return out, o

    def object(s, pkg, idx, cur):
        """An object value. One of the package's own subobjects (an instanced object, such as a damage component's
        bonus) has no asset path to name it by, and AssetGen writes no instanced object yet: the tag is noted."""
        if idx > 0 and pkg.exports[idx - 1]['outer']:
            raise ValueError('an instanced subobject %s, which AssetGen cannot write yet' % pkg.exports[idx - 1]['name'])
        return s.cx.object_ref_static(pkg, idx, cur)

    def _looks_tag(s, pkg, b, o):
        i = struct.unpack_from('<i', b, o)[0]
        return 0 <= i < len(pkg.names) and pkg.names[i] != 'None' and o + 16 < len(b) and \
            0 <= struct.unpack_from('<i', b, o + 8)[0] < len(pkg.names) and pkg.names[struct.unpack_from('<i', b, o + 8)[0]] == 'StructProperty'

    def read_tag(s, pkg, b, o):
        name = s.name_at(pkg, b, o); o += 8
        if name == 'None': return dict(name='None', next=o)
        t = dict(name=name, type=s.name_at(pkg, b, o)); o += 8
        t['size'], t['index'] = struct.unpack_from('<ii', b, o); o += 8
        if t['type'] == 'StructProperty': t['struct'] = s.name_at(pkg, b, o); o += 24
        elif t['type'] == 'BoolProperty': t['bool'] = b[o]; o += 1
        elif t['type'] in ('ByteProperty', 'EnumProperty'): t['enum'] = s.name_at(pkg, b, o); o += 8
        elif t['type'] in ('ArrayProperty', 'SetProperty'): t['inner'] = s.name_at(pkg, b, o); o += 8
        elif t['type'] == 'MapProperty': t['inner'] = s.name_at(pkg, b, o); t['value_type'] = s.name_at(pkg, b, o + 8); o += 16
        if b[o]: o += 16
        o += 1
        t['value'] = b[o:o + t['size']]
        t['next'] = o + t['size']
        return t

    def read_tags(s, pkg, b, o):
        out = []
        while True:
            t = s.read_tag(pkg, b, o)
            o = t['next']
            if t['name'] == 'None': return out, o
            out.append(t)

    def struct_value(s, pkg, sname, b, prop, cur):
        if sname in IMMUTABLE:
            fmt, cpp = IMMUTABLE[sname]
            vals = struct.unpack(fmt, b[:struct.calcsize(fmt)])
            if sname == 'Guid': return 'FGuid(%s)' % ', '.join('%d' % v for v in vals)
            return '%s(%s)' % (cpp, ', '.join(fmt_num(v) for v in vals))
        if sname == 'Color':
            bb, g, r, a = b[:4]
            return 'FColor(%d, %d, %d, %d)' % (r, g, bb, a)
        if sname == 'DateTime': return 'FDateTime(%d)' % struct.unpack('<q', b[:8])[0]
        tags, _ = s.read_tags(pkg, b, 0)
        ref = prop.ref if prop is not None else None
        return s.struct_from_tags(pkg, ref, tags, cur, sname, tp=tpkg(prop, pkg))

    def struct_from_tags(s, pkg, ref, tags, cur, sname=None, tp=None):
        tp = tp or pkg
        members = s.cx.struct_members(tp, ref) if ref else None
        if (sname or (tp.path(ref).rsplit('.', 1)[-1] if ref else None)) == 'Transform':
            return s.transform(pkg, tags, cur)
        parts = []
        for t in tags:
            mp = members.get(t['name']) if members else None
            mpkg = members.pkg if members else pkg
            v = s.value(pkg, t['type'], bytes(t['value']), mp, cur, t)
            parts.append('.%s = %s' % (ident(authored(t['name'])), v))
        tname = s.cx.types.struct(tp, ref, cur) if ref else 'F' + (sname or '')
        return '%s{%s}' % (tname, ', '.join(parts))

    def transform(s, pkg, tags, cur):
        """An FTransform written as tags, as its constructor: a member left out holds the identity's."""
        by = {t['name']: bytes(t['value']) for t in tags}
        q = struct.unpack('<ffff', by['Rotation'][:16]) if 'Rotation' in by else (0.0, 0.0, 0.0, 1.0)
        t = struct.unpack('<fff', by['Translation'][:12]) if 'Translation' in by else (0.0, 0.0, 0.0)
        sc = struct.unpack('<fff', by['Scale3D'][:12]) if 'Scale3D' in by else (1.0, 1.0, 1.0)
        f = lambda xs: ', '.join(fmt_num(x) for x in xs)
        return 'FTransform(FQuat(%s), FVector(%s), FVector(%s))' % (f(q), f(t), f(sc))


class Members(dict):
    pkg = None


# ---- the class-level context Body asks for names, members, callees

class CallInfo:
    def __init__(s):
        s.name = s.owner_cpp = s.lib = s.raw = None
        s.static = s.super_call = s.pure = False
        s.hints, s.drop, s.out = [], set(), set()
        s.container = False
        s.ret, s.ptypes = None, []          # C++ types of the return value and the parameters, where known


class Ctx:
    """What one class's bodies need: the model, the class's package and asset, its members, its ubergraph frame."""
    def __init__(s, run, asset):
        s.run, s.m, s.types, s.api = run, run.model, run.types, run.model.api
        s.asset, s.pkg = asset, asset.pkg
        s.cpp_self = asset.cpp
        s.ubergraph = None
        s.frame_props = {}
        s.api_enum_names = run.api_enum_names
        s.notes = run.notes
        s.member_idents = set()

    # names
    def local_ident(s, body, name):
        if name == '__WorldContext': return 'WorldContextObject'
        return ident(name)

    def is_param(s, body, name):
        p = body.props.get(name)
        return p is not None and p.flags & CPF_Parm

    def member_text(s, body, name):
        i = ident(name)
        st = s.pkg.struct(s.asset.export)
        if st is not None and not any(p.name == name for p in st.props):
            for cn in s.m.ancestors(s.asset.cpp)[1:]:
                hit = s.api.members(cn).get(name.lower())
                if hit: i = hit; break
        if any(ident(n) == i for n in body.props) or any(ident(n) == i for n in body.locals_used) or \
                any(v == i for v in body.frame.values()):
            return 'this->' + i
        return i

    def member_prop(s, pkg, owner, name):
        if not owner: return None, pkg
        try: at = pkg.resolve(owner)
        except Exception: at = None
        if not at: return None, pkg
        p2, k = at
        st = p2.struct(k)
        if st is None: return None, p2
        p = next((q for q in st.props if q.name == name), None)
        return p, p2

    def struct_member_ident(s, pkg, owner, name):
        return ident(authored(name))

    def other_member_ident(s, pkg, owner, name):
        return ident(name)

    def struct_members(s, pkg, ref):
        try: at = pkg.resolve(ref)
        except Exception: at = None
        out = Members()
        if not at: return out
        p2, k = at
        out.pkg = p2
        st = p2.struct(k)
        if st:
            def mark(p):
                p._tpkg = p2
                for q in (p.subs or []): mark(q)
            for p in st.props:
                mark(p)
                out[p.name] = p
        return out

    def struct_short_name(s, pkg, ref):
        return pkg.path(ref).rsplit('.', 1)[-1] if ref else None

    # enums
    def enum_entry(s, pkg, enum_idx, v):
        path = pkg.path(enum_idx)
        a = s.m.asset_of_object(path)
        if a is not None:
            e = next((n for n, val in s.run.enum_entries(a) if val == v), None)
            return '%s::%s' % (s.types.spell_mod(a, s.asset), e) if e else None
        top, _, name = path.partition('.')
        cpp = s.api.enums.get((top, name), (None,))[0]
        return s.cenum_entry(cpp, v) if cpp else None

    def cenum_entry(s, cpp, v):
        e = s.run.native_enum(cpp)
        n = next((k for k, val in e.items() if val == v), None)
        return '%s::%s' % (cpp, n) if n else None

    def enum_by_name(s, pkg, prop, entry):
        """An enum tag's value 'E::NewEnumerator3' as this decompile spells that entry."""
        enum, short = entry.split('::', 1) if '::' in entry else (None, entry)
        if prop is not None:
            idx = prop.enum if prop.type == 'EnumProperty' else prop.ref
            if idx:
                path = tpkg(prop, pkg).path(idx)
                a = s.m.asset_of_object(path)
                if a is not None:
                    for (n, val), raw in zip(s.run.enum_entries(a), s.run.enum_raw(a)):
                        if raw == short: return '%s::%s' % (s.types.spell_mod(a, s.asset), n)
                    if short == a.name + '_MAX':    # the sentinel past the last entry, which emit_enum declares
                        return '%s::%s' % (s.types.spell_mod(a, s.asset), short)
                    return '/* bpdecomp: %s */ {}' % entry
                top, _, name = path.partition('.')
                cpp = s.api.enums.get((top, name), (name,))[0]
                return '%s::%s' % (cpp, short)
        # No property to type it (a member of a native class or of a component): the tag's own name says the enum -
        # 'E::Entry' for an enum class, the bare entry for a namespaced one, which only one UeApi enum may hold.
        if enum is not None:
            a = next((x for x in s.m.assets.values() if x.kind == 'enum' and x.name == enum), None)
            if a is not None:
                for (n, val), raw in zip(s.run.enum_entries(a), s.run.enum_raw(a)):
                    if raw == short: return '%s::%s' % (s.types.spell_mod(a, s.asset), n)
            elif enum in s.run.api_enum_names and short in s.run.native_enum(enum):
                return entry
        else:
            owners = s.run.enum_owners().get(short, [])
            if len(owners) == 1: return '%s::%s' % (owners[0], short)
        return '/* bpdecomp: %s */ {}' % entry

    def enum_of_tag(s, enum, entry):
        """An enum tag's value by the enum name the tag carries, for a tag with no property at hand: a mod enum of that
        name, else UeApi's."""
        short = entry.split('::', 1)[-1]
        for a in s.m.assets.values():
            if a.kind == 'enum' and a.pkg.exports[a.export]['name'] == enum:
                for (n, val), raw in zip(s.run.enum_entries(a), s.run.enum_raw(a)):
                    if raw == short: return '%s::%s' % (s.types.spell_mod(a, s.asset), n)
        cpp = next((v[0] for (top, name), v in s.api.enums.items() if name == enum), None)
        return '%s::%s' % (cpp, short) if cpp else '/* bpdecomp: %s */ {}' % entry

    # objects
    def object_ref(s, body, idx):
        return s.object_ref_static(s.pkg, idx, s.asset)

    def object_ref_static(s, pkg, idx, cur):
        o = pkg.obj(idx)
        cls = pkg.class_of(idx)
        name = o['name']
        if cls in ('Class', 'BlueprintGeneratedClass', 'WidgetBlueprintGeneratedClass', 'AnimBlueprintGeneratedClass'):
            return '%s::StaticClass()' % s.types.cls(pkg, idx, cur)
        path = pkg.path(idx)
        if name.startswith('Default__'):
            return '%s::StaticClass()->GetDefaultObject()' % s.types.cls(pkg, o['cls'] if idx > 0 else 0, cur) \
                if idx > 0 else '/* bpdecomp: %s */ nullptr' % path
        return s.run.asset_ref(pkg, idx, cur)

    def struct_literal(s, body, sidx, vals):
        path = s.pkg.path(sidx)
        a = s.m.asset_of_object(path)
        if a is not None:
            st = a.pkg.struct(a.export)
            parts = []
            for p, v in zip(st.props, vals):
                parts.append('.%s = %s' % (ident(authored(p.name)), body.expr(v, body.hint_of_prop(a.pkg, p)).text))
            return '%s{%s}' % (s.types.spell_mod(a, s.asset), ', '.join(parts))
        top, _, name = path.partition('.')
        cpp = s.api.structs.get((top, name))
        info = s.run.native_struct(cpp) if cpp else None
        if info is None:
            s.notes.append('%s: struct literal %s' % (s.asset.path, path))
            return '/* bpdecomp: %s */ {}' % path
        fields = [f[1] for f in info['fields']]
        link = info.get('link') or fields
        by = {f: body.expr(v, body.hint_of_ctype(next((t for t, n in info['fields'] if n == f), None))).text
              for f, v in zip(link, vals)}
        return '%s(%s)' % (cpp, ', '.join(by.get(f, '{}') for f in fields))

    # calls
    def callee(s, body, n, obj):
        key = (id(n), id(obj))
        info = CallInfo()
        nargs = sum(1 for k in n.kids if k.op != 0x16)
        if n.op in (0x1B, 0x45):
            fname = next(o[1] for o in n.ops if o[0] == 'name')
            info.raw, info.name = fname, ident(fname)
            sig = None
            if obj is None:
                sig = s.run.find_mod_function(s.asset, fname)
            else:
                cls = s.static_class_of(body, obj)
                if cls is None and obj.op == 0x20:       # a by-name call on a library's default object
                    oi = next(v[1] for v in obj.ops if v[0] == 'obj')
                    if s.pkg.obj(oi)['name'].startswith('Default__'):
                        cpath = s.pkg.path(oi).replace('.Default__', '.')
                        cls = s.m.asset_of_object(cpath)
                        if cls is not None: info.static, info.owner_cpp = True, s.types.spell_mod(cls, s.asset)
                        elif s.api.classes.get(tuple(cpath.split('.', 1))):     # not the mod's: UeApi declares it
                            info.static, info.owner_cpp = True, s.api.classes[tuple(cpath.split('.', 1))]
                            nsig = s.run.native_sig(info.owner_cpp, fname, nargs)
                            if nsig:
                                ret, params, _st = nsig
                                info.ret = re.sub(r'\b(UE_\w+|static|virtual|inline)\s+', '', ret)
                                info.ptypes = [ty for ty, _ in params]
                                for k, (ty, _n) in enumerate(params):
                                    info.hints.append(body.hint_of_ctype(ty))
                                    if '&' in ty and 'const' not in ty: info.out.add(k)
                            return info
                if cls is not None: sig = s.run.find_mod_function(cls, fname)
                nsig = None
                if cls is None and not info.static:
                    t = body.type_of(obj)
                    if t and t.endswith('*'):
                        nsig = s.run.native_sig(t.replace('class ', '').rstrip('*').strip().split('::')[-1], fname, nargs)
                        if nsig:
                            ret, params, _st = nsig
                            info.ret = re.sub(r'\b(UE_\w+|static|virtual|inline)\s+', '', ret)
                            info.ptypes = [ty for ty, _ in params]
                            for k, (ty, _n) in enumerate(params):
                                info.hints.append(body.hint_of_ctype(ty))
                                if '&' in ty and 'const' not in ty: info.out.add(k)
                if not sig and not nsig and obj.op in B.VARS:    # an engine class's method (UWidget's SetVisibility): UeApi's types
                    p, ppkg = body.prop_of_var(obj)
                    native = s.types.cls(ppkg, p.ref, s.asset) if p is not None and p.type == 'ObjectProperty' else None
                    nsig = next((r for c in (s.m.ancestors(native.split('::')[-1]) if native else [])
                                 for r in [s.run.native_sig(c, fname, nargs)] if r), None)
                    if nsig:
                        info.hints = [body.hint_of_ctype(ty) for ty, _ in nsig[1]]
                        info.out = {k for k, (ty, _) in enumerate(nsig[1]) if '&' in ty and 'const' not in ty}
            if sig: s.apply_mod_sig(body, info, *sig)
            return info
        fidx = next(o[1] for o in n.ops if o[0] == 'obj')
        fobj = s.pkg.obj(fidx)
        fname = fobj['name']
        info.raw, info.name = fname, ident(fname)
        owner = fobj['outer']
        opath = s.pkg.path(owner)
        top, _, oname = opath.partition('.')
        cdo = obj is not None and obj.op == 0x20 and s.pkg.obj(next(o[1] for o in obj.ops if o[0] == 'obj'))['name'].startswith('Default__')
        oa = s.m.asset_of_object(opath)
        if oa is not None:
            info.owner_cpp = s.types.spell_mod(oa, s.asset)
            info.static = cdo
            at = s.pkg.resolve(fidx) if fidx < 0 else (s.pkg, fidx - 1)
            if not at: s.notes.append('%s: unresolved %s.%s' % (s.asset.path, opath, fname))
            if at:
                st = at[0].struct(at[1])
                info.static = bool(st.function_flags & FUNC_Static) or cdo
                info.pure = bool(st.function_flags & FUNC_BlueprintPure)
                s.apply_mod_sig(body, info, at[0], st)
            if obj is None and not info.static and oa is not s.asset and fname == body.method:
                info.super_call = True
            return info
        info.owner_cpp = s.api.classes.get((top, oname)) or ('U' + oname)
        native = top.startswith('/Script/')
        info.static = cdo or n.op == 0x68
        if obj is None and not info.static and n.op in (0x1C, 0x46) and fname == body.method: info.super_call = True
        if native and info.static: info.lib = oname
        sig = s.run.native_sig(info.owner_cpp, fname, nargs)
        if sig:
            ret, params, is_static = sig
            info.static = info.static or is_static
            info.ret, info.ptypes = re.sub(r'\b(UE_\w+|static|virtual|inline)\s+', '', ret), [ty for ty, _ in params]
            for k, (ty, pname) in enumerate(params):
                info.hints.append(body.hint_of_ctype(ty))
                if 'FLatentActionInfo' in ty: info.drop.add(k)
                if '&' in ty and 'const' not in ty: info.out.add(k)
        if info.lib in B.CONTAINER_LIBS:
            info.container = True
            if fname in ('Array_Get',): info.out.add(2)
            if fname in ('Map_Find',): info.out.add(2)
            if fname in ('Map_Keys', 'Map_Values', 'Set_ToArray'): info.out.add(1)
        info.pure = info.lib in B.PURE_MATH or (info.container and '_' in fname and fname.split('_', 1)[1] in
                                                  ('Length', 'Get', 'Contains', 'Find', 'IsValidIndex', 'LastIndex'))
        if obj is None and not info.static and fname == body.method:
            info.super_call = True      # Parent: PreConstruct, an engine class's event; unqualified it would recurse
        return info

    def apply_mod_sig(s, body, info, fpkg, st):
        k = 0
        for p in st.props:
            if p.flags & CPF_ReturnParm: info.ret = s.types.of(fpkg, p, s.asset)
            if not p.flags & CPF_Parm or p.flags & CPF_ReturnParm: continue
            info.ptypes.append(s.types.of(fpkg, p, s.asset))
            info.hints.append(body.hint_of_prop(fpkg, p))
            if p.type == 'StructProperty' and fpkg.path(p.ref).endswith('.LatentActionInfo'): info.drop.add(k)
            if p.flags & CPF_OutParm and not p.flags & CPF_ConstParm: info.out.add(k)
            k += 1

    def static_class_of(s, body, obj):
        """The mod class asset an object expression is typed as, when bytecode says (a variable of a mod class)."""
        if obj.op in B.VARS:
            p, pkg = body.prop_of_var(obj)
            if p is not None and p.type in ('ObjectProperty', 'InterfaceProperty'):
                return s.m.asset_of_object(pkg.path(p.ref))
        if obj.op == 0x51: return s.static_class_of(body, obj.kids[0])
        if obj.op == 0x17: return s.asset
        return None

    def class_of_expr(s, body, obj):
        if obj.op == 0x17: return s.cpp_self
        a = s.static_class_of(body, obj)
        return s.types.spell_mod(a, s.asset) if a else s.cpp_self

    def is_pure_call(s, body, x):
        if x.op not in B.CALLS: return True
        return s.callee(body, x, None).pure

    def is_out_arg(s, body, callnode, at):
        info = s.callee(body, callnode, None)
        args = [k for k in callnode.kids if k.op != 0x16]
        return at in info.out or (not info.hints and callnode.op in (0x1B, 0x45))

    def container_target(s, body, callnode, at):
        info = s.callee(body, callnode, None)
        return info.container and at <= 1

    def bound_delegate(s, body, d):
        if d.op not in (0x00, 0x48): return None
        key = B.var_name(d)
        binds = [n for n in body.stmts if n.op == 0x61 and n.kids[0].op in (0x00, 0x48) and B.var_name(n.kids[0]) == key]
        texts = {'%s, &%s::%s' % (body.expr(b.kids[1]).text, s.class_of_expr(body, b.kids[1]),
                                  ident(next(o[1] for o in b.ops if o[0] == 'name'))) for b in binds}
        return texts.pop() if len(texts) == 1 else None      # every Create Event into it binds the same

    def sugar(s, body, info, args, rendered):
        fn, lib = info.raw, info.lib
        o = s.api.ops.get(fn)
        if o and o['class'] == lib and not o['extra'] and len(rendered) == 2:
            p = B.BINPREC[o['op']]
            return B.E('%s %s %s' % (rendered[0].at(p), o['op'], rendered[1].at(p - 1)), p)
        if lib == 'KismetMathLibrary':
            if fn == 'Not_PreBool': return B.E('!' + rendered[0].at(B.P_UNARY), B.P_UNARY)
            if fn in ('BooleanAND', 'BooleanOR') and s.harmless(args[1]):
                op = '&&' if fn == 'BooleanAND' else '||'
                p = B.BINPREC[op]
                return B.E('%s %s %s' % (rendered[0].at(p), op, rendered[1].at(p - 1)), p)
        if lib == 'KismetSystemLibrary' and fn.startswith('MakeLiteral') and len(rendered) == 1: return rendered[0]
        if lib in B.CONTAINER_LIBS and fn.startswith(B.CONTAINER_LIBS[lib]):
            meth = B.CONTAINER_RENAME.get(fn, fn[len(B.CONTAINER_LIBS[lib]):])
            return B.E('%s.%s(%s)' % (rendered[0].at(B.P_PRIMARY), meth, ', '.join(r.text for r in rendered[1:])))
        return None

    def harmless(s, n):
        """No effect when run or skipped: variables, constants, members, math and compares of those."""
        for x in B.walk(n):
            if x.op in B.CALLS:
                info = s.callee(None if False else s._dummy_body(), x, None) if False else None
                fobj = None
                if x.op in (0x1C, 0x46, 0x68):
                    fobj = s.pkg.obj(next(o[1] for o in x.ops if o[0] == 'obj'))
                    owner = s.pkg.path(fobj['outer'])
                    if owner == '/Script/Engine.KismetMathLibrary': continue
                return False
            if x.op in (0x19, 0x1A, 0x6B): return False
        return True


# ---- the run: every asset of every mod

class Run:
    def __init__(s, model, raw_dir=None, keep=()):
        s.model, s.api = model, model.api
        s.types = Types(model)
        s.notes = []
        s.raw_dir = raw_dir
        s.keep = set(keep)
        s.assets_used = defaultdict(dict)     # mod -> {path: (class C++, C++ ns path, name)}
        s._native_enum, s._native_sig, s._native_struct = {}, {}, {}
        s.api_enum_names = {v[0] for v in s.api.enums.values()}
        s._entries = {}

    # enums
    def enum_raw(s, a):
        st = a.pkg
        i = a.export
        tags = st.tags(i)
        r = invariants.Rd(st.blob(i), tags.end)
        if r.i32(): r.o += 16
        n = r.i32()
        out = []
        for _ in range(n):
            nm = st._name(r); v = r.i64()
            if not nm.endswith('_MAX'): out.append(nm.split('::', 1)[1])
        return out

    def enum_entries(s, a):
        """[(C++ enumerator, value)] of a mod enum: its display names (from --names) as identifiers, else its own."""
        if a.path in s._entries: return s._entries[a.path]
        st = a.pkg
        i = a.export
        tags = st.tags(i)
        r = invariants.Rd(st.blob(i), tags.end)
        if r.i32(): r.o += 16
        n = r.i32()
        raw = [(st._name(r), r.i64()) for _ in range(n)]
        disp = s.model.display_names(a)
        out, seen = [], set()
        for nm, v in raw:
            short = nm.split('::', 1)[1]
            if short.endswith('_MAX') and short == a.name + '_MAX': continue
            name = disp.get(short, short)
            c = ident(name)
            if c == 'None': c = 'None_'
            if c[0] == '_' and name[:1].isdigit(): c = a.name.lstrip('E')[:1] + c
            while c in seen: c += '_'
            seen.add(c)
            out.append((c, v))
        s._entries[a.path] = out
        return out

    def enum_owners(s):
        """{enumerator: [UeApi enums declaring it]}."""
        if not hasattr(s, '_owners'):
            s._owners = defaultdict(list)
            for text in s.api._text.values():
                for cpp, body in re.findall(r'^enum\s+(?:class\s+)?(\w+)\s*(?::\s*\w+)?\s*\{([^}]*)\}', text, re.M):
                    for nm in re.findall(r'^\s*(\w+)\s*=', body, re.M): s._owners[nm].append(cpp)
        return s._owners

    def native_enum(s, cpp):
        if cpp in s._native_enum: return s._native_enum[cpp]
        out = {}
        for rel, text in s.api._text.items():
            m = re.search(r'^enum\s+(?:class\s+)?%s\s*(?::\s*\w+)?\s*\{([^}]*)\}' % re.escape(cpp), text, re.M)
            if m:
                for nm, v in re.findall(r'^\s*(\w+)\s*=\s*(-?(?:0x[0-9a-fA-F]+|\d+))', m.group(1), re.M):
                    out[nm] = int(v, 0)
                break
        s._native_enum[cpp] = out
        return out

    def native_struct(s, cpp):
        if cpp in s._native_struct: return s._native_struct[cpp]
        import json
        types = json.load(open(os.path.join(s.api.dir, 'Types.json'), encoding='utf-8'))
        for k, v in types['structs'].items(): s._native_struct[k] = v
        return s._native_struct.get(cpp)

    def native_sig(s, cls, fn, nargs):
        """(return type, [(type, name)], static) of a UeApi method of class cls with nargs parameters, or the one
        with the most when none has nargs; None when UeApi has no such method."""
        key = (cls, fn, nargs)
        if key in s._native_sig: return s._native_sig[key]
        rel = s.api.header_of(cls)
        res = None
        if rel:
            text = s.api._text[rel]
            short = cls.split('::')[-1]
            m = re.search(r'^class %s\b.*?^};' % re.escape(short), text, re.M | re.S)
            body = m.group(0) if m else text
            cands = []
            for mm in re.finditer(r'^\s*(static\s+)?([\w:<>,\s\*&]+?)\s+%s\(([^;{]*)\)\s*(?:const)?\s*[;{]' % re.escape(fn), body, re.M):
                params = split_params(mm.group(3))
                cands.append((mm.group(2).strip(), params, bool(mm.group(1))))
            if cands:
                exact = [c for c in cands if len(c[1]) == nargs]
                res = exact[0] if exact else max(cands, key=lambda c: len(c[1]))
        base = s.api.bases.get(cls.split('::')[-1])
        if res is None and base:            # declared on a parent: UBorder's SetVisibility is UWidget's
            res = s.native_sig(base, fn, nargs)
        s._native_sig[key] = res
        return res

    def find_mod_function(s, a, fname):
        """(package, struct) of the function named fname a mod class asset or its mod parents declare."""
        seen = 0
        while a is not None and seen < 20:
            seen += 1
            for i, e in enumerate(a.pkg.exports):
                if e['name'] == fname and a.pkg.class_of(i + 1) == 'Function':
                    return a.pkg, a.pkg.struct(i)
            st = a.pkg.struct(a.export)
            if st is None or not st.super: return None
            a = s.model.asset_of_object(a.pkg.path(st.super))
        return None

    def asset_ref(s, pkg, idx, cur):
        """The C++ name of a UE_ASSET_AT for the asset an index names, recorded for the mod's Assets.h."""
        path = pkg.path(idx)
        package, _, obj = path.partition('.')
        cls = s.types.cls(pkg, pkg.obj(idx)['cls'], cur) if idx > 0 else None
        if idx < 0:
            o = pkg.obj(idx)
            a = s.model.asset_of_object(o['class_package'] + '.' + o['class_name'])   # the mod's own class first:
            cls = s.types.spell_mod(a, None) if a else \
                s.api.classes.get(('/Script/' + o['class_package'].split('/')[-1], o['class_name'])) or \
                s.api.classes.get((o['class_package'], o['class_name'])) or 'UObject'   # the modded dump has it too
        ns = ['Assets'] + [ident(p) for p in package.split('/')[2:-1]]
        name = ident(obj)
        mod = cur.mod.name if cur is not None else ''
        s.assets_used[mod][path] = (cls, ns, name)
        text = '::'.join(ns + [name])
        return '&' + text


def split_params(text):
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch in '<(': depth += 1
        elif ch in '>)': depth -= 1
        if ch == ',' and depth == 0: out.append(cur); cur = ''
        else: cur += ch
    if cur.strip(): out.append(cur)
    res = []
    for p in out:
        p = p.split('=')[0].strip()
        m = re.match(r'^(.*?)(\w+)$', p)
        res.append((m.group(1).strip(), m.group(2)) if m else (p, ''))
    return res
