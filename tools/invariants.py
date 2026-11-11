#!/usr/bin/env python3
"""usage: invariants.py <dir or base>... [--sample N] [--only RULE,...] [--game <FSD/Content>] [--ueapi <UeApi>]
                        [--sdk <Dumper-7 dump>] [--list]

Checks what UE 4.27 relies on when it loads and runs a cooked Blueprint class, on every package under each dir: the
rules below, each citing the engine source that makes it one. test_bytecode.py runs them over every suite package.
Pointed at the game's own extracted content (D:/DRGExtract/FSD-WindowsNoEditor/FSD/Content), it calibrates them: a rule
Epic's cooked Blueprints break is a wrong rule, not a finding. --sample N checks every Nth package there.

As a module: Package(base) is one cooked package read into tables, check(pkg) -> [(rule, export, message)]."""
import glob, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uasset_io
from uasset import W, W5
from uasset_io import R as Rd


# ---- the package, read into tables


class Prop:
    """One FProperty off a struct's ChildProperties (FProperty::Serialize and its type's tail)."""
    def __repr__(s): return '%s %s' % (s.type, s.name)


class Node:
    """One bytecode expression: its opcode, where it starts in memory and on disk, its size in memory, its operands in
    the order the grammar reads them, and its sub-expressions."""
    def __init__(s, op, mem, disk): s.op, s.mem, s.disk, s.size, s.ops, s.kids = op, mem, disk, 0, [], []
    def walk(s):
        yield s
        for k in s.kids: yield from k.walk()

    def on_self(s, context=False):
        """(node, whether it runs on this object) for the node and everything under it: the expression an EX_Context
        guards runs on the context object, so a call by name there is looked up on that object's class."""
        yield s, not context
        for j, k in enumerate(s.kids):
            yield from k.on_self(context or (s.op in (0x12, 0x19, 0x1A) and j == 1))


class Walk(W):
    """uasset.W's bytecode grammar, keeping every expression as a Node instead of a log line."""
    def __init__(s, b, o, pkg):
        W.__init__(s, b, o, pkg.names, pkg.dumps_imports, pkg.dumps_exports)
        s.stack = [Node(None, 0, o)]
    def expr(s):
        n = Node(s.b[s.o], s.mem, s.o)
        s.stack[-1].kids.append(n); s.stack.append(n)
        try: op = super().expr()
        finally: s.stack.pop()
        n.size = s.mem - n.mem
        return op
    def i32(s): s.stack[-1].ops.append(('int', struct.unpack_from('<i', s.b, s.o)[0], s.o)); return W.i32(s)
    def u16(s): v = W.u16(s); s.stack[-1].ops.append(('int', v)); return v
    def ptr(s):
        v = struct.unpack_from('<i', s.b, s.o)[0]
        s.stack[-1].ops.append(('obj', v))
        return W.ptr(s)
    def name(s):
        v = W.name(s); s.stack[-1].ops.append(('name', v)); return v
    def fieldpath(s):
        o, segs = s.o, []
        cnt = struct.unpack_from('<i', s.b, o)[0]; o += 4
        for _ in range(cnt):
            i, num = struct.unpack_from('<ii', s.b, o); o += 8
            segs.append(s.names[i] + ('_%d' % (num - 1) if num else ''))
        s.stack[-1].ops.append(('prop', segs, struct.unpack_from('<i', s.b, o)[0]))
        return W.fieldpath(s)


class Walk5(Walk, W5):
    """Walk over uasset.W5's 5.6 operands (doubles, EX_Vector3fConst, EX_BitFieldConst): a node of those has no
    operands or kids listed, and no rule reads one."""


class Struct:
    """A UStruct export's body - UClass, UFunction, UScriptStruct - read exactly as UStruct::Serialize and its
    subclass write it (Class.cpp 1796, 4394, 5682, 2692). `end` is where the reading stopped."""


class Package:
    FUNCTION_CLASSES = ('Function', 'DelegateFunction', 'SparseDelegateFunction')

    def __init__(s, base):
        s.base = base
        ua, ue, total, names, imports, exports = uasset_io.load(base)
        s.ua, s.ue, s.total, s.names = ua, ue, total, names
        s.dumps_imports, s.dumps_exports = imports, exports
        sm = uasset_io.summary(ua)
        s.file_version, s.licensee, s.custom_versions = sm['ue4'], sm['licensee'], len(sm['customs'])
        s.total_header_size, s.folder, s.package_flags = sm['total'], sm['folder'], sm['flags']
        s.name_count, s.name_offset = sm['ncount'], sm['noff']
        s.export_count, s.export_offset = sm['ecount'], sm['eoff']
        s.import_count, s.import_offset = sm['icount'], sm['ioff']
        _, irows, erows = uasset_io.tables(ua, sm)
        fname = lambda v: names[v[0]] + ('_%d' % (v[1] - 1) if v[1] else '')
        s.imports = [dict(class_package=fname(i['class_package']), class_name=fname(i['class_name']), outer=i['outer'],
                          name=fname(i['name'])) for i in irows]
        s.exports = []
        deps = uasset_io.preload(base) if s.package_flags & 0x80000000 else [[[], [], [], []]] * s.export_count
        for k, r in enumerate(erows):
            e = {f: r[f] for f in ('cls', 'super', 'tmpl', 'outer')}
            e['name'] = fname(r['name'])
            e.update((f, r[f]) for f in ('flags', 'size', 'off', 'forced', 'not_client', 'not_server', 'package_flags',
                                         'not_always_loaded', 'is_asset'))
            e['deps'] = deps[k]                                 # serBeforeSer, createBeforeSer, serBeforeCreate, createBeforeCreate
            s.exports.append(e)
        s.ue5, s.unversioned_props = sm['ue5'], bool(sm['ue5'] and s.package_flags & uasset_io.UNVERSIONED_PROPERTIES)
        s._structs, s._tags = {}, {}

    def _name(s, r):
        i, num = r.i32(), r.i32()
        return s.names[i] + ('_%d' % (num - 1) if num else '')

    # -- FPackageIndex

    def obj(s, idx):
        """The import or export row an FPackageIndex names, or None."""
        if idx < 0: return s.imports[-idx - 1]
        if idx > 0: return s.exports[idx - 1]
        return None

    def path(s, idx):
        """An FPackageIndex as a full path: /Script/Engine.Actor:ReceiveBeginPlay for an import, the export's own
        path under this package's name for an export."""
        if idx == 0: return None
        o = s.obj(idx)
        if o['outer'] == 0:
            return o['name'] if idx < 0 else s.package_name() + '.' + o['name']
        up = s.path(o['outer'])
        return up + (':' if '.' in up else '.') + o['name']

    def class_of(s, idx):
        """The class name of the object an FPackageIndex names: an import's ClassName, an export's class's name."""
        o = s.obj(idx)
        if idx < 0: return o['class_name']
        c = s.obj(o['cls'])
        return c['name'] if c else 'Class'

    def package_name(s):
        return '/Game/' + s.base.replace(os.sep, '/').split('/Content/')[-1]

    def resolve(s, idx):
        """(Package, export index) of the object an FPackageIndex names: an export of this package, or a /Game import
        found in the Content folder this package sits in, else in GAME_CONTENT (--game). None for a /Script import, or a
        package not found."""
        if idx > 0: return s, idx - 1
        if idx == 0: return None
        top = idx
        while s.obj(top)['outer']: top = s.obj(top)['outer']
        name = s.obj(top)['name']
        if not name.startswith('/Game/'): return None
        here = s.base.replace(os.sep, '/')
        for root in [here[:here.rindex('/Content/') + len('/Content')]] + GAME_CONTENT:
            if not os.path.exists(root + name[len('/Game'):] + '.uasset'): continue
            other = load(root + name[len('/Game'):])
            want = s.path(idx).lower()
            k = next((k for k in range(len(other.exports)) if other.path(k + 1).lower() == want), None)
            return (other, k) if k is not None else None
        return None

    def blob(s, i):
        e = s.exports[i]
        return s.ue[e['off'] - s.total: e['off'] - s.total + e['size']]

    def find(s, name):
        return next((i for i, e in enumerate(s.exports) if e['name'] == name), None)

    # -- payloads

    def tags(s, i, start=0):
        """The export's FPropertyTags from `start` to None, as dicts (PropertyTag.cpp): name, type, size, index,
        struct / enum / inner / key / value type names, bool, the value's bytes. The end offset in .end of the list.
        A UE5 package is read as 5.6 writes it (_tags5); an unversioned one has no tags to list (_unversioned)."""
        if (i, start) in s._tags: return s._tags[i, start]
        if getattr(s, 'unversioned_props', False): out = s._unversioned(i, start)
        elif getattr(s, 'ue5', False): out = s._tags5(i, start)
        else: out = s._tags4(i, start)
        s._tags[i, start] = out
        return out

    def _unversioned(s, i, start):
        """RogueCore's own packages hold unversioned properties (PKG_UnversionedProperties): a header of u16 fragments
        (skip 7 bits, has-zeroes 0x80, last 0x100, value count 7 bits) and a zero mask, then the values in the class's
        property order, which only the class's schema tells apart. So no tag list: .end is past the header, and a
        header that counts values leaves .end None (the payload is opaque past it). A UClass or UFunction export counts
        none (a UClass skips its 13 properties, all default)."""
        r, values, zeroes = Rd(s.blob(i), start), 0, False
        while True:
            f = r.u16()
            values += f >> 9
            zeroes = zeroes or bool(f & 0x80)
            if f & 0x100: break
        if zeroes: r.o += 1 if values <= 8 else 2 if values <= 16 else 4 * ((values + 31) // 32)
        out = TagList()
        out.header_end = r.o
        out.end = None if values else r.o
        return out

    def _tags5(s, i, start):
        """The 5.4+ tagged grammar: a control byte before an object's list, then per tag the name, the type as a
        pre-order list of (FName, inner count) nodes, the size and a flags byte (HasArrayIndex 1, HasPropertyGuid 2,
        HasPropertyExtensions 4, BoolTrue 0x10), then the index / guid / extensions if flagged."""
        b, out = s.blob(i), TagList()
        r = Rd(b, start)
        if start == 0: r.u8()                               # SerializationControlExtensions
        def node():
            name = s._name(r)
            return name, [node() for _ in range(r.i32())]
        while True:
            name = s._name(r)
            if name == 'None': break
            (kind, kids), t = node(), dict(name=name)
            t['type'] = kind
            t['size'] = r.i32()
            flags = r.u8()
            t['index'] = r.i32() if flags & 1 else 0
            if kind == 'StructProperty' and kids: t['struct'] = kids[0][0]; t['struct_guid'] = b''
            elif kind == 'BoolProperty': t['bool'] = 1 if flags & 0x10 else 0
            elif kind in ('ByteProperty', 'EnumProperty') and kids: t['enum'] = kids[0][0]
            elif kind in ('ArrayProperty', 'SetProperty') and kids:
                t['inner'] = kids[0][0]
                if kids[0][1]: t['enum' if kids[0][0] in ('EnumProperty', 'ByteProperty') else 'struct'] = kids[0][1][0][0]
            elif kind == 'MapProperty' and len(kids) == 2: t['inner'], t['value_type'] = kids[0][0], kids[1][0]
            if flags & 2: r.o += 16                         # property guid
            if flags & 4: r.o += 1                          # extensions: a flags byte, 0 for what AssetGen writes
            t['at'] = r.o; t['value'] = b[r.o:r.o + t['size']]; r.o += t['size']
            out.append(t)
        out.end = r.o
        return out

    def _tags4(s, i, start):
        b, r, out = s.blob(i), None, TagList()
        r = Rd(b, start)
        while True:
            name = s._name(r)
            if name == 'None': break
            t = dict(name=name, type=s._name(r))
            t['size'], t['index'] = r.i32(), r.i32()
            if t['type'] == 'StructProperty': t['struct'] = s._name(r); t['struct_guid'] = b[r.o:r.o + 16]; r.o += 16
            elif t['type'] == 'BoolProperty': t['bool'] = r.u8()
            elif t['type'] in ('ByteProperty', 'EnumProperty'): t['enum'] = s._name(r)
            elif t['type'] in ('ArrayProperty', 'SetProperty'): t['inner'] = s._name(r)
            elif t['type'] == 'MapProperty': t['inner'] = s._name(r); t['value_type'] = s._name(r)
            if r.u8(): r.o += 16                            # property guid
            t['at'] = r.o; t['value'] = b[r.o:r.o + t['size']]; r.o += t['size']
            out.append(t)
        out.end = r.o
        return out

    def tag(s, i, name):
        return next((t for t in s.tags(i) if t['name'] == name), None)

    def is_struct(s, i):
        return s.class_of(i + 1) in s.FUNCTION_CLASSES + ('BlueprintGeneratedClass', 'WidgetBlueprintGeneratedClass',
                                                            'AnimBlueprintGeneratedClass', 'UserDefinedStruct', 'ScriptStruct')

    def struct(s, i):
        """Export i's UStruct body (see Struct), or None if it is not a struct."""
        if i in s._structs: return s._structs[i]
        if not s.is_struct(i): return None
        st, b = Struct(), s.blob(i)
        st.kind = s.class_of(i + 1)
        tl = s.tags(i)
        st.tags = tl
        st.size = len(b)
        if tl.end is not None: s._struct_body(i, st, Rd(b, tl.end))
        else:
            # An unversioned class whose header counts values (BlueprintGeneratedClass's NumReplicatedProperties ...):
            # their layout is the class's schema, which no table here holds. The body that follows starts with the lazy
            # guid bool, and is the one start from which it reads to exactly the export's end.
            for c in range(tl.header_end, min(len(b) - 8, tl.header_end + UNVERSIONED_SEARCH)):
                if b[c:c + 4] != bytes(4): continue
                try: s._struct_body(i, st, Rd(b, c))
                except (IndexError, ValueError, struct.error, MemoryError, OverflowError): continue
                if st.end == st.size: break
            else: raise ValueError('export %d: no struct body found past its unversioned header' % i)
        s._structs[i] = st
        return st

    def _struct_body(s, i, st, r):
        """The UStruct body from the lazy-guid bool on, into st; ValueError where it is no struct body."""
        b = r.b
        if r.i32(): r.o += 16                               # FLazyObjectPtr::PossiblySerializeObjectGuid
        st.super = r.i32()
        if abs(st.super) > len(s.imports) + len(s.exports): raise ValueError('super')
        n = r.i32()
        if not 0 <= n <= len(s.exports): raise ValueError('children')
        st.children = [r.i32() for _ in range(n)]
        n = r.i32()
        if not 0 <= n <= 4096: raise ValueError('props')
        st.props = [s._prop(r, s._name(r)) for _ in range(n)]
        st.script_mem, st.script_disk = r.i32(), r.i32()
        if not 0 <= st.script_disk <= len(b) - r.o: raise ValueError('script')
        st.script_at = r.o
        r.o += st.script_disk
        if st.kind in s.FUNCTION_CLASSES:
            st.function_flags = r.u32()
            if st.function_flags & 0x40: r.u16()            # FUNC_Net: the unused RepOffset
            st.event_graph, st.event_graph_offset = r.i32(), r.i32()
            if st.kind == 'SparseDelegateFunction': st.owning_class, st.delegate_name = s._name(r), s._name(r)
        elif st.kind in ('UserDefinedStruct', 'ScriptStruct'):
            st.struct_flags = r.u32()
            status = s.tag(i, 'Status')
            if s.unversioned_props: st.defaults, r.o = None, len(b)     # the default instance is unversioned too: opaque
            elif st.kind == 'UserDefinedStruct' and (not status or s.names[struct.unpack_from('<i', status['value'])[0]]
                                                   .endswith('UDSS_UpToDate')):
                st.defaults = s.tags(i, r.o)                # the default instance, UUserDefinedStruct::Serialize
                r.o = st.defaults.end
        else:
            st.func_map = [(s._name(r), r.i32()) for _ in range(r.i32())]
            st.class_flags, st.within, st.config = r.u32(), r.i32(), s._name(r)
            st.generated_by = r.i32()
            st.interfaces = [(r.i32(), r.i32(), r.i32()) for _ in range(r.i32())]   # Class, PointerOffset, bImplementedByK2
            st.force_script_order, st.dummy, st.cooked, st.cdo = r.i32(), s._name(r), r.i32(), r.i32()
            if s.ue5 and s.package_flags & uasset_io.FILTER_EDITOR_ONLY:
                # UBlueprintGeneratedClass::Serialize's CookedEditorTags, a TMap<FName, FString> (the runtime's is empty)
                st.cooked_editor_tags = [(s._name(r), r.fstr()) for _ in range(r.i32())]
        st.end = r.o

    def _prop(s, r, ftype):
        p = Prop()
        if not ftype.endswith('Property'): raise ValueError('field class %r' % ftype)
        p.type, p.name, p.objflags = ftype, s._name(r), r.u32()
        p.dim, p.elem_size, p.flags, p.rep_index = r.i32(), r.i32(), r.u64(), r.u16()
        p.notify, p.cond = s._name(r), r.u8()
        p.ref = p.meta = p.enum = 0
        p.subs, p.bool = [], None
        if ftype in ('ObjectProperty', 'WeakObjectProperty', 'LazyObjectProperty', 'SoftObjectProperty',
                     'InterfaceProperty', 'StructProperty', 'ByteProperty', 'DelegateProperty',
                     'MulticastInlineDelegateProperty', 'MulticastSparseDelegateProperty', 'MulticastDelegateProperty'):
            p.ref = r.i32()
        elif ftype in ('ClassProperty', 'SoftClassProperty'):
            p.ref, p.meta = r.i32(), r.i32()
        elif ftype == 'BoolProperty':
            p.bool = tuple(r.b[r.o:r.o + 6]); r.o += 6      # FieldSize, ByteOffset, ByteMask, FieldMask, BoolSize, NativeBool
        elif ftype == 'EnumProperty':
            p.enum = r.i32()
        elif ftype == 'FieldPathProperty':
            p.field_class = s._name(r)
        for _ in range({'ArrayProperty': 1, 'SetProperty': 1, 'MapProperty': 2, 'EnumProperty': 1}.get(ftype, 0)):
            p.subs.append(s._prop(r, s._name(r)))
        return p

    def script(s, i):
        """Export i's bytecode as a list of top-level statements (Nodes); [] for a struct without script."""
        st = s.struct(i)
        if not st or not st.script_disk: return []
        w = (Walk5 if s.ue5 else Walk)(s.blob(i), st.script_at, s)
        end = st.script_at + st.script_disk
        while w.o < end: w.expr()
        st.walked_mem, st.walked_end = w.mem, w.o
        return w.stack[0].kids


class TagList(list):
    end = 0
    header_end = 0


UNVERSIONED_SEARCH = 1 << 16        # how far past an unversioned header Package.struct looks for a struct body
GAME_CONTENT = []           # the game's own FSD/Content folders, where Package.resolve looks for a /Game import last
# What the rules know of the engine's own classes, which no cooked package carries: the mod API headers (UeApi) and a
# Dumper-7 dump of the game (its SDK/SDK and GObjects-Dump-WithProperties.txt). Read at import, from the environment
# test_bytecode.py sets (--ueapi, --sdk) or the flags below; a rule that needs a missing one skips the native part.
UEAPI_DIR = os.environ.get('INVARIANTS_UEAPI') or os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                                                '..', '..', 'BpMods', 'UeApi'))
SDK_DUMP = os.environ.get('INVARIANTS_SDK') or None
_LOADED = {}


def load(base):
    """Package(base), read once per process: rules that follow a parent class into its own package share it."""
    key = os.path.normcase(os.path.abspath(base))
    if key not in _LOADED: _LOADED[key] = Package(base)
    return _LOADED[key]


# ---- the rules. Each takes (pkg) and yields (export index, message); RULES maps its name to it.

RULES = {}


def rule(fn):
    RULES[fn.__name__] = fn
    return fn


@rule
def struct_body_exact(pkg):
    """A class, function or struct export reads to exactly its SerialSize, and its script walks to exactly the two
    header sizes: the loader stops at ScriptBytecodeSize (StructScriptLoader.cpp) and a short or long payload is
    `Serial size mismatch`, a Fatal (LinkerLoad.cpp)."""
    for i in range(len(pkg.exports)):
        st = pkg.struct(i)
        if not st: continue
        if st.end != st.size: yield i, 'payload read to %d of %d bytes' % (st.end, st.size)
        pkg.script(i)
        if st.script_disk and (st.walked_end - st.script_at, st.walked_mem) != (st.script_disk, st.script_mem):
            yield i, 'script walks to disk %d mem %d, header says %d / %d' % (
                st.walked_end - st.script_at, st.walked_mem, st.script_disk, st.script_mem)



def functions(pkg):
    """(export index, Struct) of every UFunction export."""
    for i in range(len(pkg.exports)):
        if pkg.class_of(i + 1) in Package.FUNCTION_CLASSES: yield i, pkg.struct(i)


def statements(pkg, i):
    """Every expression of export i's script, nested ones included, with the top-level statements' offsets."""
    top = pkg.script(i)
    return [n for t in top for n in t.walk()], {t.mem for t in top}


@rule
def jump_targets(pkg):
    """EX_Jump, EX_JumpIfNot and EX_PushExecutionFlow go to a statement of their own script: the VM sets Code to
    &Script[Offset] unchecked (ScriptCore.cpp execJump 2374, execJumpIfNot 2398, execPushExecutionFlow 2445)."""
    for i, st in functions(pkg):
        nodes, starts = statements(pkg, i)
        for n in nodes:
            if n.op in (0x06, 0x07, 0x4C):
                target = n.ops[0][1]
                if target not in starts:
                    yield i, 'op %02x at mem %d jumps to %d, not a statement' % (n.op, n.mem, target)


@rule
def context_skip(pkg):
    """An EX_Context / EX_Context_FailSilent / EX_ClassContext skip count is the in-memory size of the expression it
    guards: on a null context the VM steps over exactly that many bytes (ScriptCore.cpp ProcessContextOpcode 2878)."""
    for i, st in functions(pkg):
        for n in statements(pkg, i)[0]:
            if n.op in (0x12, 0x19, 0x1A):
                skip = next(v for k, v, *_ in n.ops if k == 'int')
                if skip != n.kids[1].size:
                    yield i, 'context at mem %d skips %d, the expression is %d' % (n.mem, skip, n.kids[1].size)


@rule
def local_operands(pkg):
    """EX_LocalVariable and EX_LocalOutVariable name a property of the function running them: the VM offsets
    Stack.Locals by it, or looks it up in Stack.OutParms with no failure path in a shipping build (ScriptCore.cpp
    execLocalVariable 2050, execLocalOutVariable 2170). An out variable is a CPF_OutParm parameter."""
    for i, st in functions(pkg):
        own = {p.name: p for p in st.props}
        for n in statements(pkg, i)[0]:
            if n.op not in (0x00, 0x48): continue
            _, segs, owner = n.ops[0]
            if owner != i + 1 or segs[-1] not in own:
                yield i, 'op %02x at mem %d reads %s of %s' % (n.op, n.mem, '.'.join(segs), pkg.path(owner))
            elif n.op == 0x48 and not own[segs[-1]].flags & 0x100:
                yield i, 'EX_LocalOutVariable at mem %d reads %s, not an out parameter' % (n.mem, segs[-1])


@rule
def script_terminated(pkg):
    """A script ends in EX_Return then EX_EndOfScript, once: the VM runs a function until EX_Return
    (ProcessLocalScriptFunction), and the editor's backend closes every script with both."""
    for i, st in functions(pkg):
        top = pkg.script(i)
        if not top: continue
        if top[-1].op != 0x53 or [t.op for t in top].count(0x53) != 1:
            yield i, 'script does not end in one EX_EndOfScript'
        elif len(top) < 2 or top[-2].op != 0x04:
            yield i, 'EX_EndOfScript not preceded by EX_Return'



CPF_Parm, CPF_OutParm, CPF_ReturnParm = 0x80, 0x100, 0x400


@rule
def params_lead(pkg):
    """A function's parameters are the first of its properties: the VM steps call arguments into ChildProperties in
    order until EX_EndFunctionParms (ScriptCore.cpp 852), and UFunction counts NumParms / ParmsSize over the leading
    run of CPF_Parm properties only. A local between two parameters would take an argument meant for the next."""
    for i, st in functions(pkg):
        flags = [p.flags & CPF_Parm for p in st.props]
        if flags != sorted(flags, reverse=True): yield i, 'a local precedes a parameter: %s' % [p.name for p in st.props]


@rule
def unique_names(pkg):
    """No two properties of a function, class or struct share an FName, which ignores case: a local or member operand is a
    field path the VM resolves by name to the first match (FFieldPath::ResolveField walks ChildProperties), so a second
    property of the name is never itself. Two locals both named Keys, a TArray<FString> and a TArray<float>, had the
    float branch write floats into the string array, and DRG crashed in Array_Add."""
    for i in range(len(pkg.exports)):
        st = pkg.struct(i)
        if not st: continue
        seen = set()
        for p in st.props:
            if p.name.lower() in seen: yield i, 'two properties named %s' % p.name
            seen.add(p.name.lower())


def callee(pkg, i, n):
    """The Struct of the function a call node reaches when this package holds it: EX_LocalFinalFunction /
    EX_FinalFunction / EX_CallMath by export, EX_LocalVirtualFunction / EX_VirtualFunction by name among the calling
    function's class's own functions. None otherwise (a native, or another package's)."""
    kind, v = n.ops[0][:2]
    if n.op in (0x1C, 0x46, 0x68) and kind == 'obj' and v > 0:
        return pkg.struct(v - 1)
    if n.op in (0x1B, 0x45) and kind == 'name':
        owner = pkg.exports[i]['outer']
        j = next((k for k, e in enumerate(pkg.exports) if e['name'] == v and e['outer'] == owner
                  and pkg.class_of(k + 1) in Package.FUNCTION_CLASSES), None)
        return pkg.struct(j) if j is not None else None
    return None


@rule
def call_arity(pkg):
    """A call to a function of this package passes at most one argument per parameter, the return value left out: a
    further one is stepped into the callee's locals, or past the last property (ScriptCore.cpp 852). Fewer is legal -
    the rest stay as the zeroed frame has them - and the game's own cooked Blueprints do it (WND_SeasonLevels)."""
    for i, st in functions(pkg):
        for n, mine in (x for t in pkg.script(i) for x in t.on_self()):
            if n.op not in (0x1B, 0x1C, 0x45, 0x46, 0x68) or (n.op in (0x1B, 0x45) and not mine): continue
            f = callee(pkg, i, n)
            if f is None: continue
            want = sum(1 for p in f.props if p.flags & CPF_Parm and not p.flags & CPF_ReturnParm)
            got = sum(1 for k in n.kids if k.op != 0x16)
            if got > want: yield i, 'call at mem %d passes %d arguments to %d parameters' % (n.mem, got, want)


def classes(pkg):
    for i in range(len(pkg.exports)):
        st = pkg.struct(i)
        if st and hasattr(st, 'class_flags'): yield i, st


@rule
def class_flags_loadable(pkg):
    """A loaded class is never CLASS_Native or CLASS_Intrinsic (checkf, Class.cpp 4486), and a Blueprint's is
    CLASS_CompiledFromBlueprint."""
    for i, st in classes(pkg):
        if st.class_flags & 0x10000080: yield i, 'ClassFlags %#x: Native / Intrinsic' % st.class_flags
        if not st.class_flags & 0x40000: yield i, 'ClassFlags %#x: not CompiledFromBlueprint' % st.class_flags


@rule
def class_default_object(pkg):
    """The class tail names its CDO: an export of this class called Default__<Class>, RF_Public |
    RF_ClassDefaultObject | RF_ArchetypeObject, outside the class (its outer is the package)."""
    for i, st in classes(pkg):
        e = pkg.exports[i]
        if st.cdo <= 0: yield i, 'CDO %d is not an export' % st.cdo; continue
        c = pkg.exports[st.cdo - 1]
        if c['cls'] != i + 1 or c['name'] != 'Default__' + e['name'] or c['flags'] & 0x31 != 0x31 or c['outer'] != 0:
            yield i, 'CDO %s class %s flags %#x outer %d' % (c['name'], pkg.path(c['cls']), c['flags'], c['outer'])


@rule
def class_children(pkg):
    """Children are the class's own UFunctions (their outer is the class), and FuncMap lists exactly them by name
    (UClass::Serialize reads FuncMap as is, Class.cpp 4413; FindFunctionByName looks it up)."""
    for i, st in classes(pkg):
        kids = {}
        for c in st.children:
            if c <= 0 or pkg.exports[c - 1]['outer'] != i + 1:
                yield i, 'child %s is not an export inside the class' % pkg.path(c); continue
            if pkg.class_of(c) in Package.FUNCTION_CLASSES: kids[pkg.exports[c - 1]['name']] = c
        if dict(st.func_map) != kids:
            yield i, 'FuncMap %s, function children %s' % (sorted(dict(st.func_map)), sorted(kids))


def check(pkg, only=None):
    """Every rule's findings on one package, as (rule, export name, message)."""
    out = []
    for name, fn in RULES.items():
        if only and name not in only: continue
        try:
            for i, msg in fn(pkg):
                out.append((name, pkg.exports[i]['name'] if i is not None else '-', msg))
        except Exception as e:                              # a rule that cannot read the package is a finding too
            out.append((name, '-', 'raised %s: %s' % (type(e).__name__, e)))
    return out


def packages(roots, sample=1):
    bases = []
    for root in roots:
        if os.path.isfile(root + '.uasset'): bases.append(root); continue
        bases += sorted(p[:-len('.uasset')] for p in glob.glob(os.path.join(root, '**', '*.uasset'), recursive=True))
    return bases[::sample]


def main():
    args, sample, only = sys.argv[1:], 1, None
    if '--list' in args:
        for name, fn in RULES.items(): print('%-28s %s' % (name, (fn.__doc__ or '').strip().split('\n')[0]))
        return
    # --json: every finding as a JSON [base, rule, export, message] line, then `done <packages>` - test_bytecode.py's sweep runs
    # shards of the game's packages this way, one process per core. --from: a file of package bases, one per line.
    as_json = '--json' in args
    if as_json: args.remove('--json')
    for flag in ('--sample', '--only', '--game', '--ueapi', '--sdk', '--from'):
        if flag in args:
            i = args.index(flag)
            if flag == '--sample': sample = int(args[i + 1])
            elif flag == '--only': only = set(args[i + 1].split(','))
            elif flag == '--game': GAME_CONTENT.append(args[i + 1])
            elif flag == '--from': args += open(args[i + 1], encoding='utf-8').read().splitlines()
            del args[i:i + 2]
    bad, n, hits = 0, 0, {}
    for base in packages(args, sample):
        try: pkg = Package(base)
        except Exception as e:
            if as_json: print(json.dumps([base, 'unreadable', '-', str(e)]))
            else: print('UNREADABLE %s: %s' % (base, e))
            bad += 1
            continue
        n += 1
        for rule_name, export, msg in check(pkg, only):
            hits[rule_name] = hits.get(rule_name, 0) + 1
            if as_json: print(json.dumps([base, rule_name, export, msg]))
            elif hits[rule_name] <= 20: print('%s  %s  %s: %s' % (rule_name, base, export, msg))
    if as_json:
        print('done %d' % (n + bad))
        return
    print('%d packages, %d unreadable; findings by rule: %s' % (n, bad, hits or 'none'))
    sys.exit(1 if bad or hits else 0)


if __name__ != '__main__':
    import invariant_rules      # noqa: F401 - every area's rules register themselves (invariant_rules/__init__.py)
else:
    # --ueapi / --sdk reach the rules through the environment they read at import; then the module is imported under
    # its own name, so the rules register in the same RULES main() walks (not a second copy in __main__).
    for flag, var in (('--ueapi', 'INVARIANTS_UEAPI'), ('--sdk', 'INVARIANTS_SDK')):
        if flag in sys.argv: os.environ[var] = sys.argv[sys.argv.index(flag) + 1]
    import invariants
    invariants.main()
