#!/usr/bin/env python3
"""usage: genueassets.py <AssetRegistry.bin> <UeApi dir> <UeAssets dir>

Writes UeAssets/<Class>.h for each class the game has assets of: every asset named by its content path,

    namespace UeAssets::UMaterialInstanceConstant::Game::Landscape::Biomes::Biomes_Ingame::AzureWeald::Assets {
    UE_ASSET_AT(::UMaterialInstanceConstant, M_Biome_AzureWeald_Rock01, "/Game/Landscape/.../M_Biome_AzureWeald_Rock01");
    }

so a mod writes `&UeAssets::UMaterialInstanceConstant::Game::...::M_Biome_AzureWeald_Rock01`, and
`UeAssets::UMaterialInstanceConstant::All` is every one of them as a soft pointer (UE_ASSET_ALL). The registry is the
game pak's FSD/AssetRegistry.bin (UnrealPak <pak> -Extract <dir> -Filter=*AssetRegistry.bin). A class UeApi has no
header for, and Blueprint classes themselves (UeApi/Game names those), are left out.
"""
import collections
import keyword
import os
import re
import sys

import dumpar

CPP_KEYWORDS = set(keyword.kwlist) | {
    "alignas", "alignof", "asm", "auto", "bool", "case", "catch", "char", "class", "const", "constexpr", "consteval",
    "constinit", "const_cast", "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
    "dynamic_cast", "enum", "explicit", "export", "extern", "float", "friend", "goto", "inline", "int", "long",
    "mutable", "namespace", "new", "noexcept", "operator", "private", "protected", "public", "register",
    "reinterpret_cast", "requires", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
    "switch", "template", "this", "thread_local", "throw", "typedef", "typeid", "typename", "union", "unsigned",
    "using", "virtual", "void", "volatile", "wchar_t", "while", "char8_t", "char16_t", "char32_t", "nullptr", "true",
    "false", "concept", "compl", "bitand", "bitor", "xor", "not", "and", "or", "xor_eq", "and_eq", "or_eq", "not_eq",
    # C11's, which clang takes in C++ too: /Game/Audio/SFX/Characters/Footsteps/_Generic is a folder
    "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert",
    "_Thread_local"}
SKIP_CLASSES = {"ObjectRedirector", "Blueprint", "WidgetBlueprint", "AnimBlueprint"}    # the last three are cooked out


def ident(s):
    s = re.sub(r"\W", "_", s, flags=re.ASCII)
    if s[:1].isdigit():
        s = "_" + s
    return s + "_" if s in CPP_KEYWORDS or s.startswith("__") else s    # __ names are the compiler's


def class_index(ueapi):
    """UE class name -> [(C++ name with its namespace, header)], from each header's UE_CLASS markers."""
    index = collections.defaultdict(list)
    for folder, _dirs, files in os.walk(ueapi):
        for f in files:
            if not f.endswith(".h"):
                continue
            path = os.path.join(folder, f)
            ns, cls = "", None
            for line in open(path, encoding="utf-8-sig"):
                if line.startswith("namespace "):
                    ns = "".join(n + "::" for n in re.findall(r"namespace (\w+) \{", line))
                m = re.match(r"class (\w+)\b[^;]*$", line)
                if m:
                    cls = m.group(1)
                m = re.search(r'UE_CLASS\("[^"]*", "(\w+)"\)', line)
                if m and cls:
                    index[m.group(1)].append((ns + cls, path))
    return index


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.strip().splitlines()[0])
    registry, ueapi, out = sys.argv[1:]
    _version, _names, rows, *_ = dumpar.read(registry)
    index = class_index(ueapi)

    by_class, skipped = collections.defaultdict(list), collections.Counter()
    for r in rows:
        cls = r["asset_class"]
        if cls.endswith("GeneratedClass") or cls in SKIP_CLASSES:
            skipped["a class, redirector or Blueprint"] += 1
        elif len(index.get(cls, ())) != 1:
            skipped["class not in UeApi" if cls not in index else "class name in two packages"] += 1
        else:
            by_class[index[cls][0]].append(r)

    os.makedirs(out, exist_ok=True)
    for (cpp, header), assets in sorted(by_class.items()):
        bare = cpp.rsplit("::", 1)[-1]
        folders = collections.defaultdict(list)      # namespace path -> [(variable, asset path)]
        for r in assets:
            *dirs, leaf = r["package_name"].strip("/").split("/")
            path = r["package_name"] + ("" if leaf == r["asset_name"] else "." + r["asset_name"])
            folders[tuple(ident(d) for d in dirs)].append((ident(r["asset_name"]), path))
        children = collections.defaultdict(set)      # a folder's subfolders: a variable may not take their names
        for dirs in folders:
            for i in range(len(dirs)):
                children[dirs[:i]].add(dirs[i])

        lines, example = [], None
        for dirs in sorted(folders):
            taken = set(children[dirs])
            lines.append("namespace UeAssets::%s::%s {" % (bare, "::".join(dirs)))
            for var, path in sorted(folders[dirs], key=lambda a: a[1].lower()):
                while var in taken:
                    var += "_"
                taken.add(var)
                lines.append('UE_ASSET_AT(::%s, %s, "%s");' % (cpp, var, path))
                if not example or example[0].split("::")[1] != "Game":
                    example = ("::".join((bare,) + dirs + (var,)), path)
            lines.append("}")
        rel = lambda p: (os.path.relpath(p, out) if os.path.splitdrive(os.path.abspath(p))[0] ==
                         os.path.splitdrive(os.path.abspath(out))[0] else os.path.abspath(p)).replace(os.sep, "/")
        text = ("#pragma once\n/*\nEvery %s asset in the game, %d of them, generated by AssetGen/tools/genueassets.py from"
                " the game's\nAssetRegistry.bin. Do not edit. Each is named by its content path:\n"
                "    &UeAssets::%s\npoints at %s.\nUeAssets::%s::All is every one of them, as soft pointers.\n*/\n"
                "#include \"%s\"\n#include \"%s\"\n\nnamespace UeAssets::%s {\nUE_ASSET_ALL(::%s);\n}\n%s\n"
                % (bare, len(assets), example[0], example[1], bare, rel(os.path.join(ueapi, "UeMeta.h")), rel(header),
                   bare, cpp, "\n".join(lines)))
        with open(os.path.join(out, bare + ".h"), "w", encoding="utf-8-sig", newline="\n") as f:
            f.write(text)
    print("%d asset(s) in %d header(s); left out: %s" % (sum(len(a) for a in by_class.values()), len(by_class),
                                                         ", ".join("%d %s" % (n, k) for k, n in skipped.items())))


if __name__ == "__main__":
    main()
