#!/usr/bin/env python3
"""usage: genueapi.py <SDK dir> <output dir>      e.g. DrgMods/SDK/SDK  BpMods/UeApi"""
import collections
import io
import os
import re
import sys

DECL = re.compile(r"^\t(static\s+)?([A-Za-z_][\w:<>,\s\*&]*?)\s+(\w+)\((.*)\)(\s*const)?;\s*$")
# A class line this fails to match is not skipped: it silently donates its body to the class above.
CLASS = re.compile(r"^class\s+(?:\w+\([^)]*\)\s+)?((?:\w+::)?\w+)(?:\s+final)?"
                   r"(?:\s*:\s*public\s+((?:\w+::)?\w+))?\s*$")
# "// Class /Script/Engine.Actor", "// BlueprintGeneratedClass /Game/Where/Foo.Foo_C"; the full
# /Game path is only present when the dump was taken with Dumper-7's FullAssetPaths=1.
CLASS_COMMENT = re.compile(r"^// (\w+)\s+([\w/\.\-]+)\.(\w+)\s*$")
PTR = re.compile(r"^(?:const\s+)?class\s+((?:\w+::)?\w+)\s*\*$")
TPL = re.compile(r"^(?:const\s+)?(TSubclassOf|TSoftObjectPtr|TSoftClassPtr|TScriptInterface)<class\s+((?:\w+::)?\w+)>\s*&?$")
# A sparse delegate's second argument is its name as a string, which a C++ template cannot take; it is dropped.
DELEGATE = re.compile(r'^(?:const\s+)?(TDelegate|TMulticastInlineDelegate|TMulticastSparseDelegate)'
                      r'<([\w\s\*&]+?)\((.*)\)(?:,\s*"\w+")?>\s*&?$')


CLASS_WORD = re.compile(r"class\s+((?:\w+::)?\w+)")
CONTAINER = re.compile(r"^(?:const\s+)?(TArray|TSet|TMap)<(.*)>\s*&?$")


def class_refs(t):
    """Every class a spelling names: `class X*`, `TWrapper<class X>`, `TArray<class X*>`, ..."""
    return CLASS_WORD.findall(t)


def split_args(text):
    return [a.strip() for a in split_params(text)]
FIELD = re.compile(r"^\t([A-Za-z_][\w:<>,\*& ()\"]*?)\s+([A-Za-z_]\w*)\s*(:\s*\d+)?;\s*//")
NO_STRUCT_LITERAL = ("TDelegate<", "TMulticast", "TScriptInterface<")    # EX_StructConst has no zero for these
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
OUT_PTR = re.compile(r"^((?:struct\s+)?[A-Za-z_][\w<>, \*:]*?)\s*\*$")     # Dumper-7 spells a non-object out-parm as T*
STRUCT_REF = re.compile(r"^(?:const\s+)?struct\s+(F\w+)\s*&?$")
ENUM_REF = re.compile(r"^(?:const\s+)?(?:TEnumAsByte<)?(E\w+)>?\s*&?$")


def map_type(raw):
    """The C++ spelling to emit, or a KINDS reason when AssetGen cannot compile such a value."""
    t = " ".join(raw.split())
    base = t[6:] if t.startswith("const ") else t          # `const int32&`: a by-value scalar to the caller
    base = base[:-1].rstrip() if base.endswith("&") else base
    if base in SCALARS:
        return SCALARS[base]
    if base in TEXT_TYPES:
        return TEXT_TYPES[base]
    m = PTR.match(t)
    if m:
        return "class %s*" % m.group(1)
    m = TPL.match(t)
    if m:
        return "%s<class %s>" % (m.group(1), m.group(2))
    m = DELEGATE.match(t)
    if m:
        ret, params = map_type(m.group(2)), parse_params(m.group(3))
        if ret in KINDS or isinstance(params, str):
            return "other"
        return "%s<%s(%s)>" % (m.group(1), ret, ", ".join("%s %s" % p for p in params))
    m = CONTAINER.match(t)
    if m:
        args = [map_type(a) for a in split_args(m.group(2))]
        if any(a in KINDS or a == "void" or a.endswith("&") for a in args) or len(args) != (2 if m.group(1) == "TMap" else 1):
            return "container"
        return "%s<%s>" % (m.group(1), ", ".join(args))
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
# Dumper-7 writes alignas(N) when the struct's MinAlignment is more than its members give (FQuat, FPlane).
STRUCT_DECL = re.compile(r"^struct (?:alignas\((0x[0-9A-Fa-f]+)\)\s+)?(\w+)(?:\s+final)?(?:\s*:\s*public\s+(\w+))?\s*$")


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
                cur_struct = STRUCTS[m.group(2)] = Struct(m.group(2), m.group(3) or "", pending[1], pending[2], size)
                cur_struct.explicit_align = int(m.group(1), 16) if m.group(1) else 1
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
            if mapped in KINDS or mapped == "void" or any(s in mapped for s in NO_STRUCT_LITERAL):
                complete = False
                continue
            own.append((mapped, name))
        st.fields = (base.fields if base else []) + own
        st.complete = complete and bool(st.fields)

    for st in list(STRUCTS.values()):
        resolve(st)


ALIGN_BY_NAME = {"bool": 1, "uint8": 1, "int8": 1, "char": 1, "int16": 2, "uint16": 2, "wchar_t": 2,
                 "int32": 4, "uint32": 4, "float": 4, "FName": 4, "TWeakObjectPtr": 4, "TLazyObjectPtr": 4,
                 "int64": 8, "uint64": 8, "double": 8, "FString": 8, "FText": 8, "TArray": 8, "TSet": 8, "TMap": 8,
                 "TSoftObjectPtr": 8, "TSoftClassPtr": 8, "TSubclassOf": 8, "TScriptInterface": 8, "TDelegate": 8,
                 "TMulticastInlineDelegate": 8, "TMulticastSparseDelegate": 8, "TFieldPath": 8, "FScriptDelegate": 8}
_ALIGN = {}


def struct_align(st):
    """The struct's alignment as C++ lays out the dump: its largest member's (recursively), raised to an
    explicit alignas. A member type this cannot read counts as 8, the most a dumped type needs short of alignas."""
    if st.cpp in _ALIGN:
        return _ALIGN[st.cpp]
    _ALIGN[st.cpp] = 8                       # a cycle through a pointer cannot recurse; pointers are 8 anyway
    align = getattr(st, "explicit_align", 1)
    if st.base in STRUCTS:
        align = max(align, struct_align(STRUCTS[st.base]))
    for raw, _ in st.raw_fields:
        t = re.sub(r"^(const|struct|class|enum)\s+", "", raw.strip())
        if t.endswith("*") or t.endswith("&"):
            a = 8
        else:
            head = re.match(r"\w+", t)
            head = head.group(0) if head else t
            if head in ALIGN_BY_NAME:
                a = ALIGN_BY_NAME[head]
            elif head in STRUCTS:
                a = struct_align(STRUCTS[head])
            elif head in ENUMS:
                a = ALIGN_BY_NAME.get(ENUMS[head].underlying, 8)
            else:
                a = 8
        align = max(align, a)
    _ALIGN[st.cpp] = align
    return align


def struct_deps(st):
    """Struct names this one must be declared after."""
    out = [st.base] if st.base in STRUCTS else []
    for t, _ in st.fields:
        for word in re.findall(r"\bF\w+", t):          # the struct itself, or a container's element / key / value
            if word in STRUCTS and word != st.cpp:
                out.append(word)
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
    rows += ['    "%s": {"package": "/Script/%s", "name": "%s", "underlying": "%s", "first": "%s"}%s'
             % (e.cpp, e.pkg, e.cpp, e.underlying, e.values[0][0] if e.values else "", "," if i + 1 < len(ens) else "")
             for i, e in enumerate(ens)]
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
        self.const_funcs = set()
        self.raw_funcs = []      # (return, name, params) as Dumper-7 spelled them, before any mapping
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
        cur.raw_funcs.append((m.group(2), m.group(3), m.group(4)))
        ret = map_type(m.group(2))
        params = parse_params(m.group(4))
        if ret in KINDS or isinstance(params, str):
            skipped[ret if ret in KINDS else params] += 1
            continue
        cur.funcs.append((bool(m.group(1)), ret, m.group(3), params))
        if m.group(5):
            cur.const_funcs.add(m.group(3))
    return classes, skipped, includes


CONV = re.compile(r"^Conv_(\w+)To(\w+)$")
CONV_SCALARS = ("FString", "FName", "FText", "int", "int64", "float", "bool", "uint8", "class UObject*")
# Conv_RotatorToVector: a rotator is not implicitly a direction. Conv_Int64ToString: only Modio has it;
# the compiler goes through Conv_Int64ToText + Conv_TextToString instead.
CONV_SKIP = ("Conv_RotatorToVector", "Conv_Int64ToString")


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
    for (a, b) in direct:       # two steps through FString or FText, as the compiler's ConvertArg tries them
        if b in ("FString", "FText"):
            reachable.update((a, d) for (s, d) in direct if s == b and d != a)
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


# Kismet's container libraries take wildcards, which Dumper-7 spells as int32. Each <Prefix>X is emitted
# as method X of the template its first parameter names, not as a static of the library:
# Array_Add(const TArray<int32>& TargetArray, const int32& NewItem) is TArray<T>::Add(const T& NewItem).
# AssetGen compiles `Items.Add(X)` back to Array_Add(Items, X).
CONTAINER_LIBS = {"KismetArrayLibrary":  ("Array_", "TArray"),
                  "BlueprintSetLibrary": ("Set_", "TSet"),
                  "BlueprintMapLibrary": ("Map_", "TMap")}
FUNC_FLAGS = re.compile(r"^// Function \w+\.(\w+)\.(\w+)\n// \(([^)]*)\)", re.M)


def is_container_method(k, fname):
    lib = CONTAINER_LIBS.get(k.ue_name)
    return bool(lib) and not k.is_bp and fname.startswith(lib[0])


def wildcard_param(raw, name, tpl):
    """A container-library parameter in template terms, or None if it is not a wildcard: any int32
    passed by reference is one, except the real `int32* OutIndex` of Array_Random."""
    t = " ".join(raw.split())
    out = t.endswith("*")
    if not (out or t.endswith("&")) or "int32" not in t or name.endswith("Index"):
        return None
    core = re.sub(r"^const\s+", "", t)[:-1].rstrip()
    elem = ("K" if "Key" in name else "V") if tpl == "TMap" else "T"
    core = core.replace("TMap<int32, int32>", "TMap<K, V>").replace("int32", elem)
    return core + "&" if out else "const %s&" % core


FUNC_BITS = {"Final": 0x1, "RequiredAPI": 0x2, "BlueprintAuthorityOnly": 0x4, "BlueprintCosmetic": 0x8, "Net": 0x40,
             "NetReliable": 0x80, "NetRequest": 0x100, "Exec": 0x200, "Native": 0x400, "Event": 0x800,
             "NetResponse": 0x1000, "Static": 0x2000, "NetMulticast": 0x4000, "UbergraphFunction": 0x8000,
             "MulticastDelegate": 0x10000, "Public": 0x20000, "Private": 0x40000, "Protected": 0x80000,
             "Delegate": 0x100000, "NetServer": 0x200000, "HasOutParams": 0x400000, "HasDefaults": 0x800000,
             "NetClient": 0x1000000, "DLLImport": 0x2000000, "BlueprintCallable": 0x4000000,
             "BlueprintEvent": 0x8000000, "BlueprintPure": 0x10000000, "EditorOnly": 0x20000000,
             "Const": 0x40000000, "NetValidate": 0x80000000}


# BlueprintPure in the dump but not a function of its arguments: two calls must stay two calls, and a discarded one
# may still move a random stream or build an object.
IMPURE_PURE = re.compile(r"Random|Now$|Today$|Create|Construct|Spawn|^New|^Make.*Object|Seed")
PURE = set()     # (class, function) of every BlueprintPure function, filled by write_events


def write_events(sdk_dir, out_dir):
    """Events.json: "Package.Class.Function" -> EFunctionFlags of every BlueprintEvent, the functions a Blueprint
    overrides or implements. The compiler copies part of these onto the override (KismetCompiler.cpp).
    Also collects PURE."""
    rows = []
    for name in sorted(f for f in os.listdir(sdk_dir) if f.endswith("_functions.cpp")):
        text = io.open(os.path.join(sdk_dir, name), encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"^// Function ([\w\.]+)\n// \(([^)]*)\)", text, re.M):
            names = m.group(2).split(", ")
            if "BlueprintPure" in names:
                PURE.add(tuple(m.group(1).split(".")[-2:]))
            if "BlueprintEvent" in names:
                rows.append('  "%s": %d' % (m.group(1), sum(FUNC_BITS[n] for n in names)))
    io.open(os.path.join(out_dir, "Events.json"), "w", encoding="utf-8", newline="\n").write("{\n" + ",\n".join(rows) + "\n}\n")
    print("  events: %d" % len(rows))


def write_containers(classes, sdk_dir, out_dir):
    """Containers.h: one UE_CONTAINER_<Template> macro of method declarations per library.
    A method is const when its function is BlueprintPure."""
    # ponytail: Keys / Values / ToArray / the set operations / RandomFromStream leave the container alone
    # but are BlueprintCallable in 4.27, so a const container cannot call them. A name list if that bites.
    text = io.open(os.path.join(sdk_dir, "Engine_functions.cpp"), encoding="utf-8", errors="replace").read()
    flags = dict(((m.group(1), m.group(2)), m.group(3)) for m in FUNC_FLAGS.finditer(text))
    macros, structs, count = [], set(), 0
    for k in classes:
        if k.is_bp or k.ue_name not in CONTAINER_LIBS:
            continue
        prefix, tpl = CONTAINER_LIBS[k.ue_name]
        members = []
        for raw_ret, fname, raw_params in k.raw_funcs:
            if not fname.startswith(prefix):
                continue
            params = [re.match(r"^(.*?)([A-Za-z_]\w*)$", p.strip()).groups() for p in split_params(raw_params)]
            if not params or tpl not in params[0][0]:
                continue
            ret = map_type(raw_ret)
            args = [(wildcard_param(t, n, tpl) or map_type(t), n) for t, n in params[1:]]
            if ret in KINDS or any(t in KINDS or t == "void" for t, _ in args):
                print("  container method held back: %s" % fname)
                continue
            structs.update(w for t, _ in args for w in re.findall(r"\bF\w+", t) if w in STRUCTS)
            members.append("    %s %s(%s)%s;" % (ret, fname[len(prefix):], ", ".join("%s %s" % a for a in args),
                                                 " const" if "BlueprintPure" in flags.get((k.ue_name, fname), "") else ""))
        count += len(members)
        macros.append("#define UE_CONTAINER_%s \\\n%s\n" % (tpl, " \\\n".join(members)))
    out = ["#pragma once",
           "/* Kismet's Array_ / Set_ / Map_ functions as methods of TArray / TSet / TMap, generated by",
           "   AssetGen/tools/genueapi.py. Do not edit. */"]
    out += ["struct %s;" % s for s in sorted(structs)] + [""] + macros
    io.open(os.path.join(out_dir, "Containers.h"), "w", encoding="utf-8-sig", newline="\n").write("\n".join(out))
    print("  container methods: %d" % count)


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
    write_containers(ordered, sdk_dir, out_dir)
    write_events(sdk_dir, out_dir)
    ops_by_pkg = write_operators(ordered, out_dir)
    write_types(out_dir)

    def rewrite(ctype):
        def one(m):
            target = by_name.get(m.group(1))
            return "class %s" % target.emit if target else m.group(0)
        return CLASS_WORD.sub(one, ctype)

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
                referenced.update(class_refs(ftype))
            for is_static, ret, fname, params in k.funcs:
                if is_container_method(k, fname):
                    continue
                # A Blueprint hides the world context pin and wires it to self; the overload without
                # it is how a mod does the same, and AssetGen fills the argument back in.
                variants = [params]
                wco = [p for p in params if p[0] == "class UObject*" and p[1].startswith("WorldContext")]
                if wco:
                    variants.append([p for p in params if p is not wco[0]])
                # A latent function's FLatentActionInfo is the compiler's to fill in (the ubergraph resume point),
                # so the overloads a mod calls leave it out; the full one stays as the UFunction's signature.
                variants = [(ret, v) for v in variants]
                latent = ("struct FLatentActionInfo", "FLatentActionInfo")
                if any(t in latent for t, _ in params):
                    variants += [(ret, [p for p in v if p[0] not in latent]) for _, v in variants]
                    # `auto Cls = LoadAssetClass(Soft)`: a latent function's one-parameter completion delegate is
                    # the compiler's too, and its parameter is what the call returns once it resumes.
                    dele = [p for p in params if p[0].startswith("TDelegate<void(") and p[0].endswith(")>")]
                    inner = dele[0][0][len("TDelegate<void("):-2] if len(dele) == 1 and ret == "void" else ""
                    if inner and "," not in inner and " " in inner:
                        variants += [(inner.rsplit(" ", 1)[0], [p for p in v if p is not dele[0]])
                                     for r, v in variants if r == "void" and not any(p[0] in latent for p in v)]
                # UE_PURE lets AssetGen drop a discarded call and reuse a repeated one; a function that answers through
                # a reference parameter is left unmarked.
                pure = ((k.ue_name, fname) in PURE and not IMPURE_PURE.search(fname)
                        and not any(t.endswith("&") and not t.startswith("const ") for t, _ in params))
                for vret, plist in variants:
                    args = ", ".join("%s %s" % (rewrite(t), n) for t, n in plist)
                    body.append("    %s%s%s %s(%s)%s;" % ("UE_PURE " if pure else "", "static " if is_static else "",
                                                           rewrite(vret), fname, args,
                                                           " const" if fname in k.const_funcs else ""))
                funcs += 1
                for t in [ret] + [t for t, _ in params]:
                    referenced.update(class_refs(t))
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
