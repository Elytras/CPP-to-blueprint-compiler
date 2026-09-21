#!/usr/bin/env python3
"""usage: bpbuild.py <repo root> <UeApi dir> <assetgen.exe> [--force] [--no-pak]"""
import io
import os
import re
import shutil
import subprocess
import sys

import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dumpexp import load as load_package

MOD_PACKAGE = re.compile(r'UE_MOD_PACKAGE\s*\(\s*"([^"]+)"')

UNREALPAK = r"C:\Program Files\Epic Games\UE_4.27\Engine\Binaries\Win64\UnrealPak.exe"


def newest(paths):
    times = [os.path.getmtime(p) for p in paths if os.path.exists(p)]
    return max(times) if times else 0


def oldest(paths):
    times = [os.path.getmtime(p) for p in paths if os.path.exists(p)]
    return min(times) if times else 0


def mod_package(source):
    text = io.open(source, encoding="utf-8-sig", errors="replace").read()
    m = MOD_PACKAGE.search(text)
    if not m:
        sys.exit("%s declares no UE_MOD_PACKAGE" % source)
    return m.group(1)


def order(mods):
    """Declaration order, except that a mod listed in someone's `needs` is built first.
    A `needs` cycle (two mods that embed each other) is not an error: compile is one pass and
    pack another, so the cyclic edge is just broken here and both still compile before either paks."""
    by_name = dict((m["name"], m) for m in mods)
    out, state = [], {}

    def place(name, trail):
        if state.get(name) == 2:
            return
        if state.get(name) == 1:
            return  # back-edge (cycle) - break it; order among the pair doesn't matter to compile
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


def transitive_needs(name, by_name, seen=None):
    """Every mod reachable through `needs`, deepest first, each once. `seen` starts holding the
    root so a `needs` cycle terminates and a mod never lists itself as its own embed dep."""
    seen = {name} if seen is None else seen
    out = []
    for dep in by_name[name].get("needs") or []:
        if dep in seen:
            continue
        seen.add(dep)
        out += transitive_needs(dep, by_name, seen) + [dep]
    return out


API_MANIFEST = ".assetgen"


def api_manifest(api_content):
    """The assets the last run generated into `api_content`, as absolute paths. Absent manifest
    (a folder from before this existed, or a hand-populated one) means claim nothing."""
    path = os.path.join(api_content, API_MANIFEST)
    if not os.path.exists(path):
        return []
    names = io.open(path, encoding="utf-8").read().split()
    return [os.path.join(api_content, n) for n in names]


def write_api_manifest(api_content):
    io.open(os.path.join(api_content, API_MANIFEST), "w", encoding="utf-8", newline="\n").write(
        "".join(os.path.basename(f) + "\n" for f in sorted(staged_assets(api_content))))


API_UE_VERSION = "4.27"  # AssetGen writes UE4.27 packages; a UE5 project needs its own writer.


def api_roots(mod, config, bp):
    """Where a `generate_api` mod's editor-side stubs go: its own `api_dir`, else the manifest's
    top-level `api_dir` (the default for every mod), else the build tree. Each entry names an
    editor project's Content folder (relative ones resolve against BpMods/), so a build updates
    those projects' API assets in place instead of leaving them to be copied by hand.

    An entry is a path, or a `{path: ..., ue: <version>}` mapping - several projects may want the
    same library, and they need not run the same engine. Only 4.27 has a writer today, so a
    version this build cannot produce is an error rather than a wrong package."""
    spec = mod.get("api_dir") or config.get("api_dir")
    if not spec:
        return [os.path.join(bp, "out", "api", "Content")]

    out = []
    for entry in (spec if isinstance(spec, list) else [spec]):
        path, ue = (entry, API_UE_VERSION) if not isinstance(entry, dict) else \
                   (entry.get("path"), str(entry.get("ue", API_UE_VERSION)))
        if not path:
            sys.exit("mods.yaml: an `api_dir` entry has no `path`")
        if ue != API_UE_VERSION:
            sys.exit("mods.yaml: api_dir %s wants UE %s; assetgen writes %s only"
                     % (path, ue, API_UE_VERSION))
        out.append(os.path.abspath(os.path.join(bp, os.path.expandvars(path))))
    return out


def content_dir(stage_fsd, package):
    return os.path.join(stage_fsd, "Content", *package.replace("/Game/", "").split("/"))


def dep_stage(dep, by_name, bp):
    """Where dep's cooked assets sit - the import path (its UE_MOD_PACKAGE) plus its build dir,
    both known from the manifest, so nothing needs threading through the build loop."""
    package = mod_package(os.path.join(bp, by_name[dep]["sources"][0]))
    return content_dir(os.path.join(bp, "build", dep, "FSD"), package), package


def embed_deps(dep_stages, stage_fsd):
    """Copy each dep's staged assets into this mod's FSD tree so the pak is self-contained.
    Clears each dest first so a renamed/dropped dep asset can't linger and ship stale."""
    for content, package in dep_stages:
        dest = content_dir(stage_fsd, package)
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        os.makedirs(dest)
        for f in staged_assets(content):
            shutil.copy2(f, os.path.join(dest, os.path.basename(f)))


def staged_assets(stage_content):
    if not os.path.isdir(stage_content):
        return []
    return [os.path.join(stage_content, f) for f in os.listdir(stage_content)
            if f.endswith(".uasset") or f.endswith(".uexp")]


def run_unrealpak(fsd_dir, pak_path):
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
    """A generated asset is <mod package>/<class>; a UE_CLASS naming the folder instead writes an
    import that loads fine and resolves to null, first seen as a null UFunction inside the VM."""
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


VS_FILTERS = (("Mods", "5b1f0c4e-6a0d-4a57-9d0a-1c1f6e2a7b01"), ("Tests", "5b1f0c4e-6a0d-4a57-9d0a-1c1f6e2a7b02"),
              ("Helpers", "5b1f0c4e-6a0d-4a57-9d0a-1c1f6e2a7b03"), ("UeApi", "5b1f0c4e-6a0d-4a57-9d0a-1c1f6e2a7b04"))


def write_vs_filters(bp):
    """BpMods.vcxproj lists its sources by wildcard, so a new mod needs no project edit - but Visual Studio gives a
    wildcard FILTER entry to the first file it matches only. So the folders of the project tree are written out file
    by file here, on every build, and only when the list changed (a rewrite makes Visual Studio reload the project)."""
    if not os.path.exists(os.path.join(bp, "BpMods.vcxproj")):
        return
    rows = []
    for f in sorted(os.listdir(bp), key=str.lower):
        if f.endswith(".cpp"):
            rows.append(("ClCompile", f, "Tests" if f.endswith("Test.cpp") else "Mods"))
        elif f.endswith(".h"):
            rows.append(("ClInclude", f, "Helpers"))
    api = os.path.join(bp, "UeApi")
    for f in sorted(os.listdir(api), key=str.lower) if os.path.isdir(api) else []:
        if f.endswith(".h"):
            rows.append(("ClInclude", "UeApi\\" + f, "UeApi"))
    text = ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<!-- Written by AssetGen/tools/bpbuild.py on every build. Do not edit. -->\n'
            '<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">\n  <ItemGroup>\n'
            + "".join('    <Filter Include="%s"><UniqueIdentifier>{%s}</UniqueIdentifier></Filter>\n' % f for f in VS_FILTERS)
            + '  </ItemGroup>\n  <ItemGroup>\n'
            + "".join('    <%s Include="%s"><Filter>%s</Filter></%s>\n' % (kind, name, folder, kind) for kind, name, folder in rows)
            + '    <None Include="mods.yaml" />\n  </ItemGroup>\n</Project>\n')
    dest = os.path.join(bp, "BpMods.vcxproj.filters")
    if not os.path.exists(dest) or io.open(dest, encoding="utf-8").read() != text:
        io.open(dest, "w", encoding="utf-8", newline="\n").write(text)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__.strip().splitlines()[-1])
    # Absolute: subprocess resolves a relative exe against PATH, not the cwd.
    repo, ue_api, assetgen = (os.path.abspath(p) for p in sys.argv[1:4])
    force = "--force" in sys.argv[4:]
    no_pak = "--no-pak" in sys.argv[4:]

    bp = os.path.join(repo, "BpMods")
    write_vs_filters(bp)
    manifest = os.path.join(bp, "mods.yaml")
    if not os.path.exists(manifest):
        sys.exit("no manifest at %s" % manifest)

    config = yaml.safe_load(io.open(manifest, encoding="utf-8-sig").read()) or {}
    mods = config.get("mods") or []
    if not mods:
        print("mods.yaml lists no mods.")
        return 0

    api_headers = [os.path.join(ue_api, f) for f in os.listdir(ue_api)] if os.path.isdir(ue_api) else []
    toolchain_time = max(newest(api_headers), newest([assetgen]))

    by_name = dict((m["name"], m) for m in mods)

    built, packed, skipped, failed = [], [], [], []
    staged = []
    records = []
    # Phase 1 - compile every mod. `embed` bakes a dep's cooked assets into the dependent's pak, so
    # all assets must exist before any pak; with mutual embed no build order puts both deps first,
    # so compiling is a pass of its own, separate from packing (phase 2).
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

        # `generate_api` writes the editor-side stub next to nothing else, so it has its own
        # staleness: turning the flag on for an already-built mod must still produce one.
        # assetgen writes one `--api` dir; further ones are mirrors of it, filled after the compile.
        api_contents = ([os.path.join(root, *package.replace("/Game/", "").split("/"))
                         for root in api_roots(mod, config, bp)]
                        if mod.get("generate_api") else [])
        api_content = api_contents[0] if api_contents else None
        api_missing = any(not staged_assets(d) for d in api_contents)

        stale = (force or not assets or api_missing
                 or max(newest(sources), toolchain_time) > oldest(assets))
        if stale:
            if not os.path.isdir(stage_content):
                os.makedirs(stage_content)
            # An asset the sources no longer cook (a struct another mod now owns) must not stay in the pak.
            for old in assets:
                os.remove(old)
            for api_dir in api_contents:
                if not os.path.isdir(api_dir):
                    os.makedirs(api_dir)
                else:
                    # A class the sources no longer expose to the API (now inline, actor-gated,
                    # or renamed) must not leave a stale stub the editor then loads and chokes on.
                    # Only ours go: `api_dir` usually points into a real project, where the same
                    # folder holds hand-made assets that must survive. The manifest is rewritten
                    # right after the compile, below.
                    for old in api_manifest(api_dir):
                        if os.path.exists(old):
                            os.remove(old)
            ok = True
            for source in sources:
                if not source.endswith(".cpp"):
                    continue
                cmd = [assetgen, "compile", source, ue_api, stage_content]
                if api_content:
                    cmd += ["--api", api_content]
                proc = subprocess.run(cmd)
                if proc.returncode != 0:
                    ok = False
                    break
            if not ok:
                print("%-16s FAILED" % name)
                failed.append(name)
                continue
            for api_dir in api_contents:
                if api_dir != api_content:
                    for f in staged_assets(api_content):
                        shutil.copy2(f, os.path.join(api_dir, os.path.basename(f)))
                write_api_manifest(api_dir)
            print("%-16s compiled -> %s" % (name, package))
            built.append(name)

        for api_dir in api_contents:
            # relpath throws across drives - an `api_dir` on another one is perfectly normal.
            shown = api_dir if os.path.splitdrive(api_dir)[0] != os.path.splitdrive(bp)[0] \
                else os.path.relpath(api_dir, bp)
            print("%-16s api      -> %s" % (name, shown))

        records.append((mod, name, package, stage_fsd, stage_content, stale))

    # Phase 2 - pack. Every mod's assets now exist, so an `embed` mod can bake in its deps.
    for mod, name, package, stage_fsd, stage_content, stale in records:
        embed = bool(mod.get("embed"))
        dep_stages = [dep_stage(d, by_name, bp) for d in transitive_needs(name, by_name)] if embed else []
        dep_assets = [f for content, _p in dep_stages for f in staged_assets(content)]
        assets = staged_assets(stage_content)
        pak = os.path.join(bp, "out", name + "_P.pak")
        # A dep's assets are baked into this pak, so a change to one restales it; likewise our own
        # assets after a --no-pak run. Judged on our sources alone the old pak would ship, and a mod
        # calling a function it lacks crashes on the null UFunction.
        unpacked = not no_pak and max(newest(assets), newest(dep_assets)) > newest([pak])
        if not stale and not unpacked:
            print("%-16s up to date" % name)
            skipped.append(name)
            continue
        if no_pak:
            continue
        if embed:
            embed_deps(dep_stages, stage_fsd)
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


if __name__ == "__main__":
    sys.exit(main())
