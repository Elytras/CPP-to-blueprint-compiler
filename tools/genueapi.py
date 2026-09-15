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

Blueprint packages are skipped too, for now: a dump spells every name through FName::ToString,
which strips the package path, so a Blueprint arrives as `Foo_C` with no way back to
/Game/Where/Foo - and an import needs exactly that. Dumper-7 grew a `FullAssetPaths=1` setting
that spells both the object dumps and the SDK class comment with GetPathName instead; admitting
Blueprint classes here waits on a dump taken with it.

That skip is also what keeps the flat C++ namespace here honest. Asset names are unique per
/Game path, not globally - DRG's modding API guarantees collisions, since every mod ships its
own InitCave and InitSpacerig - and Dumper-7's NameCollisions.inl duly lists InitCave_0,
InitCave_3, InitCave_19. Measured against this dump, no *native* class collides (the only 7
collisions are /Game structs), so emitting native packages flat is safe today.

When Blueprint classes are admitted, the shape they take is settled:

  - The disambiguator is the /Game path mirrored into a C++ namespace, NEVER Dumper-7's index
    suffix: that index is assigned per dump, so a game update could renumber it and silently
    re-point a mod at a different asset. /Game/Enemies/BP_Foo -> namespace Game::Enemies.
  - One header per ASSET NAME, not per path, so an include is derivable from the class name -
    which is what an author knows. Names that collide share a header and are separated by their
    namespaces inside it. Measured against DRG's own pak: 51,556 assets, 51,449 of those names
    unique, 107 colliding and every one of them exactly twice.
  - Each header ends with `using Foo_C = Game::Enemies::Foo_C;` for every name that is unique
    across the whole emission, so a mod writes the bare name in the overwhelming majority of
    cases and only a genuine collision forces the qualified spelling. The point of leaving the
    colliding ones unaliased is that referring to one then fails to COMPILE rather than
    resolving silently to whichever was emitted last.
  - Only base-game assets are emitted. Another mod's InitCave is not API, and including it would
    make the headers depend on which mods happened to be installed when the dump was taken. The
    filter is the game's own pak index: a /Game package listed there is base game, anything else
    arrived from a mod.

Note for whoever does it: AssetGen resolves a base class by the bare name clang reports in
`bases[0].type.qualType` against a map keyed on the bare CXXRecordDecl name, so a namespaced
base ("Game::Enemies::ABar_C") misses and reports "derives from an undeclared class". Same gap
in its object-pointer parse, which strips `class ` and `const ` but not a qualifier. Both need
to handle qualified names before these headers exist.

usage: genueapi.py <SDK dir> <output dir>      e.g. DrgMods/SDK/SDK  BpMods/UeApi
"""
import io
import os
import re
import sys

DECL = re.compile(r"^\t(static\s+)?([A-Za-z_][\w:<>,\s\*&]*?)\s+(\w+)\((.*)\);\s*$")
# The root (UObject) has no base; everything else derives from something. Either may carry an
# alignment prefix, spelled alignas(...) or SDK_ALIGN(...) - and a class line that fails to match
# here is not skipped, it silently donates its whole body to the class above it.
CLASS = re.compile(r"^class\s+(?:\w+\([^)]*\)\s+)?(\w+)(?:\s+final)?(?:\s*:\s*public\s+(\w+))?\s*$")
CLASS_COMMENT = re.compile(r"^// Class\s+([\w/\.\-]+)\.(\w+)\s*$")
# Dumper-7 labels a cooked Blueprint class by its generated-class kind, never as "Class".
BP_COMMENT = re.compile(r"^// \w*BlueprintGeneratedClass\s")
PTR = re.compile(r"^(?:const\s+)?class\s+(\w+)\s*\*$")
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
    def __init__(self, cpp, base, package, ue_name):
        self.cpp, self.base, self.package, self.ue_name = cpp, base, package, ue_name
        self.funcs = []
        self.fields = []


def parse_header(path):
    """Every class in one <Package>_classes.hpp, with the functions that survive the filter."""
    text = io.open(path, encoding="utf-8", errors="replace").read()
    classes, cur, pending = [], None, None
    skipped = dict((k, 0) for k in KINDS)
    is_blueprint = False
    includes = []
    for line in text.splitlines():
        if BP_COMMENT.match(line):
            is_blueprint = True
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
                cur = Klass(m.group(1), m.group(2) or "", pending[0], pending[1])
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
    return classes, skipped, is_blueprint, includes


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip().splitlines()[-1])
    sdk_dir, out_dir = sys.argv[1], sys.argv[2]

    headers = sorted(f for f in os.listdir(sdk_dir) if f.endswith("_classes.hpp"))
    classes, totals, skipped_bp = [], dict((k, 0) for k in KINDS), 0
    for name in headers:
        found, skipped, is_blueprint, _ = parse_header(os.path.join(sdk_dir, name))
        # Dumper-7 names a Blueprint package after its asset and does not record the /Game path
        # an import needs, so its classes are unaddressable however well they parse.
        if is_blueprint or not found:
            skipped_bp += 1 if is_blueprint else 0
            continue
        pkg = name[: -len("_classes.hpp")]
        for k in found:
            k.package = pkg
        classes.extend(found)
        for k in KINDS:
            totals[k] += skipped[k]

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
        by_pkg.setdefault(k.package, []).append(k)

    deps = {}
    for pkg, members in by_pkg.items():
        deps[pkg] = set(by_name[k.base].package for k in members
                        if k.base and by_name[k.base].package != pkg)

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

    funcs, fields = 0, 0
    for pkg, members in sorted(by_pkg.items()):
        body, referenced = [], set()
        defined = set(k.cpp for k in members)
        for k in members:
            inherits = " : public %s" % k.base if k.base else ""
            body.append("class %s%s\n{\npublic:\n    UE_CLASS(\"/Script/%s\", \"%s\");"
                        % (k.cpp, inherits, pkg, k.ue_name))
            names = set(f for _, _, f, _ in k.funcs)
            for ftype, fname in k.fields:
                if fname in names:
                    continue                # a property and a function of one name cannot coexist
                body.append("    %s %s;" % (ftype, fname))
                fields += 1
                m = PTR.match(ftype)
                if m:
                    referenced.add(m.group(1))
            for is_static, ret, fname, params in k.funcs:
                args = ", ".join("%s %s" % (t, n) for t, n in params)
                body.append("    %s%s %s(%s);" % ("static " if is_static else "", ret, fname, args))
                funcs += 1
                for t in [ret] + [t for t, _ in params]:
                    m = PTR.match(t)
                    if m:
                        referenced.add(m.group(1))
            body.append("};\n")

        out = ["#pragma once",
               "/*",
               "Package /Script/%s, generated by AssetGen/tools/genueapi.py. Do not edit." % pkg,
               "",
               "A member is here if and only if AssetGen can compile a use of it.",
               "*/",
               "#include \"UeMeta.h\""]
        # A base class must be complete, so its package is included; a pointer only needs a name.
        out += ["#include \"%s.h\"" % d for d in sorted(deps[pkg])]
        out += [""]
        fwd = sorted(c for c in referenced if c not in defined)
        if fwd:
            out += ["class %s;" % c for c in fwd] + [""]
        out += body
        io.open(os.path.join(out_dir, pkg + ".h"), "w", encoding="utf-8-sig", newline="\n").write("\n".join(out))

    reach = ", ".join("%s %d" % (k, totals[k]) for k in KINDS if totals[k])
    umbrella = ["#pragma once",
                "/*",
                "UeApi.h - every UE class a Blueprint mod can address.",
                "",
                "Generated by AssetGen/tools/genueapi.py. Do not edit.",
                "",
                "A member is here if and only if AssetGen can compile a use of it, so clang",
                "accepting a mod source is the same statement as AssetGen being able to generate it.",
                "%d classes, %d functions, %d properties across %d packages."
                % (len(ordered), funcs, fields, len(by_pkg)),
                "",
                "Including this pulls in all of them, which is convenient and slow: it costs clang a",
                "56 MB AST dump for a twenty-line mod. Prefer naming the two or three packages the",
                "mod actually uses.",
                "",
                "TODO: unimplemented value kinds held %d members back - %s." % (sum(totals.values()), reach),
                "*/"]
    umbrella += ["#include \"%s.h\"" % pkg for pkg in sorted(by_pkg)]
    io.open(os.path.join(out_dir, "UeApi.h"), "w", encoding="utf-8-sig", newline="\n").write("\n".join(umbrella) + "\n")

    print("UeApi: %d classes, %d functions, %d properties, %d packages"
          % (len(ordered), funcs, fields, len(by_pkg)))
    print("  out of reach: " + reach)
    print("  blueprint packages skipped (no /Game path in the dump): %d" % skipped_bp)


main()
