"""bpdecomp's function bodies: one function's bytecode (or one event's part of the ubergraph) as AssetGen C++ text.

The statements come from invariants.Package.script, structured by bpdecomp_flow. Each expression is rendered with a
precedence, so parentheses appear only where C++ needs them. A Kismet library call is written as the C++ AssetGen
compiles back into that same call: an operator from UeApi's Ops.json, a container method for the array, set and map
libraries, and otherwise the library's static by name. The editor's temporaries (a local written once and read once,
right after) are folded into their reader when nothing that runs in between could see the difference."""
import re, struct
import bpdecomp_flow as F
from bpdecomp_emit_util import split_top
from bpdecomp_model import ident, authored, CPF_Parm, CPF_OutParm, CPF_ReturnParm, CPF_ConstParm, CPF_ReferenceParm

CALLS = {0x1B, 0x1C, 0x45, 0x46, 0x68}
LETS = {0x0F, 0x14, 0x5F, 0x60, 0x43, 0x44}
VARS = {0x00, 0x01, 0x48}
CONTAINERS = ('TArray<', 'TSet<', 'TMap<')
P_PRIMARY, P_UNARY, P_MUL, P_ADD, P_REL, P_EQ, P_AND, P_OR, P_COND, P_ASSIGN = 0, 3, 5, 6, 9, 10, 14, 15, 16, 17
BINPREC = {'*': P_MUL, '/': P_MUL, '%': P_MUL, '+': P_ADD, '-': P_ADD, '<': P_REL, '>': P_REL, '<=': P_REL,
           '>=': P_REL, '==': P_EQ, '!=': P_EQ, '&&': P_AND, '||': P_OR, '&': 11, '^': 12, '|': 13}
CONTAINER_LIBS = {'KismetArrayLibrary': 'Array_', 'BlueprintSetLibrary': 'Set_', 'BlueprintMapLibrary': 'Map_'}
CONTAINER_RENAME = {'Array_Length': 'Num', 'Set_Length': 'Length', 'Map_Length': 'Length'}
PURE_MATH = {'KismetMathLibrary', 'KismetStringLibrary', 'KismetTextLibrary'}   # libraries whose calls change nothing


class E:
    """A rendered expression: its text and the precedence of its outermost operator."""
    def __init__(s, text, prec=P_PRIMARY, delegate=None):
        s.text, s.prec, s.delegate = text, prec, delegate
    def at(s, maxprec):
        return s.text if s.prec <= maxprec else '(%s)' % s.text


def cfloat(v):
    if v != v: return 'NAN'
    if v in (float('inf'), float('-inf')): return ('-' if v < 0 else '') + 'INFINITY'
    for digits in range(6, 10):
        t = '%.*g' % (digits, v)
        if struct.unpack('<f', struct.pack('<f', float(t)))[0] == v: break
    if 'e' not in t and '.' not in t and 'n' not in t: t += '.0'
    return t + 'f'


def cstring(s):
    out = []
    for ch in s:
        if ch == '\\': out.append('\\\\')
        elif ch == '"': out.append('\\"')
        elif ch == '\n': out.append('\\n')
        elif ch == '\r': out.append('\\r')
        elif ch == '\t': out.append('\\t')
        elif ord(ch) < 0x20 or ord(ch) == 0x7F: out.append('\\x%02x""' % ord(ch))
        else: out.append(ch)
    wide = any(ord(ch) > 0x7F for ch in s)
    return ('L"%s"' if wide else '"%s"') % ''.join(out)


def walk(n):
    yield n
    for k in n.kids: yield from walk(k)


def var_name(n):
    p = next(o for o in n.ops if o[0] == 'prop')
    return p[1][-1] if p[1] else '', p[2]


class Body:
    """Decompiles one function. `fx` is the class-level context (bpdecomp.ClassDecomp): the model, the types, the class
    asset and its package, member lookup. `frame` maps ubergraph frame variables to the text standing for them."""
    def __init__(s, fx, fidx, stmts, entry_mem=0, frame=None, split=None, method=None):
        s.fx, s.pkg, s.fidx = fx, fx.pkg, fidx
        s.blob = s.pkg.blob(fidx)
        s.stmts = stmts
        s.entry_mem = entry_mem
        s.frame = frame or {}
        s.writing = False       # rendering a target that is written: a const& parameter in it is cast writable
        s.split = split or {}
        s.method = method or s.pkg.exports[fidx]['name']
        s.notes = []
        s.st = s.pkg.struct(fidx)
        s.props = {p.name: p for p in s.st.props}
        s.locals_used = set()
        s.latent_helpers = []

    # ---- names

    def local_ident(s, name):
        return s.fx.local_ident(s, name)

    def owner_is_frame(s, owner):
        return owner and owner > 0 and owner - 1 == s.fx.ubergraph

    # ---- literal reads off the blob

    def u8(s, n): return s.blob[n.disk + 1]
    def f32(s, n, k=0): return struct.unpack_from('<f', s.blob, n.disk + 1 + 4 * k)[0]
    def cstr(s, n):
        e = s.blob.index(b'\0', n.disk + 1)
        return s.blob[n.disk + 1:e].decode('latin-1')
    def wstr(s, n):
        o = n.disk + 1
        st = o
        while s.blob[o] or s.blob[o + 1]: o += 2
        return s.blob[st:o].decode('utf-16-le')
    def text_of(s, n):
        """A string-ish constant's value: String, UnicodeString or Name."""
        if n.op == 0x1F: return s.cstr(n)
        if n.op == 0x34: return s.wstr(n)
        if n.op == 0x21: return next(o[1] for o in n.ops if o[0] == 'name')
        return None

    # ---- hints: what type an expression is wanted as

    def hint_of_prop(s, pkg, p):
        if p is None: return None
        t = p.type
        if t == 'ByteProperty' and p.ref: return ('enum', pkg, p.ref)
        if t == 'EnumProperty': return ('enum', pkg, p.enum)
        return {'NameProperty': 'name', 'TextProperty': 'text', 'StrProperty': 'string', 'FloatProperty': 'float',
                'BoolProperty': 'bool', 'ByteProperty': 'byte', 'IntProperty': 'int'}.get(t)

    def hint_of_ctype(s, ty):
        if ty is None: return None
        t = ty.replace('const ', '').replace('&', '').strip()
        if t == 'FName': return 'name'
        if t == 'FText': return 'text'
        if t == 'FString': return 'string'
        if t == 'float': return 'float'
        if t in ('uint8',): return 'byte'
        m = re.match(r'^TEnum<(\w+)>$', t)
        if m: return ('cenum', m.group(1))
        if t in s.fx.api_enum_names: return ('cenum', t)
        return None

    def prop_of_var(s, n):
        """The FProperty a variable node reads, and its package: a local, a frame variable, or a member of a mod class."""
        name, owner = var_name(n)
        if n.op in (0x00, 0x48):
            if s.owner_is_frame(owner): return s.fx.frame_props.get(name), s.pkg
            return s.props.get(name), s.pkg
        return s.fx.member_prop(s.pkg, owner, name)

    def expr_hint(s, n):
        if n.op in VARS:
            p, pkg = s.prop_of_var(n)
            if p is None and n.op == 0x01: return s.hint_of_ctype(s.native_class_member_ctype(n))
            return s.hint_of_prop(pkg, p)
        if n.op == 0x42:
            owner, name = next(o for o in n.ops if o[0] == 'prop')[2], var_name(n)[0]
            p, pkg = s.fx.member_prop(s.pkg, owner, name)
            if p is None: return s.hint_of_ctype(s.native_member_ctype(owner, name))
            return s.hint_of_prop(pkg, p)
        if n.op in (0x19, 0x1A): return s.expr_hint(n.kids[1])
        return None

    def native_class_member_ctype(s, n):
        """The C++ type of a native (or game Blueprint) class's member a variable node reads, off its UeApi header."""
        name, owner = var_name(n)
        if not owner: return None
        top, _, cname = s.pkg.path(owner).partition('.')
        cpp = s.fx.api.classes.get((top, cname))
        return s.fx.api.member_type(cpp.split('::')[-1], name) if cpp else None

    def native_member_ctype(s, owner, name):
        """The C++ type of member `name` of the native struct `owner` names, off Types.json."""
        top, _, sname = s.pkg.path(owner).partition('.')
        cpp = s.fx.api.structs.get((top, sname))
        info = s.fx.run.native_struct(cpp) if cpp else None
        return next((t for t, f in info['fields'] if f == name), None) if info else None

    def type_of(s, n):
        """The C++ type of an expression, where a variable or a call says it; else None."""
        try:
            if n.op in VARS:
                p, pkg = s.prop_of_var(n)
                if p is None and n.op == 0x01: return s.native_class_member_ctype(n)
                return s.fx.types.of(pkg, p, s.fx.asset) if p is not None else None
            if n.op in CALLS: return s.fx.callee(s, n, None).ret
            if n.op in (0x19, 0x1A) and n.kids[1].op in CALLS: return s.fx.callee(s, n.kids[1], n.kids[0]).ret
            if n.op in (0x19, 0x1A): return s.type_of(n.kids[1])
            if n.op == 0x6B:
                t = s.type_of(n.kids[0])
                return t[len('TArray<'):-1] if t and t.startswith('TArray<') else None
            if n.op == 0x17: return s.fx.cpp_self + '*'
            if n.op == 0x42:
                owner, name = next(o for o in n.ops if o[0] == 'prop')[2], var_name(n)[0]
                p, pkg = s.fx.member_prop(s.pkg, owner, name)
                return s.fx.types.of(pkg, p, s.fx.asset) if p is not None else s.native_member_ctype(owner, name)
        except Exception:
            return None
        return None

    def coerce(s, e, src, dst, lvalue=False, out=False):
        """Expression e of C++ type src where dst is wanted: Blueprint lets a pin of a parent class, a class pin of
        another class and a DeterminesOutputType return in where C++ wants a cast. The casts compile to nothing."""
        norm = lambda t: t.replace('const ', '').replace('&', '').replace('class ', '').replace(' ', '')
        if dst and not out and re.fullmatch(r'\d+', e.text):
            d = norm(dst)
            if d.startswith('TEnum<') or (d.split('::')[-1].startswith('E') and '<' not in d and not d.endswith('*')):
                return E('(%s)%s' % (d[len('TEnum<'):-1] if d.startswith('TEnum<') else d, e.text), P_UNARY)
        if not src or not dst: return e
        src, dst = norm(src), norm(dst)
        if src == dst: return e
        last = lambda t: t.rstrip('*').split('::')[-1]
        if src.startswith(CONTAINERS) and dst.startswith(CONTAINERS):
            # an array of a subclass where one of the parent is wanted: a reference cast, which compiles to nothing
            return E('(%s&)%s' % (dst, e.at(P_UNARY)), P_UNARY) if lvalue else e
        is_enum = lambda t: t.startswith('TEnum<') or (last(t).startswith('E') and not t.endswith('*') and '<' not in t)
        if dst == 'uint8' and is_enum(src): return E('(uint8)%s' % e.at(P_UNARY), P_UNARY)
        if src == 'uint8' and is_enum(dst): return E('(%s)%s' % (dst, e.at(P_UNARY)), P_UNARY)
        if out and src.endswith('*') and dst.endswith('*'):
            return E('(%s&)%s' % (dst, e.at(P_UNARY)), P_UNARY)       # an out parameter of another class
        if src.endswith('*') and dst.endswith('*') and '<' not in dst:
            if last(dst) == 'UObject' or last(dst) in s.fx.m.ancestors(last(src)): return e
            return E('(%s)%s' % (dst, e.at(P_UNARY)), P_UNARY)
        if dst.startswith('TSubclassOf<') and src.startswith(('TSubclassOf<', 'UClass*')):
            inner = lambda t: last(t[len('TSubclassOf<'):-1]) if t.startswith('TSubclassOf<') else 'UObject'
            if inner(dst) == inner(src) or src == 'UClass*': return e     # TSubclassOf<A> -> <B> goes through UClass*
            return E('(UClass*)%s' % e.at(P_UNARY), P_UNARY)
        return e

    # ---- expressions

    def expr(s, n, hint=None):
        op = n.op
        if op in VARS:
            made = s.made_delegate(n)
            return E('{%s}' % made) if made else E(s.var(n))
        if op == 0x17: return E('this')
        if op == 0x1D: return E(str(next(o[1] for o in n.ops if o[0] == 'int')), P_UNARY if next(o[1] for o in n.ops if o[0] == 'int') < 0 else P_PRIMARY)
        if op == 0x2C: return E(str(s.u8(n)))
        if op == 0x24: return s.byte_const(s.u8(n), hint)
        if op == 0x25: return E('0')
        if op == 0x26: return E('1')
        if op == 0x27: return E('true')
        if op == 0x28: return E('false')
        if op in (0x2A, 0x2D): return E('nullptr')
        if op == 0x1E:
            v = s.f32(n)
            t = cfloat(v)
            return E(t, P_UNARY if t.startswith('-') else P_PRIMARY)
        if op == 0x35: return E('(int64)%d' % struct.unpack_from('<q', s.blob, n.disk + 1)[0], P_UNARY)
        if op == 0x36: return E('(int64)%dULL' % struct.unpack_from('<Q', s.blob, n.disk + 1)[0], P_UNARY)
        if op in (0x1F, 0x34): return s.string_const(s.text_of(n), hint)
        if op == 0x21: return s.name_const(s.text_of(n), hint)
        if op == 0x29: return s.text_const(n, hint)
        if op == 0x23: return E('FVector(%s, %s, %s)' % tuple(cfloat(s.f32(n, k)) for k in range(3)))
        if op == 0x22: return E('FRotator(%s, %s, %s)' % tuple(cfloat(s.f32(n, k)) for k in range(3)))
        if op == 0x2B: return s.transform_const(n)
        if op == 0x20: return s.object_const(n)
        if op == 0x67:
            p = s.text_of(n.kids[0]) or ''
            return E('TSoftObjectPtr<UObject>(%s)' % cstring(p))
        if op in CALLS: return s.call(n, None)
        if op in (0x19, 0x1A): return s.context(n)
        if op == 0x12: return s.class_context(n)
        if op == 0x51: return s.expr(n.kids[0])
        if op == 0x42: return s.member(n)
        if op == 0x6B: return E('%s[%s]' % (s.expr(n.kids[0]).at(P_PRIMARY), s.expr(n.kids[1]).text))
        if op == 0x2E: return E('Cast<%s>(%s)' % (s.fx.types.cls(s.pkg, next(o[1] for o in n.ops if o[0] == 'obj'), s.fx.asset), s.expr(n.kids[0]).text))
        if op == 0x13:      # Cast To Class: a TSubclassOf conversion alone is unchecked, so the check is spelled out
            # ponytail: the value is evaluated twice; fine for the variables the cast node reads
            c, x = s.fx.types.cls(s.pkg, next(o[1] for o in n.ops if o[0] == 'obj'), s.fx.asset), s.expr(n.kids[0]).text
            return E('(UKismetMathLibrary::ClassIsChildOf((UClass*)%s, %s::StaticClass()) ? TSubclassOf<%s>(%s) : TSubclassOf<%s>(nullptr))'
                     % (s.expr(n.kids[0]).at(P_UNARY), c, c, x, c))
        if op in (0x52, 0x54):
            return E('TScriptInterface<%s>(%s)' % (s.fx.types.cls(s.pkg, next(o[1] for o in n.ops if o[0] == 'obj'), s.fx.asset), s.expr(n.kids[0]).text))
        if op == 0x55: return E('Cast<%s>(%s)' % (s.fx.types.cls(s.pkg, next(o[1] for o in n.ops if o[0] == 'obj'), s.fx.asset), s.expr(n.kids[0]).text))
        if op == 0x38:
            kind = s.u8(n)
            inner = s.expr(n.kids[0])
            if kind in (0x47, 0x49): return E('%s != nullptr' % inner.at(P_REL), P_EQ)
            if kind == 0x46: return inner
            s.note('primitive cast %#x' % kind)
            return inner
        if op == 0x2F: return s.struct_const(n)
        if op == 0x69: return s.switch_value(n, hint)
        if op == 0x4B: return E('&%s::%s' % (s.fx.cpp_self, ident(next(o[1] for o in n.ops if o[0] == 'name'))))
        if op in (0x65, 0x3D):     # an array's or a set's literal: its elements
            return E('{%s}' % ', '.join(s.expr(k).text for k in n.kids if k.op not in (0x66, 0x3E)))
        if op == 0x3F:             # a map's literal: keys and values alternate
            kv = [s.expr(k).text for k in n.kids if k.op != 0x40]
            return E('{%s}' % ', '.join('{%s, %s}' % (kv[i], kv[i + 1]) for i in range(0, len(kv) - 1, 2)))
        if op == 0x02:
            name, owner = var_name(n)
            return E('%s::StaticClass()->GetDefaultObject()->%s' % (s.fx.cpp_self, ident(name)))
        s.note('expression op %#x' % op)
        return E('/* bpdecomp: op %#x */ 0' % op)

    def note(s, what):
        s.notes.append('%s: %s' % (s.method, what))

    def var(s, n):
        name, owner = var_name(n)
        if n.op in (0x00, 0x48):
            if s.owner_is_frame(owner) and name in s.frame: return s.frame[name]
            s.locals_used.add(name)
            t = s.local_ident(name)
            p = s.props.get(name) if s.writing else None
            if p is not None and p.flags & CPF_Parm and not p.flags & CPF_ReturnParm and \
                    not (p.flags & CPF_OutParm and not p.flags & CPF_ConstParm) and \
                    (p.flags & CPF_ReferenceParm or (p.flags & CPF_ConstParm and p.flags & CPF_OutParm)):
                return '((%s&)%s)' % (s.fx.types.of(s.pkg, p, s.fx.asset), t)
            return t
        return s.fx.member_text(s, name)

    def byte_const(s, v, hint):
        if isinstance(hint, tuple) and hint[0] == 'enum':
            e = s.fx.enum_entry(hint[1], hint[2], v)
            if e: return E(e)
        if isinstance(hint, tuple) and hint[0] == 'cenum':
            e = s.fx.cenum_entry(hint[1], v)
            if e: return E(e)
        return E(str(v))

    def string_const(s, v, hint):
        if hint in ('string', 'name', 'text'): return E(cstring(v))
        return E('FString(%s)' % cstring(v))

    def name_const(s, v, hint):
        if v == 'None': return E('FName()')
        if hint == 'name': return E(cstring(v))
        return E('FName(%s)' % cstring(v))

    def text_const(s, n, hint):
        kind = s.u8(n)
        if kind == 0: return E('FText()')
        src = s.text_of(n.kids[0]) if n.kids else ''
        if kind == 4: s.note('a string-table text'); src = ''
        return E(cstring(src)) if hint == 'text' else E('FText(%s)' % cstring(src))

    def transform_const(s, n):
        q = [s.f32(n, k) for k in range(4)]
        t = [s.f32(n, 4 + k) for k in range(3)]
        sc = [s.f32(n, 7 + k) for k in range(3)]
        import math
        x, y, z, w = q
        pitch = math.degrees(math.asin(max(-1.0, min(1.0, 2 * (w * y - z * x)))))
        yaw = math.degrees(math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)))
        roll = math.degrees(math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y)))
        return E('UKismetMathLibrary::MakeTransform(FVector(%s), FRotator(%s), FVector(%s))' % (
            ', '.join(map(cfloat, t)), ', '.join(map(cfloat, (pitch, yaw, roll))), ', '.join(map(cfloat, sc))))

    def object_const(s, n):
        idx = next(o[1] for o in n.ops if o[0] == 'obj')
        return E('nullptr') if not idx else E(s.fx.object_ref(s, idx))     # an unset object pin is a null constant

    def struct_const(s, n):
        sidx = next(o[1] for o in n.ops if o[0] == 'obj')
        vals = [k for k in n.kids if k.op != 0x30]
        return E(s.fx.struct_literal(s, sidx, vals))

    def switch_value(s, n, hint):
        idx = s.expr(n.kids[0])
        rest = n.kids[1:]
        pairs = [(rest[2 * k], rest[2 * k + 1]) for k in range((len(rest) - 1) // 2)]
        default = rest[-1]
        ih = s.expr_hint(n.kids[0])
        text = s.expr(default, hint).at(P_COND)
        for key, val in reversed(pairs):
            text = '%s == %s ? %s : %s' % (idx.at(P_EQ - 1), s.expr(key, ih).at(P_EQ - 1), s.expr(val, hint).at(P_COND - 1), text)
        return E(text, P_COND)

    def member(s, n):
        name, owner = var_name(n)
        base = s.expr(n.kids[0])
        return E('%s.%s' % (base.at(P_PRIMARY), s.fx.struct_member_ident(s.pkg, owner, name)))

    def context(s, n):
        obj, inner = n.kids[0], n.kids[1]
        if inner.op in CALLS: return s.call(inner, obj)
        if inner.op in (0x01, 0x6C):
            name, owner = var_name(inner)
            o = s.expr(obj)
            ot = s.type_of(obj)
            if owner and ot and ot.endswith('*') and '<' not in ot:
                oc = s.fx.types.cls(s.pkg, owner, s.fx.asset)
                if oc and oc.split('::')[-1] not in s.fx.m.ancestors(ot.replace('class ', '').rstrip('*').strip().split('::')[-1]):
                    o = E('((%s*)%s)' % (oc, o.at(P_UNARY)))
            return E('%s->%s' % (o.at(P_PRIMARY), s.fx.other_member_ident(s.pkg, owner, name)))
        if inner.op == 0x42:
            return E('%s->%s' % (s.expr(obj).at(P_PRIMARY), s.member(inner).text))
        s.note('context over op %#x' % inner.op)
        return E('%s->%s' % (s.expr(obj).at(P_PRIMARY), s.expr(inner).text))

    def class_context(s, n):
        cls, inner = n.kids[0], n.kids[1]
        base = s.expr(cls)
        # Get Class Defaults: AssetGen has no EX_ClassContext yet, so the umbrella's ClassDefaults finds the object
        s.fx.run.class_defaults_used = True
        if inner.op in CALLS:
            info = s.fx.callee(s, inner, None)
            return s.call(inner, None, prefix='Cast<%s>(ClassDefaults(%s))->' % (info.owner_cpp, base.text))
        name, owner = var_name(inner)
        return E('Cast<%s>(ClassDefaults(%s))->%s' % (s.fx.types.cls(s.pkg, owner, s.fx.asset), base.text, ident(name)))

    # ---- calls

    ELEM_ARG = {'Array_Add': 1, 'Array_AddUnique': 1, 'Array_Set': 2, 'Array_Insert': 1, 'Array_Find': 1,
                'Array_Contains': 1, 'Array_RemoveItem': 1, 'Set_Add': 1, 'Set_Contains': 1, 'Set_Remove': 1,
                'Map_Add': 1, 'Map_Find': 1, 'Map_Contains': 1, 'Map_Remove': 1}

    READS = {'Array_Length', 'Array_Get', 'Array_Contains', 'Array_Find', 'Array_IsValidIndex', 'Array_LastIndex',
             'Array_Identical', 'Array_Random', 'Array_RandomFromStream', 'Set_Length', 'Set_Contains', 'Set_ToArray',
             'Set_Union', 'Set_Difference', 'Set_Intersection', 'Map_Length', 'Map_Contains', 'Map_Find', 'Map_Keys',
             'Map_Values'}
    # argument -> (type, out) with E / K / V the element, key and value type, C the container's own
    ARGS = {'Array_Get': {2: ('E&', True)}, 'Array_Add': {1: ('E', False)}, 'Array_AddUnique': {1: ('E', False)},
            'Array_Contains': {1: ('E', False)}, 'Array_Find': {1: ('E', False)}, 'Array_RemoveItem': {1: ('E', False)},
            'Array_Insert': {1: ('E', False)}, 'Array_Set': {2: ('E', False)}, 'Array_Append': {1: ('C', False)},
            'Set_Add': {1: ('E', False)}, 'Set_Contains': {1: ('E', False)}, 'Set_Remove': {1: ('E', False)},
            'Set_AddItems': {1: ('TArray<E>', False)}, 'Set_RemoveItems': {1: ('TArray<E>', False)},
            'Set_ToArray': {1: ('TArray<E>&', True)}, 'Set_Union': {1: ('C', False), 2: ('C&', True)},
            'Set_Difference': {1: ('C', False), 2: ('C&', True)}, 'Set_Intersection': {1: ('C', False), 2: ('C&', True)},
            'Map_Add': {1: ('K', False), 2: ('V', False)}, 'Map_Find': {1: ('K', False), 2: ('V&', True)},
            'Map_Contains': {1: ('K', False)}, 'Map_Remove': {1: ('K', False)},
            'Map_Keys': {1: ('TArray<K>&', True)}, 'Map_Values': {1: ('TArray<V>&', True)}}

    def container_args(s, fn, args):
        """{argument: (C++ type, out)} of a container call, off the container's type: Blueprint passes an element of a
        subclass, a byte for an enum, an array of a subclass, where C++ wants the container's own."""
        spec = s.ARGS.get(fn)
        t = s.type_of(args[0]) if spec else None
        if not t or '<' not in t: return {}
        t = t.replace('const ', '').rstrip('&').strip()
        subs = [x.strip() for x in split_top(t[t.index('<') + 1:t.rindex('>')])]
        names = {'E': subs[0], 'K': subs[0], 'V': subs[-1], 'C': t}
        out = {}
        for i, (ty, o) in spec.items():
            if i >= len(args): continue
            out[i] = (re.sub(r'\b[EKVC]\b', lambda m: names[m.group(0)], ty), o)
        return out

    def container_hints(s, fn, args, hints):
        """The hint of a wildcard element, key or value argument, off the container's own property (or C++ type):
        an enum element's value is written as its enumerator."""
        p, pkg = s.prop_of_var(args[0]) if args[0].op in VARS else (None, None)
        if p is not None and p.subs:
            subs = [s.hint_of_prop(pkg, q) for q in p.subs]
        else:
            t = s.type_of(args[0])
            if not t or '<' not in t: return
            subs = [s.hint_of_ctype(x.strip()) for x in split_top(t[t.index('<') + 1:t.rindex('>')])]
        at = s.ELEM_ARG.get(fn)
        if at is not None and at < len(hints) and subs[0]: hints[at] = subs[0]
        if fn == 'Map_Add' and len(subs) > 1 and len(hints) > 2 and subs[1]: hints[2] = subs[1]

    def call(s, n, obj, prefix=None):
        info = s.fx.callee(s, n, obj)
        args = [k for k in n.kids if k.op != 0x16]
        hints = info.hints + [None] * max(0, len(args) - len(info.hints))
        if info.container and args: s.container_hints(info.raw, args, hints)
        rendered = []
        want = s.container_args(info.raw, args) if info.container and args else {}
        for i, a in enumerate(args):
            if i in info.drop: continue
            if i == 0 and info.container and info.raw not in s.READS: e = E(s.lvalue(a))     # the container is written
            elif i in info.out and not info.container: e = E(s.lvalue(a))
            else: e = s.expr(a, hints[i])
            if i in want:
                e = s.coerce(e, s.type_of(a), want[i][0], lvalue=want[i][1] or a.op in VARS, out=want[i][1])
            if i < len(info.ptypes):
                # a struct's member is an lvalue too: `(TMap<int, int>&)Transformed.ActorTransform` for a wildcard
                e = s.coerce(e, s.type_of(a), info.ptypes[i], lvalue=i in info.out or a.op in VARS or a.op == 0x42,
                             out=i in info.out)
                if i in info.out and a.op in VARS and e.text == s.var(a):
                    p, _pkg = s.prop_of_var(a)
                    if p is not None and p.flags & CPF_ConstParm and p.flags & CPF_Parm:
                        e = E('(%s&)%s' % (info.ptypes[i].replace('const ', '').rstrip('&'), e.text), P_UNARY)
            rendered.append(e)
        if info.static and info.lib:
            sug = s.fx.sugar(s, info, args, rendered)
            if sug is not None: return sug
        texts = ', '.join(r.text for r in rendered)
        if prefix is not None: return E('%s%s(%s)' % (prefix, info.name, texts))
        if info.static: return E('%s::%s(%s)' % (info.owner_cpp, info.name, texts))
        if obj is not None:
            o = s.expr(obj)
            ot = s.type_of(obj)
            if info.owner_cpp and ot and ot.endswith('*') and '<' not in ot:
                own = info.owner_cpp.split('::')[-1]
                if own not in s.fx.m.ancestors(ot.replace('class ', '').rstrip('*').strip().split('::')[-1]):
                    o = E('((%s*)%s)' % (info.owner_cpp, o.at(P_UNARY)))
            return E('%s->%s(%s)' % (o.at(P_PRIMARY), info.name, texts))
        if info.super_call: return E('%s::%s(%s)' % (info.owner_cpp, info.name, texts))
        return E('%s(%s)' % (info.name, texts))

    # ---- statements

    def stmt(s, n):
        """The C++ lines of one top-level statement."""
        op = n.op
        if op in LETS:
            dst, src = n.kids[0], n.kids[1]
            if dst.op in VARS and dst.op != 0x01 and src.op in VARS:
                if var_name(dst) == var_name(src): return []
            st, dt = s.type_of(src), s.type_of(dst)
            lhs = E(s.lvalue(dst))
            if src.op not in VARS and st and dt and st.startswith(CONTAINERS):
                lhs = s.coerce(lhs, dt, st, lvalue=True)       # a call's array of the parent class: cast the target
                st = dt
            e = s.coerce(s.expr(src, s.expr_hint(dst)), st, dt, lvalue=src.op in VARS)
            return ['%s = %s;' % (lhs.text, e.text)]
        if op == 0x64:
            name, owner = var_name(n)
            dst = s.frame.get(name) or s.local_ident(name)
            if name not in s.frame: s.locals_used.add(name)
            return ['%s = %s;' % (dst, s.expr(n.kids[0], s.hint_of_prop(s.pkg, s.fx.frame_props.get(name))).text)]
        if op in CALLS or op in (0x19, 0x1A, 0x12):
            e = s.expr(n)
            return [e.text + ';'] if e.text else []
        if op in (0x31, 0x39, 0x3B):
            dst = n.kids[0]
            elems = [k for k in n.kids[1:] if k.op not in (0x32, 0x3A, 0x3C)]
            if op == 0x3B:
                body = ', '.join('{%s, %s}' % (s.expr(elems[2 * k]).text, s.expr(elems[2 * k + 1]).text) for k in range(len(elems) // 2))
            else:
                h = None
                p, pkg = s.prop_of_var(dst) if dst.op in VARS else (None, None)
                if p is not None and p.subs: h = s.hint_of_prop(pkg, p.subs[0])
                body = ', '.join(s.expr(e, h).text for e in elems)
            return ['%s = {%s};' % (s.lvalue(dst), body)]
        if op == 0x61:
            if s.made_delegate(n.kids[0]): return []     # the value is written where it is used
            fn = next(o[1] for o in n.ops if o[0] == 'name')
            dst, obj = n.kids[0], n.kids[1]
            return ['%s = {%s, &%s::%s};' % (s.lvalue(dst), s.expr(obj).text, s.fx.class_of_expr(s, obj), ident(fn))]
        if op in (0x5C, 0x62):
            mc, d = s.expr(n.kids[0]), n.kids[1]
            verb = 'Add' if op == 0x5C else 'Remove'
            bound = s.fx.bound_delegate(s, d)
            if bound: return ['%s.%s(%s);' % (mc.text, verb, bound)]
            return ['%s.%s(%s);' % (mc.text, verb, s.expr(d).text)]
        if op == 0x5D: return ['%s.Clear();' % s.expr(n.kids[0]).text]
        if op == 0x63:
            args = [k for k in n.kids[1:] if k.op != 0x16]
            return ['%s.Broadcast(%s);' % (s.expr(n.kids[0]).text, ', '.join(s.expr(a).text for a in args))]
        if op == 0x09: return []
        if op in F.NOOPS: return []
        s.note('statement op %#x' % op)
        return ['/* bpdecomp: statement op %#x */' % op]

    def made_delegate(s, n):
        """`obj, &C::F` for a Create Event node's output local that only ever holds that one binding."""
        if n.op not in (0x00, 0x48) or not var_name(n)[0].startswith('K2Node_CreateDelegate'): return None
        return s.fx.bound_delegate(s, n)

    def lvalue(s, n):
        was, s.writing = s.writing, True
        try:
            return s.expr(n).text
        finally:
            s.writing = was

    # ---- the whole function

    def build(s):
        g = F.Graph(s.stmts, s.entry_mem, s.split)
        bs = F.blocks(g)
        try:
            tree = F.structure(bs, g.entry)
            s.fold(tree)
            return s.lower(tree, [])
        except F.Irreducible:
            s.note('irreducible flow: written as labelled blocks')
            out = []
            for k, body in F.linear(bs, g.entry):
                out.append(Label(s.label(k)))
                out += s.lower(body, [])
            return out

    def label(s, key):
        return 'L_%04X' % s.stmts[key[0]].mem if key != F.RET else 'L_RET'

    def lower(s, items, loops):
        out = []
        for it in items:
            if isinstance(it, tuple):
                for line in s.stmt(it[1]): out.append(S(line))
            elif isinstance(it, F.If):
                out.append(CIf(s.cond(it.cond), s.lower(it.then, loops), s.lower(it.other, loops)))
            elif isinstance(it, F.Loop):
                body = s.lower(it.body, loops + [it.label])
                out.append(CWhile('true', [Label(s.label(it.label) + '_top')] + body))
            elif isinstance(it, F.Seq):
                out += s.lower(it.body, loops)
                out.append(Label(s.label(it.label)))
            elif isinstance(it, F.Break):
                out.append(Goto(s.label(it.label)))
            elif isinstance(it, F.Continue):
                out.append(S('continue;') if loops and loops[-1] == it.label else Goto(s.label(it.label) + '_top'))
            elif isinstance(it, F.Return):
                out.append(CReturn())
            elif isinstance(it, F.Latent):
                out += s.lower_latent(it, loops)
            else:
                raise TypeError(it)
        return out

    def lower_latent(s, it, loops):
        call = s.stmt(it.call)
        now = s.lower(it.now, loops)
        resume = s.lower(it.resume, [])
        if len(now) == 1 and isinstance(now[0], CReturn):
            return [S(l) for l in call] + resume
        if not resume or (len(resume) == 1 and isinstance(resume[0], CReturn) and resume[0].text is None):
            return [S(l) for l in call] + now
        name = '%s_Resume%d' % (s.method, len(s.latent_helpers))
        s.latent_helpers.append((name, [S(l) for l in call] + resume))
        return [S('%s();' % name)] + now

    def cond(s, n):
        return s.expr(n, 'bool').text

    # ---- folding the editor's temporaries

    def fold(s, tree):
        uses, defs = {}, {}
        def count(items):
            for it in items:
                if isinstance(it, tuple): nodes = [it[1]]
                elif isinstance(it, F.If): nodes = [it.cond]; count(it.then); count(it.other)
                elif isinstance(it, (F.Loop, F.Seq)): count(it.body); nodes = []
                elif isinstance(it, F.Latent): nodes = [it.call]; count(it.resume); count(it.now)
                else: nodes = []
                for root in nodes:
                    skip = None
                    if root.op in LETS and root.kids[0].op in (0x00, 0x48):
                        k = var_name(root.kids[0])
                        defs[k] = defs.get(k, 0) + 1
                        skip = root.kids[0]
                    for x in walk(root):
                        if x is skip: continue
                        if x.op in (0x00, 0x48): uses[var_name(x)] = uses.get(var_name(x), 0) + 1
                        if x.op == 0x64:
                            k = var_name(x); defs[k] = defs.get(k, 0) + 2
        count(tree)
        s._fold_list(tree, uses, defs)

    def _fold_list(s, items, uses, defs):
        changed = True
        while changed:
            changed = False
            for i in range(len(items) - 1):
                a, b = items[i], items[i + 1]
                if not (isinstance(a, tuple) and a[1].op in LETS and a[1].kids[0].op in (0x00, 0x48)): continue
                key = var_name(a[1].kids[0])
                if defs.get(key) != 1 or uses.get(key) != 1: continue
                if key[0] in s.frame or s.fx.is_param(s, key[0]): continue
                target = b[1] if isinstance(b, tuple) else b.cond if isinstance(b, F.If) else None
                if target is None: continue
                src = a[1].kids[1]
                if s._replace(target, key, src):
                    del items[i]
                    changed = True
                    break
        for it in items:
            if isinstance(it, F.If): s._fold_list(it.then, uses, defs); s._fold_list(it.other, uses, defs)
            elif isinstance(it, (F.Loop, F.Seq)): s._fold_list(it.body, uses, defs)
            elif isinstance(it, F.Latent): s._fold_list(it.resume, uses, defs); s._fold_list(it.now, uses, defs)

    def _replace(s, root, key, src):
        """Puts src where root reads the variable key, if nothing root runs before that read has an effect src could
        see or that could see src run earlier. False (root unchanged) otherwise."""
        order = []
        def visit(x, parent, at):
            order.append((x, parent, at))
            for j, k in enumerate(x.kids): visit(k, x, j)
        visit(root, None, None)
        src_calls = any(x.op in CALLS and not s.fx.is_pure_call(s, x) for x in walk(src))
        for x, parent, at in order:
            if x.op in (0x00, 0x48) and var_name(x) == key:
                if parent is None: return False
                if parent.op in LETS and at == 0: return False            # written, not read
                if parent.op in CALLS and s.fx.is_out_arg(s, parent, at): return False
                if parent.op in (0x31, 0x39, 0x3B) and at == 0: return False
                if parent.op in (0x42, 0x6B) and at == 0 and not s._addressable(src): return False
                if parent.op in CALLS and s.fx.container_target(s, parent, at) and not s._addressable(src): return False
                parent.kids[at] = src
                return True
            if src_calls and x.op in CALLS and not s.fx.is_pure_call(s, x): return False
            if x.op in LETS and x is not root: return False
        return False

    def _addressable(s, n):
        return n.op in VARS or (n.op == 0x42 and s._addressable(n.kids[0])) or (n.op in (0x19, 0x1A) and n.kids[1].op in (0x01,))


# ---- the C++ statement tree and its tidying

class S:
    def __init__(s, text): s.text = text
class CIf:
    def __init__(s, cond, then, other): s.cond, s.then, s.other = cond, then, other
class CWhile:
    def __init__(s, cond, body): s.cond, s.body = cond, body
class Label:
    def __init__(s, name): s.name = name
class Goto:
    def __init__(s, name): s.name = name
class CReturn:
    def __init__(s, text=None): s.text = text


def negate(c):
    if c.startswith('!') and re.fullmatch(r'!\w+', c): return c[1:]
    if c.startswith('!(') and c.endswith(')') and balanced(c[2:-1]): return c[2:-1]
    m = re.fullmatch(r'(.*) != nullptr', c)
    if m and balanced(m.group(1)): return '%s == nullptr' % m.group(1)
    if re.fullmatch(r'[\w.\->\[\]]+', c) and balanced(c): return '!' + c
    return '!(%s)' % c


def balanced(t):
    d = 0
    for ch in t:
        if ch == '(': d += 1
        elif ch == ')':
            d -= 1
            if d < 0: return False
    return d == 0


def tidy(stmts, ret_var=None):
    """Peephole passes until nothing changes: a goto to the label right after it, a label nothing jumps to, an if
    with an empty arm, a loop that tests first and leaves to right after itself, `R = X; return R;`."""
    for _ in range(50):
        changed = [False]
        targets = set()
        def collect(xs):
            for x in xs:
                if isinstance(x, Goto): targets.add(x.name)
                elif isinstance(x, CIf): collect(x.then); collect(x.other)
                elif isinstance(x, CWhile): collect(x.body)
        collect(stmts)

        def run(xs, after):
            """after: the label names reached by falling off the end of xs (or 'LOOP' for a loop's top)."""
            out = []
            for i, x in enumerate(xs):
                rest = xs[i + 1:]
                nxt = set()
                j = 0
                while j < len(rest) and isinstance(rest[j], Label): nxt.add(rest[j].name); j += 1
                if j == len(rest): nxt |= after
                if isinstance(x, Label) and x.name not in targets: changed[0] = True; continue
                if isinstance(x, Goto) and x.name in nxt: changed[0] = True; continue
                if isinstance(x, S) and x.text == 'continue;' and 'LOOP' in nxt and j == len(rest): changed[0] = True; continue
                if isinstance(x, CIf):
                    x.then = run(x.then, nxt); x.other = run(x.other, nxt)
                    if not x.then and x.other:
                        x.cond, x.then, x.other = negate(x.cond), x.other, []; changed[0] = True
                    if not x.then and not x.other and re.fullmatch(r'!?[\w.]+', x.cond):
                        changed[0] = True; continue
                    if len(x.other) == 1 and isinstance(x.other[0], CIf) and False: pass
                if isinstance(x, CWhile):
                    x.body = run(x.body, {'LOOP'} | {l.name for l in x.body[:1] if isinstance(l, Label)})
                    x = loop_test(x, nxt) or x
                if isinstance(x, CReturn) and ret_var and out and isinstance(out[-1], S):
                    m = re.fullmatch(r'%s = (.*);' % re.escape(ret_var), out[-1].text)
                    if m and x.text is None: out[-1] = CReturn(m.group(1)); changed[0] = True; continue
                out.append(x)
            # an if whose then-arm ends in a jump away: its else-arm can follow it
            return out
        stmts[:] = run(stmts, set())
        if not changed[0]: break
    return stmts


def loop_test(w, after):
    """`while (true) { if (C) goto Out; ... }` where Out is right after the loop: `while (!C) { ... }`; a goto Out
    elsewhere in the loop's own body (not in an inner loop) becomes break."""
    body = [x for x in w.body if not (isinstance(x, Label) and x.name.endswith('_top'))]
    tops = [x for x in w.body if isinstance(x, Label) and x.name.endswith('_top')]
    changed = False
    def brk(xs):
        nonlocal changed
        for k, x in enumerate(xs):
            if isinstance(x, Goto) and x.name in after: xs[k] = S('break;'); changed = True
            elif isinstance(x, CIf): brk(x.then); brk(x.other)
    brk(body)
    if w.cond == 'true' and body and isinstance(body[0], CIf):
        f = body[0]
        if f.then == [S('break;')] or (len(f.then) == 1 and isinstance(f.then[0], S) and f.then[0].text == 'break;'):
            w.cond = negate(f.cond); body = f.other + body[1:]; changed = True
        elif len(f.other) == 1 and isinstance(f.other[0], S) and f.other[0].text == 'break;':
            w.cond = f.cond; body = f.then + body[1:]; changed = True
    if changed:
        w.body = tops + body
        return w
    return None


def render(stmts, indent=1):
    out = []
    pad = '    ' * indent
    for x in stmts:
        if isinstance(x, S): out.append(pad + x.text)
        elif isinstance(x, Label): out.append(pad[:-2] + x.name + ':')
        elif isinstance(x, Goto): out.append(pad + 'goto %s;' % x.name)
        elif isinstance(x, CReturn): out.append(pad + ('return %s;' % x.text if x.text else 'return;'))
        elif isinstance(x, CIf):
            out.append(pad + 'if (%s) {' % x.cond)
            out += render(x.then, indent + 1)
            if x.other:
                if len(x.other) == 1 and isinstance(x.other[0], CIf):
                    inner = render(x.other, indent)
                    out.append(pad + '} else ' + inner[0].lstrip())
                    out += inner[1:]
                    continue
                out.append(pad + '} else {')
                out += render(x.other, indent + 1)
            out.append(pad + '}')
        elif isinstance(x, CWhile):
            out.append(pad + 'while (%s) {' % x.cond)
            out += render(x.body, indent + 1)
            out.append(pad + '}')
    return out
