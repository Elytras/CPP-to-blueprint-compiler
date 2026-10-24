#!/usr/bin/env python3
"""usage: bpbuild.py <mods dir (holds mods.yaml) or a parent with BpMods/> <UeApi dir> <assetgen executable> [--force] [--no-pak]"""
import glob
import io
import json
import os
import re
import shutil
import subprocess
import sys
import uuid
import zipfile
from concurrent.futures import ThreadPoolExecutor

import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dumpexp import load as load_package
import mintcache
from mintcache import MINT_CACHE, MODIO_URL   # MINT's download cache: what a mod.io `game_content` names

MOD_PACKAGE = re.compile(r'UE_MOD_PACKAGE\s*\(\s*"([^"]+)"')

# Off Windows, a `C:/...` path in mods.yaml is one on that Windows drive as this machine mounts it (a dual boot's
# drives): BPBUILD_DRIVES="C=/mnt/c;D=/media/me/Data" names the mounts, else the first of /mnt/<x>, /mnt/win<x> that exists.
DRIVES = dict((d.strip().upper(), root.strip()) for d, _eq, root in
              (e.partition("=") for e in os.environ.get("BPBUILD_DRIVES", "").split(";") if "=" in e))
WINDOWS_PATH = re.compile(r"^([A-Za-z]):[\\/]")


def drive_path(path):
    """Off Windows, Windows drive path `path` on that drive's mount (DRIVES), or None when the drive is not mounted;
    any other path as it is."""
    m = WINDOWS_PATH.match(path) if os.name != "nt" else None
    if not m:
        return path
    drive = m.group(1).upper()
    root = DRIVES.get(drive) or next((r for r in ("/mnt/" + drive.lower(), "/mnt/win" + drive.lower())
                                      if os.path.isdir(r)), None)
    return os.path.join(root, *re.split(r"[\\/]+", path[3:])) if root else None


def manifest_path(bp, path):
    """A path mods.yaml gives, absolute: relative ones resolve against BpMods/, $VARS expand, and a Windows drive
    path is found on that drive's mount (drive_path)."""
    path = os.path.expandvars(path)
    host = drive_path(path)
    if host is None:
        drive = path[0].upper()
        sys.exit("mods.yaml: %s is on drive %s:, which is not mounted here (set BPBUILD_DRIVES=\"%s=<its mount>\")"
                 % (path, drive, drive))
    return os.path.abspath(os.path.join(bp, host))


# UnrealPak: UNREALPAK, else UE 4.27's default install. Off Windows, a Linux UnrealPak on PATH, else that default
# install's UnrealPak.exe on the mounted Windows drive; an .exe there runs under Wine. With none of those, repak
# (github.com/trumank/repak, what MINT packs with) on PATH packs and extracts instead; UNREALPAK=<repak> picks it.
UNREALPAK_DEFAULT = r"C:\Program Files\Epic Games\UE_4.27\Engine\Binaries\Win64\UnrealPak.exe"
UNREALPAK = os.environ.get("UNREALPAK") or (UNREALPAK_DEFAULT if os.name == "nt" else
                                            shutil.which("UnrealPak") or drive_path(UNREALPAK_DEFAULT) or "UnrealPak")
UNREALPAK_RUN = ([shutil.which("wine") or "wine", UNREALPAK]
                 if os.name != "nt" and UNREALPAK.lower().endswith(".exe") else [UNREALPAK])
REPAK = (UNREALPAK if os.path.basename(UNREALPAK).lower().startswith("repak") else
         shutil.which("repak") if not os.environ.get("UNREALPAK") and not os.path.exists(UNREALPAK) else None)


def newest(paths):
    times = [os.path.getmtime(p) for p in paths if os.path.exists(p)]
    return max(times) if times else 0


def oldest(paths):
    times = [os.path.getmtime(p) for p in paths if os.path.exists(p)]
    return min(times) if times else 0


def tree_files(root):
    """Every file under root, subfolders included (UeApi/Game/ holds the dumped Blueprint classes)."""
    return [os.path.join(d, f) for d, _dirs, files in os.walk(root) for f in files]


# A compile is one busy core, like the suite's WORKERS; BPBUILD_JOBS overrides it.
JOBS = int(os.environ.get("BPBUILD_JOBS") or max(1, (os.cpu_count() or 2) // 2))


def mod_settings(mod, config):
    """What mods.yaml says about a mod, as the text its build stamp holds: its own entry (sources, needs,
    embed, generate_api, api_dir) and the top-level keys a compile reads. A change to any of them restales
    the mod - turning `embed` on must repack, though no source changed."""
    return json.dumps({"mod": mod, "api_dir": config.get("api_dir"), "game_content": config.get("game_content")},
                      sort_keys=True, default=str)


def read_text(path):
    return io.open(path, encoding="utf-8").read() if os.path.exists(path) else None


def mod_package(sources):
    """The first UE_MOD_PACKAGE among a mod's sources - any one .cpp of a multi-file mod may carry it."""
    for source in sources:
        if source.endswith(".cpp"):
            m = MOD_PACKAGE.search(io.open(source, encoding="utf-8-sig", errors="replace").read())
            if m:
                return m.group(1)
    sys.exit("%s declares no UE_MOD_PACKAGE" % ", ".join(s for s in sources if s.endswith(".cpp")))


INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)
GENERATED = ("UeApi", "UeAssets")  # dumped headers: thousands of files, covered by toolchain_time instead


def mod_sources(mod, bp):
    """A mod's `sources`, globs expanded (`Foo/*.cpp`), plus every local header they #include, transitively,
    so a mod lists its .cpp files only. An entry naming nothing comes back as-is, for the caller's missing check."""
    out = []
    for entry in mod.get("sources") or []:
        path = os.path.join(bp, entry)
        out += sorted(glob.glob(path)) if glob.has_magic(entry) else [path]
    seen = set(os.path.normcase(os.path.abspath(p)) for p in out)
    todo = [p for p in out if os.path.exists(p)]
    while todo:
        src = todo.pop()
        for inc in INCLUDE.findall(io.open(src, encoding="utf-8-sig", errors="replace").read()):
            path = os.path.abspath(os.path.join(os.path.dirname(src), inc))
            key = os.path.normcase(path)
            if any(g.lower() in key.split(os.sep) for g in GENERATED):
                continue
            if key not in seen and os.path.exists(path):
                seen.add(key)
                out.append(path)
                todo.append(path)
    return out


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


API_UE_VERSION = "4.27"  # the editor stubs (Uncooked.cpp) are 4.27 only; cooked output follows the UeApi (S40)

# The game's project folder, which a pak's tree and mount point are named after (FSD/Content, RogueCore/Content):
# the UeApi's Version.json "project", which genueapi reads off the dump. Set by main.
PROJECT = "FSD"


def ueapi_project(ue_api):
    """The project a UeApi was generated for; one from before S40 carries none and is DRG's."""
    try:
        return json.load(io.open(os.path.join(ue_api, "Version.json"), encoding="utf-8")).get("project") or "FSD"
    except (OSError, ValueError):
        return "FSD"


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
        out.append(manifest_path(bp, path))
    return out


def content_dir(stage_fsd, package):
    return os.path.join(stage_fsd, "Content", *package.replace("/Game/", "").split("/"))


def dep_stage(dep, by_name, bp):
    """Where dep's cooked assets sit - the import path (its UE_MOD_PACKAGE) plus its build dir,
    both known from the manifest, so nothing needs threading through the build loop."""
    package = mod_package(mod_sources(by_name[dep], bp))
    fsd = os.path.join(bp, "build", dep, PROJECT)
    return content_dir(fsd, package), package, fsd


def embed_deps(dep_stages, stage_fsd, assetgen):
    """Copy each dep's staged assets into this mod's FSD tree so the pak is self-contained, and its
    registry rows into the pak's one FSD/AssetRegistry.bin. Clears each dest first so a
    renamed/dropped dep asset can't linger and ship stale."""
    for content, package, fsd in dep_stages:
        dest = content_dir(stage_fsd, package)
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        os.makedirs(dest)
        for f in cooked_assets(content):
            to = os.path.join(dest, os.path.relpath(f, content))
            os.makedirs(os.path.dirname(to), exist_ok=True)
            shutil.copy2(f, to)
        registry = os.path.join(fsd, "AssetRegistry.bin")
        if os.path.exists(registry) and subprocess.run(
                [assetgen, "registry", os.path.join(stage_fsd, "AssetRegistry.bin"), registry]).returncode != 0:
            return False
    return True


def bundle_files(mod, bp):
    """The files a mod's `bundle:` folders hold, each as (the file, its path under the FSD tree). A folder is laid out
    like a staged FSD tree (`Content/...`), so the relative path is the one the pak ships it at."""
    out = []
    for folder in mod.get("bundle") or []:
        root = os.path.join(bp, folder)
        if not os.path.isdir(root):
            sys.exit("mods.yaml: a `bundle` entry is not a folder: %s" % folder)
        out += [(os.path.join(dirpath, f), os.path.relpath(os.path.join(dirpath, f), root))
                for dirpath, _dirs, files in os.walk(root) for f in files]
    return out


def stage_bundle(files, stage_fsd):
    """Copy a mod's bundle into its FSD tree to ship verbatim: copy2 only, nothing reads or rewrites the bytes."""
    for src, rel in files:
        to = os.path.join(stage_fsd, rel)
        os.makedirs(os.path.dirname(to), exist_ok=True)
        shutil.copy2(src, to)


def mint_blob(url, cache):
    """The file MINT downloaded for `url`, a MODIO_URL, out of the MINT cache folder `cache`: (its path, what it is,
    a note or None). Unpinned, it is the mod's latest file MINT knows, the one a profile that lists the same URL
    installs; pinned, that file, with a note when MINT knows a newer one. Stops the build when MINT has not fetched
    it. content_of calls it for a `game_content` or a `kept` `from` that is a mod.io URL; the cache reading itself
    is mintcache.lookup, shared with tools/mintlib.py."""
    try:
        hit = mintcache.lookup(url, cache)
    except mintcache.Miss as e:
        sys.exit(str(e))
    return hit["path"], hit["what"], hit["note"] and hit["note"] + ": recheck the patch against it"


def content_of(src, bp, root):
    """The Content folder of `src`, a path in mods.yaml: the folder itself, or, for a .pak or a .zip holding one (a
    mod.io download, as MINT caches it), its extraction under `root`, made once and again only when the file changes.
    A mod.io URL (MODIO_URL) names the download MINT cached for it (mint_blob)."""
    what = None
    if MODIO_URL.match(src):
        src, what, note = mint_blob(src, MINT_CACHE)
        if note:
            print("note: " + note)
    src = manifest_path(bp, src)
    if os.path.isdir(src):
        root = src
    else:
        stamp = os.path.join(root, ".pak")
        want = "%s %d %d" % (src, os.path.getsize(src), int(os.path.getmtime(src)))
        if read_text(stamp) != want:
            shutil.rmtree(root, ignore_errors=True)
            os.makedirs(root)
            pak = src
            if zipfile.is_zipfile(src):
                with zipfile.ZipFile(src) as z:
                    paks = [n for n in z.namelist() if n.lower().endswith(".pak")]
                    if len(paks) != 1:
                        sys.exit("%s holds %d .pak files, not one" % (src, len(paks)))
                    pak = z.extract(paks[0], os.path.join(root, ".zip"))
            extract = [REPAK, "unpack", pak, "-o", root, "-f", "-q"] if REPAK else UNREALPAK_RUN + [pak, "-Extract", root]
            proc = subprocess.run(extract, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, errors="replace")
            if proc.returncode != 0:
                sys.exit("UnrealPak could not extract %s:\n%s" % (pak, proc.stdout[-2000:]))
            io.open(stamp, "w", encoding="utf-8").write(want)
            if what:
                print("extracted %s -> %s" % (what, root))
    for sub in ("Content", os.path.join("FSD", "Content")):
        if os.path.isdir(os.path.join(root, sub)):
            return os.path.join(root, sub)
    return root


def kept_source(entry, mod, bp, i):
    """The Content folder a `kept` entry's packages come from: its `from`, extracted under build/<mod>/kept (kept<i>
    for the i-th entry after the first)."""
    return content_of(entry["from"], bp, os.path.join(bp, "build", mod["name"], "kept" + (str(i) if i else "")))


def place_kept(mod, bp, stage_fsd):
    """A mod's `kept` packages - assets it ships as they were cooked, which no source of it builds (textures, fonts,
    a data asset no source fills) - into its FSD tree for the pak. `kept` is one `{from, paths}` entry or a list of
    them; `paths` lists /Game package paths or folders. Hard links where the volume allows: a texture folder runs to
    gigabytes. Returns how many files it placed."""
    kept = mod.get("kept")
    if not kept:
        return 0
    n = 0
    for i, entry in enumerate(kept if isinstance(kept, list) else [kept]):
        n += place_kept_entry(entry, mod, bp, i, stage_fsd)
    return n


def place_kept_entry(entry, mod, bp, i, stage_fsd):
    content = kept_source(entry, mod, bp, i)
    n = 0
    for path in entry.get("paths") or []:
        rel = path[len("/Game/"):] if path.startswith("/Game/") else path.lstrip("/")
        base = os.path.join(content, *rel.split("/"))
        files = [os.path.join(r, f) for r, _d, fs in os.walk(base) for f in fs] if os.path.isdir(base) else \
            [base + ext for ext in (".uasset", ".uexp", ".ubulk", ".umap") if os.path.exists(base + ext)]
        if not files:
            sys.exit("%s: kept path %s is not in %s" % (mod["name"], path, content))
        for f in files:
            to = os.path.join(stage_fsd, "Content", os.path.relpath(f, content))
            os.makedirs(os.path.dirname(to), exist_ok=True)
            if os.path.exists(to):
                os.remove(to)
            try:
                os.link(f, to)
            except OSError:
                shutil.copy2(f, to)
            n += 1
    return n


def staged_assets(stage_content):
    if not os.path.isdir(stage_content):
        return []
    return [os.path.join(stage_content, f) for f in os.listdir(stage_content)
            if f.endswith(".uasset") or f.endswith(".uexp")]


def cooked_assets(stage_content):
    """Every package a compile staged in a mod's folder, its subfolders included, since a namespace is a folder.
    An `api_dir` keeps to staged_assets: it can share its folders with a project's hand-made assets."""
    return [os.path.join(root, f) for root, _dirs, files in os.walk(stage_content)
            for f in files if f.endswith(".uasset") or f.endswith(".uexp")]


def staged_edits(stage_fsd, stage_content, game_dir):
    """The game's packages a mod's compile edited, each as (its staged copy, the game's own): every cooked file in the
    staged Content tree, outside the mod's own folder, that game_content holds too. An embedded dep's folder is no
    game package, so it is not one."""
    if not game_dir:
        return []
    content = os.path.join(stage_fsd, "Content")
    own = os.path.normcase(os.path.abspath(stage_content))
    out = []
    for root, _dirs, files in os.walk(content):
        here = os.path.normcase(os.path.abspath(root))
        if here == own or here.startswith(own + os.sep):
            continue
        for f in files:
            if f.endswith((".uasset", ".uexp", ".umap")):
                staged = os.path.join(root, f)
                game = os.path.join(game_dir, os.path.relpath(staged, content))
                if os.path.exists(game):
                    out.append((staged, game))
    return out


def value_reads(out):
    """The game packages a compile read values off when the mod was built: each `value <- <package>` line it printed."""
    return sorted(set(re.findall(r"^  value +<- (\S+)", out, re.M)))


def read_files(reads_file, game_dir):
    """The game's files of the packages reads_file lists (value_reads, one a line), as game_content holds them now."""
    if not game_dir or not os.path.exists(reads_file):
        return []
    return [os.path.join(game_dir, *p[len("/Game/"):].split("/")) + ext
            for p in read_text(reads_file).split() for ext in (".uasset", ".uexp", ".umap")
            if os.path.exists(os.path.join(game_dir, *p[len("/Game/"):].split("/")) + ext)]


def run_compile(cmd):
    """One assetgen compile, on a pool thread. Its output is captured, for the caller to print in build order."""
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    return proc.returncode == 0, proc.stdout


def run_unrealpak(fsd_dir, pak_path):
    if REPAK:
        # What UnrealPak writes for the response file below: UE 4.27's pak version, zlib, the tree under ../../../FSD/.
        if os.path.exists(pak_path):
            os.remove(pak_path)
        proc = subprocess.run([REPAK, "pack", "--version", "V11", "--compression", "Zlib", "-m", "../../../%s/" % PROJECT,
                               fsd_dir, pak_path, "-q"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if proc.returncode != 0:
            print("  repak failed (%d):" % proc.returncode)
            print("    " + "\n    ".join(proc.stdout.strip().splitlines()[-6:]))
            return False
        return True
    if not os.path.exists(UNREALPAK):
        print("  UnrealPak not found at %s - skipping the pak." % UNREALPAK)
        return False

    response = os.path.join(os.path.dirname(fsd_dir), "autogen.txt")
    io.open(response, "w", encoding="utf-8", newline="\n").write(
        '"%s" "..\\..\\..\\%s\\*.*"\n' % (os.path.join(fsd_dir, "*.*"), PROJECT))

    cmd = UNREALPAK_RUN + [pak_path, "-platform=Windows", "-create=" + response, "-compress"]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        print("  UnrealPak failed (%d):" % proc.returncode)
        print("    " + "\n    ".join(proc.stdout.strip().splitlines()[-6:]))
        return False
    return True


def check_mod_imports(staged, mod_packages):
    """A generated asset is <mod package>/<class>; a UE_CLASS naming the folder instead writes an
    import that loads fine and resolves to null, first seen as a null UFunction inside the VM."""
    def packages(content):
        return sorted(os.path.relpath(f, content).replace(os.sep, "/")
                      for f in cooked_assets(content) if f.endswith(".uasset"))

    # A mod may cook under a second root (`namespace Game::...`, as EEE does), beside its package's folder: look at
    # its whole Content folder, which holds what it ships, kept packages included.
    staged = [(mod, os.path.normpath(os.path.join(content, *[".."] * (package.count("/") - 1))), "/Game")
              for mod, content, package in staged]
    produced = set()
    for _mod, content, package in staged:
        produced.update(package + "/" + f[:-len(".uasset")] for f in packages(content))

    bad = []
    for _mod, content, _package in staged:
        for f in packages(content):
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


def vs_namespace_filter(path, folder_name):
    """The sub-filter a mod's source goes under in Visual Studio: its first namespace, which is its package folder
    in game (Manager::Core::Services::Logger gives Manager\\Core\\Services\\Logger), so a mod of hundreds of files in
    one folder (EMM, EEE) shows as its package tree rather than one alphabetical list. A leading Game:: (a child of a
    game Blueprint) is dropped. None for a file with no namespace and for the mod's own umbrella files, named after
    its folder (ElytrasModManager.h / .cpp), which stay at the top."""
    if os.path.splitext(os.path.basename(path))[0].lower() == folder_name.lower():
        return None
    m = re.search(r"^namespace ([\w:]+) \{", io.open(path, encoding="utf-8-sig", errors="replace").read(), re.M)
    if not m:
        return None
    name = m.group(1)
    if name.startswith("Game::"):
        name = name[len("Game::"):]
    return name.replace("::", "\\")


def write_vs_filters(bp):
    """The project's file list and its folders (Mods / Tests / Helpers / UeApi), written from what is on disk, on
    every build and only when it changed - a rewrite makes Visual Studio reload the project. Wildcards would need no
    writing, but Visual Studio pairs a filter with an item by its Include STRING: a wildcard filter reaches the first
    file only, and a named filter does not reach a wildcard item at all. So both files name every file."""
    project = os.path.join(bp, "BpMods.vcxproj")
    if not os.path.exists(project):
        return
    # A mod named *Test is a test (CppTest is HelloWorld.cpp): its sources go under Tests, not Mods.
    manifest = os.path.join(bp, "mods.yaml")
    mods = (yaml.safe_load(io.open(manifest, encoding="utf-8-sig").read()) or {}).get("mods") or [] \
        if os.path.exists(manifest) else []
    tests = {os.path.normcase(os.path.normpath(p)) for m in mods if str(m.get("name", "")).endswith("Test")
             for s in m.get("sources") or [] for p in glob.glob(os.path.join(bp, s))}
    rows = []
    for f in sorted(os.listdir(bp), key=str.lower):
        if f.endswith(".cpp"):
            test = f.endswith("Test.cpp") or os.path.normcase(os.path.normpath(os.path.join(bp, f))) in tests
            rows.append(("ClCompile", f, "Tests" if test else "Mods"))
        elif f.endswith(".h"):
            rows.append(("ClInclude", f, "Helpers"))
    # A mod in its own folder (`MyMod/`) gets a filter of its own under Mods (`Mods\MyMod`), nested as the folders
    # are; everything under `Tests/` (the test mods and each mod's behaviour tests, .py included) goes under Tests
    # instead. Generated headers and build output are skipped at any depth (a game's own `RC/UeApi` is thousands of
    # files). A filter's GUID is derived from its name, so a rewrite does not churn it.
    filters = list(VS_FILTERS)
    known = {f for f, _ in filters}
    def add_filter(folder):
        # Each level of a nested filter is declared, the outer first, once.
        parts = folder.split("\\")
        for i in range(1, len(parts) + 1):
            level = "\\".join(parts[:i])
            if level not in known:
                known.add(level)
                filters.append((level, str(uuid.uuid5(uuid.NAMESPACE_URL, "bpmods:" + level.lower()))))
    for root, dirs, files in os.walk(bp):
        dirs[:] = sorted((d for d in dirs if d not in ("build", "out", "__pycache__") + GENERATED), key=str.lower)
        if root != bp:
            rel = os.path.relpath(root, bp)
            test = rel.split(os.sep)[0] == "Tests"
            folder = rel if test else "Mods\\" + rel
            if folder != "Tests":
                add_filter(folder)
            for f in sorted(files, key=str.lower):
                if f.endswith((".cpp", ".h")) or (test and f.endswith(".py")):
                    kind = "ClCompile" if f.endswith(".cpp") else "ClInclude" if f.endswith(".h") else "None"
                    where = folder
                    sub = None if test else vs_namespace_filter(os.path.join(root, f), os.path.basename(root))
                    if sub:
                        where = folder + "\\" + sub
                        add_filter(where)
                    rows.append((kind, os.path.relpath(os.path.join(root, f), bp), where))
    # The compiler's own test mods (their headers and tests/ast too, not the test run's build/) and the headers any mod
    # may include live beside it, in AssetGen.
    add_filter("Tests\\AssetGen")
    for sub, folder in (("tests", "Tests\\AssetGen"), ("include", "Helpers")):
        d = os.path.join(bp, "..", "AssetGen", sub)
        for root, dirs, files in os.walk(d):
            dirs[:] = sorted((x for x in dirs if x != "build"), key=str.lower)
            for f in sorted(files, key=str.lower):
                if f.endswith((".cpp", ".h")) and (sub == "tests" or f.endswith(".h")):
                    rel = os.path.relpath(os.path.join(root, f), os.path.join(bp, ".."))
                    rows.append(("ClCompile" if f.endswith(".cpp") else "ClInclude", "..\\" + rel, folder))
    api = os.path.join(bp, "UeApi")
    for f in sorted(os.listdir(api), key=str.lower) if os.path.isdir(api) else []:
        if f.endswith(".h"):
            rows.append(("ClInclude", "UeApi\\" + f, "UeApi"))
    text = ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<!-- Written by AssetGen/tools/bpbuild.py on every build. Do not edit. -->\n'
            '<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">\n  <ItemGroup>\n'
            + "".join('    <Filter Include="%s"><UniqueIdentifier>{%s}</UniqueIdentifier></Filter>\n' % f for f in filters)
            + '  </ItemGroup>\n  <ItemGroup>\n'
            + "".join('    <%s Include="%s"><Filter>%s</Filter></%s>\n' % (kind, name, folder, kind) for kind, name, folder in rows)
            + '    <None Include="mods.yaml" />\n  </ItemGroup>\n</Project>\n')
    dest = os.path.join(bp, "BpMods.vcxproj.filters")
    if not os.path.exists(dest) or io.open(dest, encoding="utf-8").read() != text:
        io.open(dest, "w", encoding="utf-8", newline="\n").write(text)

    # The same list, between the two marker comments of the project file.
    begin, end = "<!-- items: begin (bpbuild.py) -->", "<!-- items: end -->"
    old = io.open(project, encoding="utf-8", newline="").read()
    if begin in old and end in old:
        items = "".join('\n    <%s Include="%s" />' % (kind, name) for kind, name, _ in rows) + '\n    <None Include="mods.yaml" />\n    '
        new = old[:old.index(begin) + len(begin)] + items + old[old.index(end):]
        if new != old:
            io.open(project, "w", encoding="utf-8", newline="").write(new)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__.strip().splitlines()[-1])
    # Absolute: subprocess resolves a relative exe against PATH, not the cwd.
    repo, ue_api, assetgen = (os.path.abspath(p) for p in sys.argv[1:4])
    global PROJECT
    PROJECT = ueapi_project(ue_api)
    force = "--force" in sys.argv[4:]
    no_pak = "--no-pak" in sys.argv[4:]

    bp = repo if os.path.exists(os.path.join(repo, "mods.yaml")) else os.path.join(repo, "BpMods")
    write_vs_filters(bp)
    manifest = os.path.join(bp, "mods.yaml")
    if not os.path.exists(manifest):
        sys.exit("no manifest at %s" % manifest)

    config = yaml.safe_load(io.open(manifest, encoding="utf-8-sig").read()) or {}
    mods = config.get("mods") or []
    if not mods:
        print("mods.yaml lists no mods.")
        return 0

    # UE_ASSET_EDIT / UE_PATCH start from the game's own packages: the /Game folder of the extracted game pak.
    # ponytail: a mod's staleness does not see this tree (300k files); after re-extracting an updated game, --force.
    game = config.get("game_content")
    game_dir = manifest_path(bp, game) if game else None

    game_dirs = {}

    def mod_game_dir(mod):
        """A mod's own `game_content` wins over the manifest's: another mod's pak, mod.io zip or mod.io URL, for a
        patch of a third-party mod's Blueprint, extracted under build/<mod>/game. Resolved once a build."""
        own = mod.get("game_content")
        if not own:
            return game_dir
        if mod["name"] not in game_dirs:
            game_dirs[mod["name"]] = content_of(own, bp, os.path.join(bp, "build", mod["name"], "game"))
        return game_dirs[mod["name"]]

    # The dumped headers mod_sources does not walk: UeApi and its sibling UeAssets, each with its subfolders.
    ue_assets = os.path.join(os.path.dirname(ue_api), "UeAssets")
    toolchain_time = max(newest(tree_files(ue_api)), newest(tree_files(ue_assets)), newest([assetgen]))

    by_name = dict((m["name"], m) for m in mods)

    built, packed, skipped, failed = [], [], [], []
    staged = []
    records = []
    entries = []
    # Phase 1 - compile every mod. `embed` bakes a dep's cooked assets into the dependent's pak, so
    # all assets must exist before any pak; with mutual embed no build order puts both deps first,
    # so compiling is a pass of its own, separate from packing (phase 2). No compile reads another
    # mod's output, so they run side by side.
    pool = ThreadPoolExecutor(JOBS)
    for mod in order(mods):
        name = mod["name"]
        sources = mod_sources(mod, bp)
        missing = [s for s in sources if not os.path.exists(s)]
        if not any(s.endswith(".cpp") for s in sources) or missing:
            print("%-16s SKIP - no such source: %s" % (name, ", ".join(missing) or "(no .cpp listed)"))
            failed.append(name)
            continue

        package = mod_package(sources)
        game = mod_game_dir(mod)
        stage_fsd = os.path.join(bp, "build", name, PROJECT)
        stage_content = os.path.join(stage_fsd, "Content", *package.replace("/Game/", "").split("/"))
        assets = cooked_assets(stage_content)
        staged.append((name, stage_content, package))

        # `generate_api` writes the editor-side stub next to nothing else, so it has its own
        # staleness: turning the flag on for an already-built mod must still produce one.
        # assetgen writes one `--api` dir; further ones are mirrors of it, filled after the compile.
        api_contents = ([os.path.join(root, *package.replace("/Game/", "").split("/"))
                         for root in api_roots(mod, config, bp)]
                        if mod.get("generate_api") else [])
        api_content = api_contents[0] if api_contents else None
        api_missing = any(not staged_assets(d) for d in api_contents)

        # An edit's output is the game's package, edited: it is stale when the game's copy is newer (a re-extract
        # after a game update), or an edit of the old package would ship over the new one.
        edits = staged_edits(stage_fsd, stage_content, game)
        outputs = assets + [s for s, _g in edits]
        # The stamp sits beside FSD, not in it: the pak takes the whole FSD tree.
        settings = mod_settings(mod, config)
        settings_file = os.path.join(bp, "build", name, "mod.json")
        # A value the compile read off a game package is baked into the mod: a newer game copy restales it too.
        reads_file = os.path.join(bp, "build", name, "reads.txt")
        inputs_time = max(newest(sources), toolchain_time)
        stale = (force or not outputs or api_missing
                 or read_text(settings_file) != settings
                 or inputs_time > oldest(outputs)
                 or any(os.path.getmtime(g) > os.path.getmtime(s) for s, g in edits)
                 or newest(read_files(reads_file, game)) > oldest(outputs))
        job = None
        if stale:
            # The pak takes the whole FSD tree, so the whole Content tree goes, not just this package's folder:
            # an asset the sources no longer cook (a struct another mod now owns), a folder left by an earlier
            # UE_MOD_PACKAGE, a failed compile's leftovers. Embedded deps are copied back in at pack time.
            shutil.rmtree(os.path.join(stage_fsd, "Content"), ignore_errors=True)
            if os.path.exists(os.path.join(stage_fsd, "AssetRegistry.bin")):
                os.remove(os.path.join(stage_fsd, "AssetRegistry.bin"))    # the compiles below refill it
            os.makedirs(stage_content)
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
            # One translation unit per mod: compiled apart, every .cpp would re-cook each class of the headers
            # they share, the last one's copy winning with only its own bodies. So several .cpp files compile as
            # a generated file that #includes them all - each still resolves its quoted includes from its own folder.
            # ponytail: a unity build, so file-local names (static, anonymous namespaces) must not clash across files.
            # The listed .cpp files only: one reached through an #include (a mod whose main file takes in the
            # rest) is in `sources` for staleness, and compiled where it is included.
            listed = {os.path.normcase(os.path.abspath(p)) for e in mod.get("sources") or []
                      for p in (glob.glob(os.path.join(bp, e)) if glob.has_magic(e) else [os.path.join(bp, e)])}
            cpps = [s for s in sources if s.endswith(".cpp") and os.path.normcase(os.path.abspath(s)) in listed]
            unit = cpps[0]
            if len(cpps) > 1:
                unit = os.path.join(bp, "build", name, name + ".unity.cpp")
                io.open(unit, "w", encoding="utf-8", newline="\n").write(
                    "".join('#include "%s"\n' % os.path.abspath(s).replace("\\", "/") for s in cpps))
            cmd = [assetgen, "compile", unit, ue_api, stage_content]
            if api_content:
                cmd += ["--api", api_content]
            if game:
                cmd += ["--game", game]
            job = pool.submit(run_compile, cmd)
        entries.append((mod, name, package, stage_fsd, stage_content, stale, job, api_contents, settings_file, settings,
                        inputs_time))

    # The compiles run on the pool while the rest of the mods are prepared; their results are taken in
    # build order, so each mod's output prints whole and the log reads as it did when they ran one by one.
    for (mod, name, package, stage_fsd, stage_content, stale, job, api_contents, settings_file, settings,
         inputs_time) in entries:
        if job:
            ok, out = job.result()
            sys.stdout.write(out)
            if not ok:
                print("%-16s FAILED" % name)
                failed.append(name)
                continue
            for api_dir in api_contents:
                if api_dir != api_contents[0]:
                    for f in staged_assets(api_contents[0]):
                        shutil.copy2(f, os.path.join(api_dir, os.path.basename(f)))
                write_api_manifest(api_dir)
            reads = value_reads(out)
            reads_file = os.path.join(bp, "build", name, "reads.txt")
            if reads:
                io.open(reads_file, "w", encoding="utf-8", newline="\n").write("\n".join(reads) + "\n")
            elif os.path.exists(reads_file):
                os.remove(reads_file)
            # Stamped only once it compiled, so a failed compile stays stale; and only when no source was saved while
            # it compiled (an editor's save, widgetloop --watch), which the assets, newer than that save, would hide.
            if max(newest(mod_sources(mod, bp)), toolchain_time) == inputs_time:
                io.open(settings_file, "w", encoding="utf-8", newline="\n").write(settings)
            elif os.path.exists(settings_file):
                os.remove(settings_file)
            print("%-16s compiled -> %s" % (name, package))
            built.append(name)

        for api_dir in api_contents:
            # relpath throws across drives - an `api_dir` on another one is perfectly normal.
            shown = api_dir if os.path.splitdrive(api_dir)[0] != os.path.splitdrive(bp)[0] \
                else os.path.relpath(api_dir, bp)
            print("%-16s api      -> %s" % (name, shown))

        records.append((mod, name, package, stage_fsd, stage_content, stale))
    pool.shutdown()

    # Phase 2 - pack. Every mod's assets now exist, so an `embed` mod can bake in its deps.
    for mod, name, package, stage_fsd, stage_content, stale in records:
        embed = bool(mod.get("embed"))
        dep_stages = [dep_stage(d, by_name, bp) for d in transitive_needs(name, by_name)] if embed else []
        dep_assets = [f for content, _p, _f in dep_stages for f in cooked_assets(content)]
        bundle = bundle_files(mod, bp)
        assets = cooked_assets(stage_content) + [s for s, _g in staged_edits(stage_fsd, stage_content, mod_game_dir(mod))]
        pak = os.path.join(bp, "out", name + "_P.pak")
        # A dep's assets are baked into this pak, so a change to one restales it; likewise our own
        # assets after a --no-pak run. Judged on our sources alone the old pak would ship, and a mod
        # calling a function it lacks crashes on the null UFunction.
        # A bundle's files are baked in the same way, so they count with the deps.
        unpacked = not no_pak and max(newest(assets), newest(dep_assets + [src for src, _rel in bundle])) > newest([pak])
        if not stale and not unpacked:
            print("%-16s up to date" % name)
            skipped.append(name)
            continue
        if no_pak:
            continue
        if embed and not embed_deps(dep_stages, stage_fsd, assetgen):
            failed.append(name)
            continue
        stage_bundle(bundle, stage_fsd)
        if mod.get("kept"):     # after the deps: a kept package wins over a rebuilt one of the same path
            print("%-16s kept     %d files" % (name, place_kept(mod, bp, stage_fsd)))
        if not os.path.isdir(os.path.dirname(pak)):
            os.makedirs(os.path.dirname(pak))
        if run_unrealpak(stage_fsd, pak):
            print("%-16s packed   -> %s" % (name, pak))
            packed.append(name)
        else:
            failed.append(name)

    # Under --no-pak no mod's kept packages are placed, so every import of one would read as missing:
    if not failed and not no_pak and not check_mod_imports(staged, set(p for _n, _c, p in staged)):
        failed.append("cross-mod imports")

    print("built %d, packed %d, up to date %d, failed %d"
          % (len(built), len(packed), len(skipped), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
