#!/usr/bin/env python3
"""Build the Blueprint mods listed in BpMods/mods.yaml, skipping the ones nothing changed under.

The unit of staleness is a mod, not a file: a mod source compiles to several assets at once
(a class plus its spawn hooks), so there is no file-to-file mapping to diff. A mod rebuilds when
any of its sources, the UeApi headers, or assetgen.exe itself is newer than the oldest asset it
last produced - the oldest, because a half-written previous run must not look up to date.

Packing follows the same rule one level up: a pak is rebuilt when its mod recompiled or when the
pak is older than the staged assets, so a run that changes nothing costs one stat per file.

usage: bpbuild.py <repo root> <UeApi dir> <assetgen.exe> [--force] [--no-pak]
"""
import io
import os
import re
import subprocess
import sys

import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dumpexp import load as load_package

MOD_PACKAGE = re.compile(r'UE_MOD_PACKAGE\s*\(\s*"([^"]+)"')

UNREALPAK = r"C:\Program Files\Epic Games\UE_4.27\Engine\Binaries\Win64\UnrealPak.exe"


def newest(paths):
    """Newest mtime among existing paths, or 0."""
    times = [os.path.getmtime(p) for p in paths if os.path.exists(p)]
    return max(times) if times else 0


def oldest(paths):
    times = [os.path.getmtime(p) for p in paths if os.path.exists(p)]
    return min(times) if times else 0


def mod_package(source):
    """The /Game path the mod writes into, read from the source rather than repeated in yaml."""
    text = io.open(source, encoding="utf-8-sig", errors="replace").read()
    m = MOD_PACKAGE.search(text)
    if not m:
        sys.exit("%s declares no UE_MOD_PACKAGE" % source)
    return m.group(1)


def order(mods):
    """Declaration order, except that a mod listed in someone's `needs` is built first."""
    by_name = dict((m["name"], m) for m in mods)
    out, state = [], {}

    def place(name, trail):
        if state.get(name) == 2:
            return
        if state.get(name) == 1:
            sys.exit("cycle in mods.yaml `needs`: %s" % " -> ".join(trail + [name]))
        if name not in by_name:
            sys.exit("mods.yaml: `needs` names an unknown mod: %s" % name)
        state[name] = 1
        for dep in by_name[name].get("needs") or []:
            place(dep, trail + [name])
        state[name] = 2
        out.append(by_name[name])

    for m in mods:
        place(m["name"], [])
    return out


def staged_assets(stage_content):
    if not os.path.isdir(stage_content):
        return []
    return [os.path.join(stage_content, f) for f in os.listdir(stage_content)
            if f.endswith(".uasset") or f.endswith(".uexp")]


def run_unrealpak(fsd_dir, pak_path):
    """UnrealPak's own response-file form: stage the FSD tree, mount it back at ..\\..\\..\\FSD."""
    if not os.path.exists(UNREALPAK):
        print("  UnrealPak not found at %s - skipping the pak." % UNREALPAK)
        return False

    response = os.path.join(os.path.dirname(fsd_dir), "autogen.txt")
    io.open(response, "w", encoding="utf-8", newline="\n").write(
        '"%s\\*.*" "..\\..\\..\\FSD\\*.*"\n' % fsd_dir)

    cmd = [UNREALPAK, pak_path, "-platform=Windows", "-create=" + response, "-compress"]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        print("  UnrealPak failed (%d):" % proc.returncode)
        print("    " + "\n    ".join(proc.stdout.strip().splitlines()[-6:]))
        return False
    return True


def check_mod_imports(staged, mod_packages):
    """Report imports of a mod package that no mod in this build actually produces.

    A generated asset lives at <mod package>/<class>, so `/Game/Mods/Lib/Lib`, while the folder
    it sits in - `/Game/Mods/Lib` - is not a package at all. Naming the folder in a UE_CLASS
    reads as correct whenever the mod and its class share a name, and nothing offline notices:
    the import table is written, the package loads, and every object under that import resolves
    to null. The first sign of it is a null UFunction at call time, one frame inside the VM.

    Only imports under a known mod package are judged. A mod is free to reference a real game
    asset, and there is no list of those to check against.
    """
    produced = set()
    for _mod, content, package in staged:
        for f in os.listdir(content):
            if f.endswith(".uasset"):
                produced.add(package + "/" + f[:-len(".uasset")])

    bad = []
    for _mod, content, _package in staged:
        for f in sorted(os.listdir(content)):
            if not f.endswith(".uasset"):
                continue
            base = os.path.join(content, f[:-len(".uasset")])
            imports = load_package(base)[4]
            for entry in imports:
                if not entry.startswith("Package'"):
                    continue
                name = entry[len("Package'"):-1]
                under = any(name == m or name.startswith(m + "/") for m in mod_packages)
                if under and name not in produced:
                    bad.append((f, name))
    for asset, name in bad:
        print("  %s imports %s, which no mod in this build produces" % (asset, name))
    return not bad


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__.strip().splitlines()[-1])
    # Absolute throughout: subprocess resolves a relative exe against PATH, not the cwd.
    repo, ue_api, assetgen = (os.path.abspath(p) for p in sys.argv[1:4])
    force = "--force" in sys.argv[4:]
    no_pak = "--no-pak" in sys.argv[4:]

    bp = os.path.join(repo, "BpMods")
    manifest = os.path.join(bp, "mods.yaml")
    if not os.path.exists(manifest):
        sys.exit("no manifest at %s" % manifest)

    config = yaml.safe_load(io.open(manifest, encoding="utf-8-sig").read()) or {}
    mods = config.get("mods") or []
    if not mods:
        print("mods.yaml lists no mods.")
        return 0

    # The whole API surface is one input: any header moving can change what a mod compiles to.
    api_headers = [os.path.join(ue_api, f) for f in os.listdir(ue_api)] if os.path.isdir(ue_api) else []
    toolchain_time = max(newest(api_headers), newest([assetgen]))

    built, packed, skipped, failed = [], [], [], []
    staged = []
    for mod in order(mods):
        name = mod["name"]
        sources = [os.path.join(bp, s) for s in (mod.get("sources") or [])]
        missing = [s for s in sources if not os.path.exists(s)]
        if not sources or missing:
            print("%-16s SKIP - no such source: %s" % (name, ", ".join(missing) or "(none listed)"))
            failed.append(name)
            continue

        package = mod_package(sources[0])
        stage_fsd = os.path.join(bp, "build", name, "FSD")
        stage_content = os.path.join(stage_fsd, "Content", *package.replace("/Game/", "").split("/"))
        assets = staged_assets(stage_content)
        staged.append((name, stage_content, package))

        stale = force or not assets or max(newest(sources), toolchain_time) > oldest(assets)
        if not stale:
            print("%-16s up to date" % name)
            skipped.append(name)
            continue

        if not os.path.isdir(stage_content):
            os.makedirs(stage_content)
        ok = True
        for source in sources:
            proc = subprocess.run([assetgen, "compile", source, ue_api, stage_content])
            if proc.returncode != 0:
                ok = False
                break
        if not ok:
            print("%-16s FAILED" % name)
            failed.append(name)
            continue

        print("%-16s compiled -> %s" % (name, package))
        built.append(name)

        if no_pak:
            continue
        pak = os.path.join(bp, "out", name + "_P.pak")
        if not os.path.isdir(os.path.dirname(pak)):
            os.makedirs(os.path.dirname(pak))
        if run_unrealpak(stage_fsd, pak):
            print("%-16s packed   -> %s" % (name, pak))
            packed.append(name)
        else:
            failed.append(name)

    if not failed and not check_mod_imports(staged, set(p for _n, _c, p in staged)):
        failed.append("cross-mod imports")

    print("built %d, packed %d, up to date %d, failed %d"
          % (len(built), len(packed), len(skipped), len(failed)))
    return 1 if failed else 0


sys.exit(main())
