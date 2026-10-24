"""MINT's download cache (drg-mod-integration), read-only and standard library only.

cache.json maps each mod.io mod MINT has fetched to its files, and each file to a blob: the download as mod.io
serves it (a .zip holding the mod's .pak), stored under blobs/ by its sha256. The layout is the fork's
src/providers/modio.rs `ModioCache`: mod_id_map (name_id -> mod id), mods (id -> name_id, name, latest_modfile,
modfiles), modfile_blobs (modfile id -> blob name), dependencies, last_update_time.

The one reader of that cache: bpbuild's `game_content` URLs (mint_blob there) and tools/mintlib.py both come here.
It lives beside bpbuild so AssetGen's public mirror stays self-contained.
"""
import io
import json
import os
import re

MINT_CACHE = os.environ.get("MINT_CACHE") or (
    os.path.join(os.environ.get("LOCALAPPDATA", ""), "drg-mod-integration", "cache") if os.name == "nt" else
    os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "drg-mod-integration"))

# A mod.io mod as a MINT profile lists it: https://mod.io/g/drg/m/<name>, then MINT's `#<mod id>`, then `/<modfile id>`
# when the profile pins one version. Looser than MINT's own regex (scheme, www. and a trailing slash are optional).
MODIO_URL = re.compile(r"(?:https?://)?(?:www\.)?mod\.io/g/drg/m/([^#/?]+)/?(?:#(\d+)(?:/(\d+))?)?$")


class Miss(Exception):
    """The cache cannot answer: no cache, a mod MINT never fetched, or a file it never downloaded. The message
    names the URL and what to do."""


def modio_url(name_id, mod_id, modfile=None):
    """The URL the way MINT writes it into a profile (modio.rs format_spec)."""
    return "https://mod.io/g/drg/m/%s#%s" % (name_id, mod_id) + ("/%s" % modfile if modfile else "")


def index(cache=MINT_CACHE, url=""):
    """cache.json's mod.io section. `url` only prefixes the Miss message."""
    path = os.path.join(cache, "cache.json")
    if not os.path.exists(path):
        raise Miss("%s: no MINT cache at %s (set MINT_CACHE to the folder that holds cache.json)" % (url, cache))
    return json.load(io.open(path, encoding="utf-8"))["cache"].get("modio") or {
        "mod_id_map": {}, "mods": {}, "modfile_blobs": {}, "dependencies": {}, "last_update_time": None}


def find(query, modio):
    """Mod ids whose name_id or display name matches `query`: an exact name_id or name first, else every mod whose
    name_id or name contains it (case-insensitive)."""
    q = query.casefold()
    mods = modio.get("mods") or {}
    exact = [i for i, m in mods.items() if q in (m.get("name_id", "").casefold(), m.get("name", "").casefold())]
    return exact or [i for i, m in mods.items() if q in m.get("name_id", "").casefold() or q in m.get("name", "").casefold()]


def lookup(url, cache=MINT_CACHE, modio=None):
    """The file MINT downloaded for `url`, a MODIO_URL: a dict with name_id, name, mod_id, modfile (the one a
    profile listing this URL installs: the latest MINT knows when unpinned, else the pinned one), latest,
    version / latest_version, path (the blob), pinned, what ("<name_id> <version> (modfile <id>)") and note
    (set when a pinned file is not the latest MINT knows). Raises Miss when MINT has not fetched the mod or not
    downloaded that file."""
    m = MODIO_URL.match(url)
    if not m:
        raise Miss("%s: not a mod.io DRG mod URL" % url)
    if modio is None:
        modio = index(cache, url)
    mod_id = m.group(2) or modio.get("mod_id_map", {}).get(m.group(1))
    mod = modio.get("mods", {}).get(str(mod_id)) if mod_id is not None else None
    if not mod:
        raise Miss("%s: MINT has not fetched this mod; add it to a MINT profile and let MINT update" % url)
    name_id = mod.get("name_id", m.group(1))
    latest = str(mod["latest_modfile"])
    want = m.group(3) or latest
    versions = dict((str(f["id"]), f.get("version") or "?") for f in mod.get("modfiles") or [])
    what = "%s %s (modfile %s)" % (name_id, versions.get(want, "?"), want)
    blob = modio.get("modfile_blobs", {}).get(want)
    path = os.path.join(cache, "blobs", blob) if blob else None
    if not path or not os.path.exists(path):
        raise Miss("%s: MINT has not downloaded %s; let MINT update a profile that lists it" % (url, what))
    note = None
    if want != latest:
        note = "%s is pinned; MINT knows %s %s (modfile %s)" % (
            what, name_id, versions.get(latest, "?"), latest)
    return {"name_id": name_id, "name": mod.get("name", name_id), "mod_id": int(mod_id), "modfile": int(want),
            "latest": int(latest), "version": versions.get(want, "?"), "latest_version": versions.get(latest, "?"),
            "path": path, "pinned": bool(m.group(3)), "what": what, "note": note}
