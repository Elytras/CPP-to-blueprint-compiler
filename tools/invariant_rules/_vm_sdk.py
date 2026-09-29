"""The native (/Script) half of what the VMSEM rules need, read off a Dumper-7 dump of the running game: the cooked
packages carry no native struct, no native function signature and no computed property flag (FProperty::Serialize
masks CPF_ComputedFlags on save), so a rule about a /Script struct or function reads them here.

table() -> dict, built once and cached in the temp folder:
  structs     '/Script/Engine.HitResult' -> {cpp, super (path or None), size, fields: [[type, name, offset, size, dim, flags]]}
              own fields only, in offset order; a bitfield bool is one field; flags as Dumper-7 prints them
  types       a C++ spelling ('struct FHitResult', ...) -> [zero_constructor, plain_old_data, no_destructor], from
              every field and parameter that has that spelling (CPF_ZeroConstructor / CPF_IsPlainOldData / CPF_NoDestructor,
              which UE computes from the C++ type: TIsZeroConstructType, TIsPODType, TIsTriviallyDestructible)
  functions   '/Script/Engine.GameplayStatics:GetAllActorsOfClass' -> {flags: [...], params: [[type, name, size, dim, flags]]}
  interfaces  every /Script class path that is an interface (Dumper-7 declares it `class I<Name>`)
  classes     '/Script/Engine.Actor' -> {cpp, super (path or None), fields: name -> [type, size]}

Without the dump (no SDK_DUMP) table() is None and the rules skip native operands."""
import json, os, re, tempfile
from invariants import SDK_DUMP

SDK = os.path.join(SDK_DUMP, 'SDK', 'SDK') if SDK_DUMP else ''
CACHE = os.path.join(tempfile.gettempdir(), 'invariants_vm_sdk_%s.json' % re.sub(r'\W', '_', os.path.basename(SDK_DUMP or 'none')))

FIELD = re.compile(r'^\t(?P<type>[^/;:]+?)\s+(?P<name>[A-Za-z_]\w*)(?:\[(?P<dim>0x[0-9A-Fa-f]+|\d+)\])?(?P<bit>\s*:\s*\d+)?;\s*'
                   r'// 0x(?P<off>[0-9A-Fa-f]+)\(0x(?P<size>[0-9A-Fa-f]+)\)\((?P<flags>.*)\)\s*$')
STRUCT_COMMENT = re.compile(r'^// ScriptStruct (/Script/\S+)\s*$')
STRUCT_DECL = re.compile(r'^struct (?:\w+\([^)]*\)\s+)?(\w+)(?:\s+final)?(?:\s*:\s*public\s+(\w+))?\s*$')
SIZE = re.compile(r'^// 0x([0-9A-Fa-f]+) \(0x([0-9A-Fa-f]+) - 0x([0-9A-Fa-f]+)\)')
PARAMS_COMMENT = re.compile(r'^// Function (/Script/\S+)\s*$')
FUNC_COMMENT = re.compile(r'^// Function (\w+)\.(\w+)\.(\w+)\s*$')
CLASS_COMMENT = re.compile(r'^// Class (/Script/\S+)\s*$')
CLASS_DECL = re.compile(r'^class (?:\w+\([^)]*\)\s+)?(\w+)(?:\s+final)?(?:\s*:\s*public\s+(\w+))?\s*$')

_T = [None]


def _flags(text):
    """Dumper-7's flag list; a bitfield's sits inside `BitIndex: .., PropSize: .. (flags)`."""
    m = re.search(r'PropSize: 0x[0-9A-Fa-f]+ \((.*)\)$', text)
    return [f.strip() for f in (m.group(1) if m else text).split(',') if f.strip()]


def _norm(t):
    return ' '.join(t.replace('const ', '').replace('&', '').split())


def build():
    dump = os.path.join(SDK, '..', '..', 'GObjects-Dump.txt')
    script = set(re.findall(r'} Package /Script/(\w+)\s*$', open(dump, encoding='utf-8', errors='replace').read(), re.M))
    names = [f for f in os.listdir(SDK) if f.rsplit('_', 1)[0] in script]
    structs, types, functions, interfaces, classes = {}, {}, {}, [], {}

    def note(t, flags):
        z, p, d = types.setdefault(_norm(t), [False, False, False])
        types[_norm(t)] = [z or 'ZeroConstructor' in flags, p or 'IsPlainOldData' in flags, d or 'NoDestructor' in flags]

    for f in names:
        if f.endswith('_structs.hpp'):
            path, cur, cpp_of = None, None, {}
            for line in open(os.path.join(SDK, f), encoding='utf-8', errors='replace'):
                line = line.rstrip('\n')
                m = STRUCT_COMMENT.match(line)
                if m: path = m.group(1); continue
                m = SIZE.match(line)
                if m and path: size = int(m.group(2), 16); continue
                m = STRUCT_DECL.match(line)
                if m:
                    cur = None
                    if path:
                        cur = structs[path] = dict(cpp=m.group(1), super=m.group(2), size=size, fields=[])
                    path = None
                    continue
                if line.startswith('};'): cur = None; continue
                m = FIELD.match(line)
                if m and cur is not None and not m.group('name').startswith(('Pad_', 'BitPad_')):
                    flags = _flags(m.group('flags'))
                    dim = int(m.group('dim'), 0) if m.group('dim') else 1
                    t = 'bool' if m.group('bit') else m.group('type').strip()
                    cur['fields'].append([t, m.group('name'), int(m.group('off'), 16), int(m.group('size'), 16), dim, flags])
                    note(t, flags)
        elif f.endswith('_parameters.hpp'):
            path = None
            for line in open(os.path.join(SDK, f), encoding='utf-8', errors='replace'):
                line = line.rstrip('\n')
                m = PARAMS_COMMENT.match(line)
                if m:
                    pkg, rest = m.group(1).split('.', 1)
                    cls, fn = rest.rsplit('.', 1)
                    path = '%s.%s:%s' % (pkg, cls, fn)
                    functions.setdefault(path, dict(flags=[], params=[]))
                    continue
                if line.startswith('};'): path = None; continue
                m = FIELD.match(line)
                if m and path and not m.group('name').startswith(('Pad_', 'BitPad_')):
                    flags = _flags(m.group('flags'))
                    dim = int(m.group('dim'), 0) if m.group('dim') else 1
                    t = 'bool' if m.group('bit') else m.group('type').strip()
                    functions[path]['params'].append([t, m.group('name'), int(m.group('size'), 16), dim, flags])
                    note(t, flags)
        elif f.endswith('_functions.cpp'):
            prev = None
            for line in open(os.path.join(SDK, f), encoding='utf-8', errors='replace'):
                line = line.rstrip('\n')
                if prev:
                    if line.startswith('// (') and line.endswith(')'):
                        functions.setdefault(prev, dict(flags=[], params=[]))['flags'] = [x.strip() for x in line[4:-1].split(',')]
                    prev = None
                m = FUNC_COMMENT.match(line)
                if m: prev = '/Script/%s.%s:%s' % m.groups()
        elif f.endswith('_classes.hpp'):
            path, cur = None, None
            for line in open(os.path.join(SDK, f), encoding='utf-8', errors='replace'):
                line = line.rstrip('\n')
                m = CLASS_COMMENT.match(line)
                if m: path = m.group(1); continue
                m = CLASS_DECL.match(line)
                if m and path:
                    if m.group(1).startswith('I') and m.group(1)[1:] == path.split('.')[-1]: interfaces.append(path)
                    cur = classes[path] = dict(cpp=m.group(1), super=m.group(2), fields={})
                    path = None
                    continue
                if line.startswith('};'): cur = None; continue
                m = FIELD.match(line)
                if m and cur is not None and not m.group('name').startswith(('Pad_', 'BitPad_')):
                    t = 'bool' if m.group('bit') else m.group('type').strip()
                    cur['fields'][m.group('name')] = [t, int(m.group('size'), 16)]
    cpp_path = {v['cpp']: k for k, v in structs.items()}
    for v in structs.values(): v['super'] = cpp_path.get(v['super']) if v['super'] else None
    cls_path = {v['cpp']: k for k, v in classes.items()}
    for v in classes.values(): v['super'] = cls_path.get(v['super']) if v['super'] else None
    return dict(structs=structs, types=types, functions=functions, interfaces=sorted(set(interfaces)), classes=classes)


def table():
    if _T[0] is None:
        if os.path.exists(CACHE):
            _T[0] = json.load(open(CACHE, encoding='utf-8'))
        elif os.path.isdir(SDK):
            _T[0] = build()
            json.dump(_T[0], open(CACHE, 'w', encoding='utf-8'))
        else:
            _T[0] = False
    return _T[0] or None


# ---- lookups the rules share

def struct_link(path):
    """A native struct's PropertyLink as UStruct::Link builds it (TFieldIterator with IncludeSuper: own properties
    first, then each super's), as [(type, name, dim, flags)]; None when the dump does not have it."""
    t = table()
    if not t or path not in t['structs']: return None
    out, seen = [], set()
    while path and path in t['structs'] and path not in seen:
        seen.add(path)
        st = t['structs'][path]
        out += [(ty, name, dim, flags) for ty, name, off, size, dim, flags in st['fields']]
        path = st['super']
    return out


def function(path):
    t = table()
    return t['functions'].get(path) if t else None


def type_flags(spelling):
    """(zero_constructor, plain_old_data, no_destructor) of a C++ spelling as some dumped property of it has them, or None."""
    t = table()
    return tuple(t['types'][_norm(spelling)]) if t and _norm(spelling) in t['types'] else None


def class_chain(path):
    """A /Script class and its native supers, nearest first."""
    t, out = table(), []
    while t and path and path in t['classes'] and path not in out:
        out.append(path)
        path = t['classes'][path]['super']
    return out


def class_field(path, name):
    """(C++ spelling, size) of a /Script class's member `name`, looked up through its supers, or None."""
    t = table()
    for c in class_chain(path):
        if name in t['classes'][c]['fields']: return tuple(t['classes'][c]['fields'][name])
    return None


def find_function(cls, name):
    """A /Script class's function `name` (its own or inherited), as function() gives it, with its path."""
    t = table()
    for c in class_chain(cls):
        if c + ':' + name in t['functions']: return c + ':' + name, t['functions'][c + ':' + name]
    return None, None


def is_interface(path):
    t = table()
    return bool(t) and path in t['interfaces']
