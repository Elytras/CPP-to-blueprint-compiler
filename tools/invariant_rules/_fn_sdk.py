"""The native half of the FUNC rules: every /Script function's EFunctionFlags and parameter block, and every native
class's super, read once from a Dumper-7 dump of the game (its *_functions.cpp flag comments, *_parameters.hpp
structs and *_classes.hpp class comments) and cached as JSON.

usage: sdk_table.py <Dumper-7 SDK dir> <out.json>
As a module: load(json_path) -> Table (flags, params, supers)."""
import io, json, os, re, sys

FUNC_BITS = {"Final": 0x1, "RequiredAPI": 0x2, "BlueprintAuthorityOnly": 0x4, "BlueprintCosmetic": 0x8, "Net": 0x40,
             "NetReliable": 0x80, "NetRequest": 0x100, "Exec": 0x200, "Native": 0x400, "Event": 0x800,
             "NetResponse": 0x1000, "Static": 0x2000, "NetMulticast": 0x4000, "UbergraphFunction": 0x8000,
             "MulticastDelegate": 0x10000, "Public": 0x20000, "Private": 0x40000, "Protected": 0x80000,
             "Delegate": 0x100000, "NetServer": 0x200000, "HasOutParams": 0x400000, "HasDefaults": 0x800000,
             "NetClient": 0x1000000, "DLLImport": 0x2000000, "BlueprintCallable": 0x4000000,
             "BlueprintEvent": 0x8000000, "BlueprintPure": 0x10000000, "EditorOnly": 0x20000000,
             "Const": 0x40000000, "NetValidate": 0x80000000}
PARM_BITS = {"ConstParm": 0x2, "Parm": 0x80, "OutParm": 0x100, "ReturnParm": 0x400, "ReferenceParm": 0x8000000}

CLASS_COMMENT = re.compile(r"^// Class (/Script/[\w/]+)\.(\w+)\n(?:(?://|#)[^\n]*\n)*class\s+(?:\w+\(\w+\)\s+)?(\w+)(?:\s+final)?"
                           r"(?:\s*:\s*public\s+(\w+))?", re.M)
FUNC = re.compile(r"^// Function ([^.\n]+)\.([^.\n]+)\.([^\n]+)\n// \(([^)]*)\)", re.M)
PARAMS = re.compile(r"^// Function (/Script/[\w/]+)\.(\w+)\.([^\n]+)\n// [^\n]*\nstruct \w+ final\n\{\n(.*?)^\};", re.M | re.S)
MEMBER = re.compile(r"^\t(.+?)\s+(\w+)(?:\[0x[0-9A-Fa-f]+\])?(?:\s*:\s*\d+)?;\s*// 0x([0-9A-F]+)\(0x([0-9A-F]+)\)\(([^)]*)\)", re.M)


def build(sdk):
    files = os.listdir(sdk)
    text = lambda f: io.open(os.path.join(sdk, f), encoding='utf-8', errors='replace').read()
    path_of, cpp_super, supers = {}, {}, {}
    for f in files:
        if not f.endswith('_classes.hpp'): continue
        for m in CLASS_COMMENT.finditer(text(f)):
            path = '%s.%s' % (m.group(1), m.group(2))
            path_of[m.group(3)] = path
            cpp_super[path] = m.group(4)
    for path, sup in cpp_super.items():
        supers[path] = path_of.get(sup) if sup else None
    # A native interface is Dumper-7's `class IName final` with no base (its UInterface twin is `class UName`).
    interfaces = sorted(p for c, p in path_of.items() if c[0] == 'I' and not cpp_super[p] and p != '/Script/CoreUObject.Interface')
    native = {p.split('/')[-1] for p in path_of.values()}          # "Engine.Actor"
    flags, params = {}, {}
    for f in files:
        if f.endswith('_functions.cpp'):
            for m in FUNC.finditer(text(f)):
                pkg, cls, real = m.group(1), m.group(2), m.group(3).rstrip()
                if '%s.%s' % (pkg, cls) not in native: continue
                flags['/Script/%s.%s:%s' % (pkg, cls, real)] = sum(FUNC_BITS.get(w, 0) for w in m.group(4).split(', '))
        elif f.endswith('_parameters.hpp'):
            for m in PARAMS.finditer(text(f)):
                rows = []
                for mm in MEMBER.finditer(m.group(4)):
                    words = mm.group(5).split(', ')
                    if 'Parm' not in words: continue                     # Dumper-7's own padding
                    rows.append([mm.group(2), ' '.join(mm.group(1).split()), int(mm.group(3), 16), int(mm.group(4), 16),
                                 sum(PARM_BITS.get(w, 0) for w in words)])
                params['%s.%s:%s' % (m.group(1), m.group(2), m.group(3).rstrip())] = rows
    return dict(flags=flags, params=params, supers=supers, interfaces=interfaces)


class Table:
    def __init__(s, d): s.flags, s.params, s.supers, s.interfaces = d['flags'], d['params'], d['supers'], set(d['interfaces'])
    def functions_of(s, cls_path):
        pre = cls_path + ':'
        return {k[len(pre):]: v for k, v in s.flags.items() if k.startswith(pre)}


def load(path):
    return Table(json.load(open(path, encoding='utf-8')))


if __name__ == '__main__':
    d = build(sys.argv[1])
    json.dump(d, open(sys.argv[2], 'w', encoding='utf-8'))
    print('flags %d, params %d, classes %d' % (len(d['flags']), len(d['params']), len(d['supers'])))
