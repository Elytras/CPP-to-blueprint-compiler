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
}


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
    return classify(t)


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
    if "FText" in raw:
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


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip().splitlines()[-1])
    sdk_dir, out_dir = sys.argv[1], sys.argv[2]

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

    deps = {}
    for pkg, members in by_pkg.items():
        deps[pkg] = set(by_name[k.base].header for k in members
                        if k.base and by_name[k.base].header != pkg)

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

    def rewrite(ctype):
        m = PTR.match(ctype)
        target = by_name.get(m.group(1)) if m else None
        return "class %s*" % target.emit if target else ctype

    funcs, fields, aliased = 0, 0, 0
    for pkg, members in sorted(by_pkg.items()):
        body, referenced = [], set()
        defined = set(k.cpp for k in members)
        ns_open = None
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
