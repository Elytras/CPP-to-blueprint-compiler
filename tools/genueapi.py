#!/usr/bin/env python3
"""Emit the UeApi headers a Blueprint mod compiles against, from a Dumper-7 SDK dump.

The dump is already the right shape: it declares every reflected function as ordinary C++, so
this is a filter rather than a translation. What it filters on is the only rule that matters -
a member is emitted if and only if AssetGen can currently compile a use of it into Kismet
bytecode: a call for a function, a read or an assignment for a property. Anything involving a
struct, an enum, an FName or a container is left out, because emitting it would let a mod write
something that type-checks and then cannot be generated.

A property is spelled for the VM, not for memory. Nothing here is ever laid over a real object -
clang only ever parses these headers - so Dumper-7's `uint8 bHidden : 1` becomes a plain `bool`,
which is what the FBoolProperty behind it actually is and what picks EX_LetBool downstream.

Blueprint classes are emitted as well, which needs a dump taken with Dumper-7's
FullAssetPaths=1: without it every name goes through FName::ToString, a Blueprint arrives as
`Foo_C` with no way back to /Game/Where/Foo, and an import needs exactly that. A class whose
path did not survive is dropped and counted rather than emitted at a package that cannot exist.

Their names are not unique, so their spelling is not flat. Asset names are unique per /Game
path, not globally - DRG's modding API guarantees it, since every installed mod ships its own
InitCave and InitSpacerig, and this dump holds 32 of each. So:

  - A Blueprint is namespaced by its own /Game path (/Game/Where/Foo -> namespace Game::Where),
    never by Dumper-7's InitCave_0 / InitCave_19 suffixes: that index is assigned per dump, so a
    game update could renumber it and silently re-point a mod at a different asset.
  - One header per CLASS name under Game/, so an include follows from the type being written.
    Names that collide share a header and are told apart by their namespaces inside it.
  - Each header ends with `using Foo_C = Game::Where::Foo_C;` for every name that exactly one
    class claims - 7,108 of 7,246 here - so a mod writes the bare name unless it is naming one
    of the genuinely ambiguous few. Those are deliberately left unaliased: naming one bare then
    fails to COMPILE rather than resolving to whichever was emitted last.
  - Native classes keep their flat Dumper-7 names. None of them collide, and renaming them would
    churn every mod source for nothing.

Not filtered to base-game assets. Everything the dump had is emitted, so whichever mods were
installed when it was taken are in here too - another mod's InitCave is not API, and the headers
depending on that is a reproducibility wart. It is only a wart: the namespaces keep it from
being a correctness problem. The filter, if it is ever worth it, is the game's own pak index -
a /Game package listed there is base game, anything else arrived from a mod.

usage: genueapi.py <SDK dir> <output dir>      e.g. DrgMods/SDK/SDK  BpMods/UeApi
"""
import collections
import io
import os
import re
import sys

DECL = re.compile(r"^\t(static\s+)?([A-Za-z_][\w:<>,\s\*&]*?)\s+(\w+)\((.*)\);\s*$")
# The root (UObject) has no base; everything else derives from something. Either may carry an
# alignment prefix, spelled alignas(...) or SDK_ALIGN(...) - and a class line that fails to match
# here is not skipped, it silently donates its whole body to the class above it.
# Dumper-7 puts a colliding class in a namespace of its own (InitCave_0::AInitCave_C), so both
# the declared name and the base can be qualified. The qualified spelling is what identifies a
# class within one dump - it is the key everything here joins on.
CLASS = re.compile(r"^class\s+(?:\w+\([^)]*\)\s+)?((?:\w+::)?\w+)(?:\s+final)?"
                   r"(?:\s*:\s*public\s+((?:\w+::)?\w+))?\s*$")
# "// Class /Script/Engine.Actor", "// BlueprintGeneratedClass /Game/Where/Foo.Foo_C" - the kind
# distinguishes the two and the path is what a UE_CLASS needs. The path is only spelled in full
# when the dump was taken with Dumper-7's FullAssetPaths=1; without it a Blueprint arrives as
# "Foo.Foo_C" and is refused rather than emitted at a package that does not exist.
CLASS_COMMENT = re.compile(r"^// (\w+)\s+([\w/\.\-]+)\.(\w+)\s*$")
PTR = re.compile(r"^(?:const\s+)?class\s+((?:\w+::)?\w+)\s*\*$")
FIELD = re.compile(r"^\t([A-Za-z_][\w:<>,\*& ]*?)\s+([A-Za-z_]\w*)\s*(:\s*\d+)?;\s*//")
INCLUDE = re.compile(r'^#include\s+"(\w+)_classes\.hpp"')

SCALARS = {
    "void":   "void",
    "bool":   "bool",
    "float":  "float",
    "double": "double",
    # Signed integers. int32 collapses to int for now, so a regen against this dump keeps the
    # existing UeApi headers byte-identical; int64/int16/int8 emit their UE spellings, which
    # Types.h aliases so clang accepts them.
    "int":    "int",
    "int8":   "int8",
    "int16":  "int16",
    "int32":  "int",
    "int64":  "int64",
    # Unsigned integers. Bitfields upstream still resolve to bool through parse_field; these are
    # for plain byte/short/int/long members that Dumper-7 spells with a UE alias.
    "uint8":  "uint8",
    "uint16": "uint16",
    "uint32": "uint32",
    "uint64": "uint64",
}

"""
Why a type is out of reach. The blockers are not the same size, and not the ones they look like.

  enum       Only EX_ByteConst is missing from the assembler. Nothing else: bytecode stores an
             enum as its numeric value, so the enumerator's spelling never has to be written.
             Dumper-7 resolving real UserDefinedEnum names (no NewEnumeratorN survives in this
             dump) makes a mod source readable; it is not a dependency of generation.
  struct     The opposite situation, and the reason structs stay out even though EX_StructConst
             exists. A UserDefinedStruct's real property name carries a GUID suffix -
             CategoryName_2_<32 hex> - and that suffixed name is what the cooked asset stores
             and what a tagged property has to spell. Dumper-7 strips it, so the dump no longer
             carries it: the SDK says CategoryName. Emitting these would produce members that
             look right and bind to nothing. The way back is the pak: every UserDefinedStruct
             package lists its own suffixed field names in its name table, so one sweep over
             the shipped pak yields a complete field -> field_N_<guid> sidecar, with no
             Dumper-7 change and no per-asset lookup. Sweeping everything rather than resolving
             on demand is what keeps structs as automatic as classes: resolve per asset and a
             mod author would have to name each struct they want before they could use it.
             Worth doing when EX_StructConst is wired up, not before.
  name       EX_NameConst is not in the assembler yet.
  container  TArray/TMap/TSet literals have no constant opcode at all.
  other      delegates, templates, and anything else unrecognised.
"""
KINDS = ("enum", "struct", "name", "container", "other")


def classify(t):
    if t.startswith("struct ") or t.startswith("const struct "):
        return "struct"
    if t.startswith(("TArray<", "TMap<", "TSet<", "const TArray<", "const TMap<", "const TSet<")):
        return "container"
    if re.match(r"^(?:const\s+)?E[A-Z]\w*&?$", t):
        return "enum"
    return "other"


# UE text types the compile surface carries as real structs (in Types.h): the generator emits
# the aliased spelling rather than dropping the member or collapsing it to a C-string. Includes
# the const-reference forms Dumper-7 spells for a by-const-ref parameter.
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
    """The C++ spelling to emit, or a KINDS reason when AssetGen could not compile such a value."""
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
    """Top-level commas only; a template argument list is not a parameter boundary."""
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
    """[(type, name)] as C++ we can emit, or a KINDS reason if any parameter is out of reach."""
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
        return                              # Dumper-7's layout filler, not a reflected property
    if "FText" in raw:
        return                              # FText has no compile-time surface yet - dropped
    mapped = "bool" if bits else map_type(raw)
    if mapped in KINDS or mapped == "void":
        skipped[mapped if mapped in KINDS else "other"] += 1
        return
    cur.fields.append((mapped, fname))


class Klass(object):
    """One generated class: how the dump spells it, and how we will.

    `cpp` / `base` are Dumper-7's spellings, qualified where it had to disambiguate - they are
    join keys, never output. `path` is the UE package ("/Script/Engine", "/Game/Where/Foo") and
    `ue_name` the reflected class name; together they are the class's real identity and what
    UE_CLASS carries. `emit` and `header` are filled in once the whole set is known, since both
    depend on which names turn out to be unique.
    """

    def __init__(self, cpp, base, path, ue_name, is_bp):
        self.cpp, self.base, self.path, self.ue_name = cpp, base, path, ue_name
        self.is_bp = is_bp
        self.ns = ""            # C++ namespace mirroring `path`, empty for a native class
        self.emit = ""          # the name a mod writes, qualified when it has to be
        self.header = ""        # the header this class is emitted into, without ".h"
        self.funcs = []
        self.fields = []


def parse_header(path):
    """Every class in one <Package>_classes.hpp, with the functions that survive the filter."""
    text = io.open(path, encoding="utf-8", errors="replace").read()
    classes, cur, pending = [], None, None
    skipped = dict((k, 0) for k in KINDS)
    includes = []
    for line in text.splitlines():
        m = INCLUDE.match(line)
        if m:
            includes.append(m.group(1))     # Dumper-7's own include list is the dependency graph
        m = CLASS_COMMENT.match(line)
        if m:
            pending = m.groups()
            continue
        m = CLASS.match(line)
        if m:
            cur = None
            if pending:
                kind, path, ue_name = pending
                # Anim / Widget / Script BlueprintGeneratedClass are all cooked Blueprints.
                is_bp = kind.endswith("BlueprintGeneratedClass")
                if is_bp or kind == "Class":
                    cur = Klass(m.group(1), m.group(2) or "", path, ue_name, is_bp)
                    classes.append(cur)
            pending = None
            continue
        if cur is None:
            continue
        if "//" in line:                    # a property carries an offset comment; a declaration does not
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
            # A native class's package is the file it came from; only its path spelling varies
            # with FullAssetPaths, and the file is the same either way. A Blueprint has no such
            # fallback - without the path it cannot be named back to the engine, so it is dropped.
            if not k.is_bp:
                k.path = "/Script/" + name[: -len("_classes.hpp")]
        keep = [k for k in found if not k.is_bp or k.path.startswith("/")]
        pathless += len(found) - len(keep)
        classes.extend(keep)
        for k in KINDS:
            totals[k] += skipped[k]

    """
    How a class is named, and where it lands.

    A native class keeps the flat name Dumper-7 gave it: no two collide in this dump, and
    changing their spelling would churn every mod source for nothing. A Blueprint is namespaced
    by its own /Game path, which is the only disambiguator that survives a game update -
    Dumper-7's InitCave_0 / InitCave_19 suffixes are assigned per dump and would silently
    re-point a mod at a different asset. Every name that turns out to be unique across the whole
    emission also gets a `using` alias at global scope, so a mod writes the bare name unless it
    is naming one of the genuinely ambiguous few, where the qualified spelling is required and a
    bare one fails to compile rather than resolving to whichever was emitted last.
    """
    def namespace_of(path):
        parts = []
        for seg in path.strip("/").split("/")[:-1]:     # the last segment is the asset itself
            seg = re.sub(r"\W", "_", seg)
            parts.append("_" + seg if seg[:1].isdigit() else seg)
        return "::".join(parts)

    name_count = collections.Counter(k.ue_name for k in classes)
    for k in classes:
        k.ns = namespace_of(k.path) if k.is_bp else ""
        k.emit = (k.ns + "::" + k.ue_name) if k.ns else (k.ue_name if k.is_bp else k.cpp)
        k.header = ("Game/" + k.ue_name) if k.is_bp else k.path[len("/Script/"):]
    unique = set(n for n, c in name_count.items() if c == 1)

    """
    One header, in base-before-derived order.

    Mirroring Dumper-7's file-per-package layout looked tidier and bought nothing: it needs the
    include graph reproduced, and Dumper-7's within-file order is not topological anyway
    (USkeletalMeshComponent precedes the USkinnedMeshComponent it derives from). Sorting the
    whole set once removes the ordering problem, the include graph and the cross-package
    forward declarations together, and clang reads the result in about a second.
    """
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
                return False        # its base is unaddressable, so it cannot be derived from
        ordered.append(k)
        return True

    sys.setrecursionlimit(10000)
    for k in classes:
        place(k)

    """
    One header per package, in dependency order.

    A single giant header compiles fine (~130 ms) but gives an editor 4400 classes to index for
    one call, so a mod includes the two or three packages it actually names instead. (It does
    not shrink the AST dump much: reaching one FSD class drags in most of the engine either way.)

    Only a base class forces an include, and that is what makes the split possible. UE lets a
    class hold a pointer to its own subclass, so type references are freely cyclic - but a
    pointer needs no more than a forward declaration, so those never become include cycles.
    Inheritance is the acyclic part, and only per class: two packages can still form a cycle
    between them (a class in A deriving from one in B, and one in B from one in A) without any
    class being its own ancestor. So it is checked below rather than assumed, and aborts if it
    ever happens. It does not happen in this dump.
    """
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
        """A Dumper-7 type as we spell it. Only a class pointer can name something we renamed."""
        m = PTR.match(ctype)
        target = by_name.get(m.group(1)) if m else None
        return "class %s*" % target.emit if target else ctype

    funcs, fields, aliased = 0, 0, 0
    for pkg, members in sorted(by_pkg.items()):
        body, referenced = [], set()
        defined = set(k.cpp for k in members)
        ns_open = None
        for k in members:
            # Classes in one header can sit in different namespaces - that is the whole point of
            # the /Game split - so each run of same-namespace classes gets its own block.
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
                    continue                # a property and a function of one name cannot coexist
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

        # A name unique across the whole emission is reachable unqualified. An ambiguous one is
        # deliberately left out, so naming it bare fails to compile rather than resolving to
        # whichever of the several assets with that name happened to be emitted last.
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
        # A base class must be complete, so its header is included; a pointer only needs a name.
        out += ["#include \"%s.h\"" % d for d in sorted(deps[pkg])]
        out += [""]
        # A forward declaration belongs in the class's own namespace, so they are grouped by it.
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
