#!/usr/bin/env python3
"""usage: genueapi.py <SDK dir> <output dir>      e.g. DrgMods/SDK/SDK  BpMods/UeApi"""
import collections
import io
import os
import re
import sys

DECL = re.compile(r"^\t(static\s+)?([A-Za-z_][\w:<>,\s\*&]*?)\s+(\w+)\((.*)\);\s*$")
# A class line this fails to match is not skipped: it silently donates its body to the class above.
CLASS = re.compile(r"^class\s+(?:\w+\([^)]*\)\s+)?((?:\w+::)?\w+)(?:\s+final)?"
                   r"(?:\s*:\s*public\s+((?:\w+::)?\w+))?\s*$")
# "// Class /Script/Engine.Actor", "// BlueprintGeneratedClass /Game/Where/Foo.Foo_C"; the full
# /Game path is only present when the dump was taken with Dumper-7's FullAssetPaths=1.
CLASS_COMMENT = re.compile(r"^// (\w+)\s+([\w/\.\-]+)\.(\w+)\s*$")
PTR = re.compile(r"^(?:const\s+)?class\s+((?:\w+::)?\w+)\s*\*$")
FIELD = re.compile(r"^\t([A-Za-z_][\w:<>,\*& ]*?)\s+([A-Za-z_]\w*)\s*(:\s*\d+)?;\s*//")
INCLUDE = re.compile(r'^#include\s+"(\w+)_classes\.hpp"')

SCALARS = {
    "void":   "void",
    "bool":   "bool",
    "float":  "float",
    "double": "double",
    "int":    "int",
    "int8":   "int8",
    "int16":  "int16",
    "int32":  "int",
    "int64":  "int64",
    "uint8":  "uint8",
    "uint16": "uint16",
    "uint32": "uint32",
    "uint64": "uint64",
}

KINDS = ("enum", "struct", "name", "container", "other")


def classify(t):
    if t.startswith("struct ") or t.startswith("const struct "):
        return "struct"
    if t.startswith(("TArray<", "TMap<", "TSet<", "const TArray<", "const TMap<", "const TSet<")):
        return "container"
    if re.match(r"^(?:const\s+)?E[A-Z]\w*&?$", t):
        return "enum"
    return "other"


TEXT_TYPES = {
    "class FString":         "FString",
    "const class FString&":  "FString",
    "const class FString &": "FString",
    "class FName":           "FName",
    "const class FName&":    "FName",
    "const class FName &":   "FName",
    "FName":                 "FName",
    "const FName&":          "FName",
    "const FName &":         "FName",
    "class FText":           "FText",
    "const class FText&":    "FText",
    "const class FText &":   "FText",
    "FText":                 "FText",
    "const FText&":          "FText",
    "const FText &":         "FText",
}


STRUCTS = {}     # cpp name -> Struct, filled by parse_structs before any class header is mapped
ENUMS = {}       # cpp name -> Enum
OUT_PTR = re.compile(r"^((?:struct\s+)?[A-Za-z_]\w*)\s*\*$")     # Dumper-7 spells a non-object out-parm as T*
STRUCT_REF = re.compile(r"^(?:const\s+)?struct\s+(F\w+)\s*&?$")
ENUM_REF = re.compile(r"^(?:const\s+)?(?:TEnumAsByte<)?(E\w+)>?\s*&?$")


def map_type(raw):
    """The C++ spelling to emit, or a KINDS reason when AssetGen cannot compile such a value."""
    t = " ".join(raw.split())
    if t in SCALARS:
        return SCALARS[t]
    if t in TEXT_TYPES:
        return TEXT_TYPES[t]
    m = PTR.match(t)
    if m:
        return "class %s*" % m.group(1)
    m = OUT_PTR.match(t)
    if m:
        inner = map_type(m.group(1))
        return "other" if inner == "void" else inner if inner in KINDS else inner + "&"
    m = STRUCT_REF.match(t)
    if m and m.group(1) in STRUCTS:
        return m.group(1)
    m = ENUM_REF.match(t)
    if m and m.group(1) in ENUMS:
        return m.group(1)
    return classify(t)


class Struct(object):
    def __init__(self, cpp, base, path, ue_name, size):
        self.cpp, self.base, self.path, self.ue_name, self.size = cpp, base, path, ue_name, size
        self.raw_fields = []     # (raw type, name) in offset order, own fields only
        self.fields = []         # (mapped type, name), base first, filled by resolve_structs
        self.complete = False    # every reflected field is emitted, so a constructor over all of them exists
        self.pkg = path[len("/Script/"):]


class Enum(object):
    def __init__(self, cpp, pkg, underlying):
        self.cpp, self.pkg, self.underlying = cpp, pkg, underlying
        self.values = []


ENUM_COMMENT = re.compile(r"^// Enum (\S+)\.(\w+)\s*$")
ENUM_DECL = re.compile(r"^enum class (\w+) : (\w+)\s*$")
ENUM_VALUE = re.compile(r"^\t(\w+)\s*=\s*(-?\d+),?\s*$")
STRUCT_COMMENT = re.compile(r"^// ScriptStruct (\S+)\.(\w+)\s*$")
STRUCT_SIZE = re.compile(r"^// 0x([0-9A-Fa-f]+) \(0x([0-9A-Fa-f]+) - 0x([0-9A-Fa-f]+)\)")
STRUCT_DECL = re.compile(r"^struct (\w+)(?:\s+final)?(?:\s*:\s*public\s+(\w+))?\s*$")
ALIGN16 = ("Quat", "Vector4", "Plane", "Matrix", "Transform")


def parse_structs(path, pkg):
    """Collects every native enum and ScriptStruct of one *_structs.hpp into ENUMS / STRUCTS."""
    text = io.open(path, encoding="utf-8", errors="replace").read()
    pending, size, cur_struct, cur_enum = None, 0, None, None
    for line in text.splitlines():
        m = ENUM_COMMENT.match(line)
        if m:
            pending = ("enum", m.group(1), m.group(2))
            continue
        m = STRUCT_COMMENT.match(line)
        if m:
            pending = ("struct", m.group(1), m.group(2))
            continue
        m = STRUCT_SIZE.match(line)
        if m:
            size = int(m.group(2), 16)
            continue
        m = ENUM_DECL.match(line)
        if m:
            cur_struct, cur_enum = None, None
            if pending and pending[0] == "enum" and "/" not in pending[1]:
                cur_enum = ENUMS[m.group(1)] = Enum(m.group(1), pkg, m.group(2))
            pending = None
            continue
        m = STRUCT_DECL.match(line)
        if m:
            cur_struct, cur_enum = None, None
            if pending and pending[0] == "struct" and pending[1].startswith("/Script/"):
                cur_struct = STRUCTS[m.group(1)] = Struct(m.group(1), m.group(2) or "", pending[1], pending[2], size)
            pending = None
            continue
        if line.startswith("};"):
            cur_struct, cur_enum = None, None
            continue
        if cur_enum is not None:
            m = ENUM_VALUE.match(line)
            if m:
                cur_enum.values.append((m.group(1), int(m.group(2))))
        elif cur_struct is not None and "//" in line:
            m = FIELD.match(line)
            if m and not m.group(2).startswith(("Pad_", "BitPad_")):
                cur_struct.raw_fields.append(("bool" if m.group(3) else m.group(1), m.group(2)))


def resolve_structs():
    """Second pass: map field types now that every struct and enum name is known."""
    done = set()

    def resolve(st):
        if st.cpp in done:
            return
        done.add(st.cpp)
        base = STRUCTS.get(st.base)
        if base:
            resolve(base)
        own, complete = [], base.complete if base else True
        for raw, name in st.raw_fields:
            mapped = map_type(raw)
            if mapped in KINDS or mapped == "void":
                complete = False
                continue
            own.append((mapped, name))
        st.fields = (base.fields if base else []) + own
        st.complete = complete and bool(st.fields)

    for st in list(STRUCTS.values()):
        resolve(st)


def struct_align(st):
    if st.ue_name in ALIGN16:
        return 16
    if st.size % 8 == 0 and any(t in ("int64", "uint64", "double", "FString", "FText") or t.endswith("*")
                                for t, _ in st.fields):
        return 8
    return 4 if st.size % 4 == 0 else 1


def struct_deps(st):
    """Struct names this one must be declared after."""
    out = [st.base] if st.base in STRUCTS else []
    out += [t for t, _ in st.fields if t in STRUCTS and t != st.cpp]
    return out


def emit_struct(st, conv_names):
    body = ["struct %s%s" % (st.cpp, (" : public %s" % st.base) if st.base in STRUCTS else ""), "{"]
    base = STRUCTS.get(st.base)
    own = st.fields[len(base.fields):] if base else st.fields
    for t, n in own:
        body.append("    %s %s;" % (t, n))
    body.append("")
    body.append("    %s() = default;" % st.cpp)
    if st.complete:
        body.append("    %s(%s) {}" % (st.cpp, ", ".join("%s %s" % (t, n) for t, n in st.fields)))
    if st.cpp in conv_names:
        body.append("    UE_CONV_%s" % st.cpp)
    body.append("};")
    return "\n".join(body)


def emit_enum(en):
    width = max([len(n) for n, _ in en.values] + [1])
    body = ["enum class %s : %s" % (en.cpp, en.underlying), "{"]
    body += ["    %-*s = %d," % (width, n, v) for n, v in en.values]
    body.append("};")
    return "\n".join(body)


OPS = {"Add": "+", "Subtract": "-", "Multiply": "*", "Divide": "/", "EqualEqual": "==", "NotEqual": "!=",
       "Less": "<", "Greater": ">", "LessEqual": "<=", "GreaterEqual": ">="}
OP_FN = re.compile(r"^(%s)_(\w+)$" % "|".join(OPS))


def operator_extra(params):
    return [(0.0001 if "Tolerance" in n else 0.0) if t == "float" else (False if t == "bool" else 0) for t, n in params]


def operators(classes):
    """(op, lhs, rhs) -> (ret, package, class, fn, extra) for every Kismet <Op>_<A><B> over a struct."""
    found = {}
    for k in sorted(classes, key=lambda k: (k.path != "/Script/Engine", k.path, k.cpp)):
        if k.is_bp:
            continue
        for is_static, ret, fname, params in k.funcs:
            m = OP_FN.match(fname)
            if not (m and is_static and len(params) >= 2):
                continue
            lhs, rhs = params[0][0], params[1][0]
            if lhs not in STRUCTS and rhs not in STRUCTS:
                continue
            found.setdefault((OPS[m.group(1)], lhs, rhs), (ret, k.path, k.ue_name, fname, operator_extra(params[2:])))
    return found


def write_operators(classes, out_dir):
    ops = operators(classes)
    by_pkg = {}
    for (op, lhs, rhs), (ret, pkg, cls, fn, extra) in ops.items():
        by_pkg.setdefault(pkg[len("/Script/"):], []).append("inline %s operator%s(const %s&, const %s&) { return {}; }"
                                                              % (ret, op, lhs, rhs))
    rows = []
    for (op, lhs, rhs), (ret, pkg, cls, fn, extra) in sorted(ops.items()):
        args = ", ".join(("true" if a else "false") if isinstance(a, bool) else repr(a) for a in extra)
        rows.append('  {"op": "%s", "lhs": "%s", "rhs": "%s", "ret": "%s", "package": "%s", "class": "%s", "fn": "%s", "extra": [%s]}'
                    % (op, lhs, rhs, ret, pkg, cls, fn, args))
    io.open(os.path.join(out_dir, "Ops.json"), "w", encoding="utf-8", newline="\n").write("[\n" + ",\n".join(rows) + "\n]\n")
    print("  operators: %d" % len(ops))
    return by_pkg


def write_types(out_dir):
    rows = ['{', '  "enums": {']
    ens = sorted(ENUMS.values(), key=lambda e: e.cpp)
    rows += ['    "%s": {"package": "/Script/%s", "name": "%s", "underlying": "%s"}%s'
             % (e.cpp, e.pkg, e.cpp, e.underlying, "," if i + 1 < len(ens) else "") for i, e in enumerate(ens)]
    rows += ['  },', '  "structs": {']
    sts = sorted(STRUCTS.values(), key=lambda t: t.cpp)
    for i, st in enumerate(sts):
        fields = ", ".join('["%s", "%s"]' % (t, n) for t, n in st.fields)
        rows.append('    "%s": {"package": "%s", "name": "%s", "size": %d, "align": %d, "complete": %s, "fields": [%s]}%s'
                    % (st.cpp, st.path, st.ue_name, st.size, struct_align(st), "true" if st.complete else "false",
                       fields, "," if i + 1 < len(sts) else ""))
    rows += ['  }', '}']
    io.open(os.path.join(out_dir, "Types.json"), "w", encoding="utf-8", newline="\n").write("\n".join(rows) + "\n")
    print("  types: %d enums, %d structs (%d complete)"
          % (len(ENUMS), len(STRUCTS), sum(1 for t in STRUCTS.values() if t.complete)))


def split_params(text):
    out, depth, start = [], 0, 0
    for i, c in enumerate(text):
        if c in "<(":
            depth += 1
        elif c in ">)":
            depth -= 1
        elif c == "," and depth == 0:
            out.append(text[start:i])
            start = i + 1
    if text[start:].strip():
        out.append(text[start:])
    return out


def parse_params(text):
    """[(type, name)], or a KINDS reason if any parameter is out of reach."""
    if not text.strip():
        return []
    out = []
    for raw in split_params(text):
        p = raw.strip()
        m = re.match(r"^(.*?)([A-Za-z_]\w*)$", p)
        if not m:
            return "other"
        mapped = map_type(m.group(1))
        if mapped in KINDS:
            return mapped
        if mapped == "void":
            return "other"
        out.append((mapped, m.group(2)))
    return out


def parse_field(cur, m, skipped):
    raw, fname, bits = m.group(1), m.group(2), m.group(3)
    if fname.startswith(("Pad_", "BitPad_")):
        return
    mapped = "bool" if bits else map_type(raw)
    if mapped in KINDS or mapped == "void":
        skipped[mapped if mapped in KINDS else "other"] += 1
        return
    cur.fields.append((mapped, fname))


class Klass(object):
    """`cpp` / `base` are Dumper-7's (possibly namespace-qualified) spellings and only join keys;
    `path` + `ue_name` are what UE_CLASS carries; `emit` / `header` are filled once all are known."""

    def __init__(self, cpp, base, path, ue_name, is_bp):
        self.cpp, self.base, self.path, self.ue_name = cpp, base, path, ue_name
        self.is_bp = is_bp
        self.ns = ""
        self.emit = ""
        self.header = ""
        self.funcs = []
        self.fields = []


def parse_header(path):
    text = io.open(path, encoding="utf-8", errors="replace").read()
    classes, cur, pending = [], None, None
    skipped = dict((k, 0) for k in KINDS)
    includes = []
    for line in text.splitlines():
        m = INCLUDE.match(line)
        if m:
            includes.append(m.group(1))
        m = CLASS_COMMENT.match(line)
        if m:
            pending = m.groups()
            continue
        m = CLASS.match(line)
        if m:
            cur = None
            if pending:
                kind, path, ue_name = pending
                is_bp = kind.endswith("BlueprintGeneratedClass")
                if is_bp or kind == "Class":
                    cur = Klass(m.group(1), m.group(2) or "", path, ue_name, is_bp)
                    classes.append(cur)
            pending = None
            continue
        if cur is None:
            continue
        if "//" in line:
            m = FIELD.match(line)
            if m:
                parse_field(cur, m, skipped)
            continue
        m = DECL.match(line)
        if not m:
            continue
        ret = map_type(m.group(2))
        params = parse_params(m.group(4))
        if ret in KINDS or isinstance(params, str):
            skipped[ret if ret in KINDS else params] += 1
            continue
        cur.funcs.append((bool(m.group(1)), ret, m.group(3), params))
    return classes, skipped, includes


CONV = re.compile(r"^Conv_(\w+)To(\w+)$")
CONV_SCALARS = ("FString", "FName", "FText", "int", "int64", "float", "bool", "uint8", "class UObject*")
CONV_SKIP = ("Conv_RotatorToVector",)      # a rotator is not implicitly a direction


def conv_kind(t):
    return t if t in CONV_SCALARS or t in STRUCTS else None
# Formatting parameters after the value take these; a Conv_ with any other extra parameter is skipped.
CONV_DEFAULTS = {"bAlwaysSign": False, "bUseGrouping": False,
                 "MinimumIntegralDigits": 1, "MaximumIntegralDigits": 324}
CONV_STRUCTS = ("FString", "FName", "FText")


def conversions(classes):
    """(from, to) -> (package, class, fn, extra args), preferring /Script/Engine, then direct over via-string."""
    found = {}
    for k in sorted(classes, key=lambda k: (k.path != "/Script/Engine", k.path, k.cpp)):
        if k.is_bp:
            continue
        for is_static, ret, fname, params in k.funcs:
            m = CONV.match(fname)
            if not (m and is_static and params):
                continue
            src, dst = params[0][0], ret
            if not conv_kind(src) or not conv_kind(dst) or src == dst or fname in CONV_SKIP:
                continue
            if any(n not in CONV_DEFAULTS for _, n in params[1:]):
                continue
            extra = [CONV_DEFAULTS[n] for _, n in params[1:]]
            found.setdefault((src, dst), (k.path, k.ue_name, fname, extra))
    return found


def write_conversions(classes, out_dir):
    direct = conversions(classes)
    table = ['[']
    for (src, dst), (pkg, cls, fn, extra) in sorted(direct.items()):
        args = ", ".join(("true" if a else "false") if isinstance(a, bool) else str(a) for a in extra)
        table.append('  {"from": "%s", "to": "%s", "package": "%s", "class": "%s", "fn": "%s", "extra": [%s]},'
                     % (src, dst, pkg, cls, fn, args))
    table[-1] = table[-1].rstrip(",")
    table.append(']')
    io.open(os.path.join(out_dir, "Conv.json"), "w", encoding="utf-8", newline="\n").write("\n".join(table) + "\n")

    reachable = set(direct)
    for (a, b) in direct:
        if b == "FString":
            reachable.update((a, d) for (s, d) in direct if s == "FString" and d != a)
    targets = tuple(CONV_STRUCTS) + tuple(sorted(t for (_, t) in reachable if t in STRUCTS))
    out = ["#pragma once",
           "/* Every Kismet Conv_XToY, generated by AssetGen/tools/genueapi.py. Do not edit.",
           "   A struct converts from anything a Conv_ reaches (directly or via FString) with a",
           "   constructor, and to a scalar with an explicit operator: `int(Str)`. */",
           "class UObject;", ""]
    out += ["struct %s;" % t for t in sorted(set(t for pair in reachable for t in pair if t in STRUCTS))]
    for t in targets:
        members = []
        for (src, dst) in sorted(reachable):
            if dst == t:
                arg = ("const %s&" % src) if (src in CONV_STRUCTS or src in STRUCTS) else src
                members.append("    %s(%s) {}" % (t, arg))
            elif src == t and dst not in CONV_STRUCTS and dst not in STRUCTS:
                members.append("    explicit operator %s() const { return {}; }" % dst)
        if members:
            out.append("#define UE_CONV_%s \\\n%s" % (t, " \\\n".join(members)))
            out.append("")
    io.open(os.path.join(out_dir, "Conv.h"), "w", encoding="utf-8-sig", newline="\n").write("\n".join(out))
    print("  conversions: %d direct, %d reachable" % (len(direct), len(reachable)))
    return set(t for t in targets if t in STRUCTS)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip().splitlines()[-1])
    sdk_dir, out_dir = sys.argv[1], sys.argv[2]

    for name in sorted(f for f in os.listdir(sdk_dir) if f.endswith("_structs.hpp")):
        parse_structs(os.path.join(sdk_dir, name), name[: -len("_structs.hpp")])
    resolve_structs()

    headers = sorted(f for f in os.listdir(sdk_dir) if f.endswith("_classes.hpp"))
    classes, totals, pathless = [], dict((k, 0) for k in KINDS), 0
    for name in headers:
        found, skipped, _ = parse_header(os.path.join(sdk_dir, name))
        if not found:
            continue
        for k in found:
            if not k.is_bp:
                k.path = "/Script/" + name[: -len("_classes.hpp")]
        keep = [k for k in found if not k.is_bp or k.path.startswith("/")]
        pathless += len(found) - len(keep)
        classes.extend(keep)
        for k in KINDS:
            totals[k] += skipped[k]

    # A Blueprint is namespaced by its /Game path, never by Dumper-7's per-dump InitCave_N suffix.
    def namespace_of(path):
        parts = []
        for seg in path.strip("/").split("/")[:-1]:
            seg = re.sub(r"\W", "_", seg)
            parts.append("_" + seg if seg[:1].isdigit() else seg)
        return "::".join(parts)

    name_count = collections.Counter(k.ue_name for k in classes)
    for k in classes:
        k.ns = namespace_of(k.path) if k.is_bp else ""
        k.emit = (k.ns + "::" + k.ue_name) if k.ns else (k.ue_name if k.is_bp else k.cpp)
        k.header = ("Game/" + k.ue_name) if k.is_bp else k.path[len("/Script/"):]
    unique = set(n for n, c in name_count.items() if c == 1)

    by_name = dict((k.cpp, k) for k in classes)
    ordered, seen = [], set()

    def place(k):
        if k.cpp in seen:
            return True
        seen.add(k.cpp)
        if k.base:
            base = by_name.get(k.base)
            if base is None or not place(base):
                seen.discard(k.cpp)
                return False
        ordered.append(k)
        return True

    sys.setrecursionlimit(10000)
    for k in classes:
        place(k)

    by_pkg = {}
    for k in ordered:
        by_pkg.setdefault(k.header, []).append(k)

    for st in STRUCTS.values():
        by_pkg.setdefault(st.pkg, [])
    for en in ENUMS.values():
        by_pkg.setdefault(en.pkg, [])

    deps = {}
    for pkg, members in by_pkg.items():
        deps[pkg] = set(by_name[k.base].header for k in members
                        if k.base and by_name[k.base].header != pkg)
    for st in STRUCTS.values():
        deps[st.pkg].update(STRUCTS[d].pkg for d in struct_deps(st) if STRUCTS[d].pkg != st.pkg)
        deps[st.pkg].update(ENUMS[t].pkg for t, _ in st.fields if t in ENUMS and ENUMS[t].pkg != st.pkg)
    for k in ordered:
        for t in [t for _, r, _, ps in k.funcs for t in [r] + [pt for pt, _ in ps]] + [t for t, _ in k.fields]:
            if t in STRUCTS and STRUCTS[t].pkg != k.header:
                deps[k.header].add(STRUCTS[t].pkg)
            elif t in ENUMS and ENUMS[t].pkg != k.header:
                deps[k.header].add(ENUMS[t].pkg)

    colour = dict((pkg, 0) for pkg in by_pkg)

    def acyclic(pkg, trail):
        colour[pkg] = 1
        for dep in deps[pkg]:
            if colour[dep] == 1:
                sys.exit("base-class cycle between packages: %s" % " -> ".join(trail + [dep]))
            if colour[dep] == 0:
                acyclic(dep, trail + [dep])
        colour[pkg] = 2

    for pkg in by_pkg:
        if colour[pkg] == 0:
            acyclic(pkg, [pkg])

    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    conv_structs = write_conversions(ordered, out_dir)
    ops_by_pkg = write_operators(ordered, out_dir)
    write_types(out_dir)

    def rewrite(ctype):
        m = PTR.match(ctype)
        target = by_name.get(m.group(1)) if m else None
        return "class %s*" % target.emit if target else ctype

    funcs, fields, aliased = 0, 0, 0
    for pkg, members in sorted(by_pkg.items()):
        body, referenced = [], set()
        defined = set(k.cpp for k in members)
        ns_open = None
        for en in sorted((e for e in ENUMS.values() if e.pkg == pkg), key=lambda e: e.cpp):
            body.append(emit_enum(en) + "\n")
        placed = set()

        def place_struct(st):
            if st.cpp in placed:
                return
            placed.add(st.cpp)
            for d in struct_deps(st):
                if STRUCTS[d].pkg == pkg:
                    place_struct(STRUCTS[d])
            body.append(emit_struct(st, conv_structs) + "\n")

        for st in sorted((t for t in STRUCTS.values() if t.pkg == pkg), key=lambda t: t.cpp):
            place_struct(st)
        for k in members:
            if k.ns != ns_open:
                if ns_open:
                    body.append("}   // namespace %s\n" % ns_open)
                if k.ns:
                    body.append("namespace %s {\n" % k.ns)
                ns_open = k.ns
            base = by_name[k.base].emit if k.base else ""
            inherits = " : public %s" % base if base else ""
            body.append("class %s%s\n{\npublic:\n    UE_CLASS(\"%s\", \"%s\");"
                        % (k.ue_name if k.is_bp else k.cpp, inherits, k.path, k.ue_name))
            names = set(f for _, _, f, _ in k.funcs)
            for ftype, fname in k.fields:
                if fname in names:
                    continue
                body.append("    %s %s;" % (rewrite(ftype), fname))
                fields += 1
                m = PTR.match(ftype)
                if m:
                    referenced.add(m.group(1))
            for is_static, ret, fname, params in k.funcs:
                args = ", ".join("%s %s" % (rewrite(t), n) for t, n in params)
                body.append("    %s%s %s(%s);"
                            % ("static " if is_static else "", rewrite(ret), fname, args))
                funcs += 1
                for t in [ret] + [t for t, _ in params]:
                    m = PTR.match(t)
                    if m:
                        referenced.add(m.group(1))
            body.append("};\n")
        if ns_open:
            body.append("}   // namespace %s\n" % ns_open)

        body += ops_by_pkg.get(pkg, [])

        # An ambiguous name gets no alias, so naming it bare fails to compile instead of resolving
        # to whichever asset was emitted last.
        for k in members:
            if k.ns and k.ue_name in unique:
                body.append("using %s = %s;" % (k.ue_name, k.emit))
                aliased += 1

        title = ("Blueprint class " + pkg[len("Game/"):]) if pkg.startswith("Game/") \
            else "Package /Script/" + pkg
        out = ["#pragma once",
               "/*",
               "%s, generated by AssetGen/tools/genueapi.py. Do not edit." % title,
               "",
               "A member is here if and only if AssetGen can compile a use of it.",
               "*/",
               "#include \"UeMeta.h\""]
        out += ["#include \"%s.h\"" % d for d in sorted(deps[pkg])]
        out += [""]
        fwd = {}
        for c in sorted(referenced - defined):
            target = by_name.get(c)
            if target:
                fwd.setdefault(target.ns, []).append(target.ue_name if target.is_bp else target.cpp)
        for ns in sorted(fwd):
            decls = ["class %s;" % n for n in fwd[ns]]
            out += ["namespace %s { %s }" % (ns, " ".join(decls))] if ns else decls
        if fwd:
            out += [""]
        out += body
        dest = os.path.join(out_dir, pkg + ".h")
        if not os.path.isdir(os.path.dirname(dest)):
            os.makedirs(os.path.dirname(dest))
        io.open(dest, "w", encoding="utf-8-sig", newline="\n").write("\n".join(out))

    reach = ", ".join("%s %d" % (k, totals[k]) for k in KINDS if totals[k])
    umbrella = ["#pragma once",
                "/*",
                "UeApi.h - every UE class a Blueprint mod can address.",
                "",
                "Generated by AssetGen/tools/genueapi.py. Do not edit.",
                "",
                "A member is here if and only if AssetGen can compile a use of it, so clang",
                "accepting a mod source is the same statement as AssetGen being able to generate it.",
                "%d classes, %d functions, %d properties across %d headers."
                % (len(ordered), funcs, fields, len(by_pkg)),
                "",
                "Native packages only. A Blueprint class lives in a header of its own name under",
                "Game/, because there are thousands of them and a mod wants one: include",
                "\"Game/BP_Foo_C.h\" and write BP_Foo_C, or its namespaced spelling if the name is",
                "one of the few that several assets share.",
                "",
                "Including this pulls in all of them, which is convenient and slow: it costs clang a",
                "56 MB AST dump for a twenty-line mod. Prefer naming the two or three packages the",
                "mod actually uses.",
                "",
                "TODO: unimplemented value kinds held %d members back - %s." % (sum(totals.values()), reach),
                "*/"]
    umbrella += ["#include \"%s.h\"" % pkg for pkg in sorted(by_pkg) if not pkg.startswith("Game/")]
    io.open(os.path.join(out_dir, "UeApi.h"), "w", encoding="utf-8-sig", newline="\n").write("\n".join(umbrella) + "\n")

    bp = [k for k in ordered if k.is_bp]
    print("UeApi: %d classes, %d functions, %d properties, %d headers"
          % (len(ordered), funcs, fields, len(by_pkg)))
    print("  blueprint classes: %d, of which %d reachable unqualified" % (len(bp), aliased))
    print("  out of reach: " + reach)
    if pathless:
        print("  blueprint classes dropped for want of a /Game path: %d"
              " (re-dump with Dumper-7 FullAssetPaths=1)" % pathless)


if __name__ == "__main__":
    main()
