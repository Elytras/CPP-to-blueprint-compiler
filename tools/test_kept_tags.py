"""usage: python AssetGen/tools/test_kept_tags.py --assetgen <x64/Release/assetgen.exe>

bpbuild's registry rows for kept materials carry the tags the Material Creator filters on: MaterialDomain on a
Material (read off its name table), Parent on a MaterialInstanceConstant (its one material import), and
`assetgen registry-rows` stores `key=value` columns after the class as the row's tags.
"""
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bpbuild  # noqa: E402
import dumpar  # noqa: E402
from uasset_io import summary, tables  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PJ = os.path.join(REPO, "BpMods/ElytrasModManager/Kept/Content/_ElytrasMods/_Shared/Materials/UI/Paintjobs")
GAME = "/Game/_ElytrasMods/_Shared/Materials/UI/Paintjobs"
ASSETGEN = os.path.abspath(sys.argv[sys.argv.index("--assetgen") + 1])


def tags_of(name):
    b = open(os.path.join(PJ, name + ".uasset"), "rb").read()
    names, imps, exps = tables(b, summary(b))
    cls = next(names[imps[-e["cls"] - 1]["name"][0]] for e in exps if e["is_asset"] and e["outer"] == 0)
    return bpbuild.material_tags(names, imps, cls)


assert tags_of("M_SDF_Honeycomb_UI") == [("MaterialDomain", "MD_UI")], tags_of("M_SDF_Honeycomb_UI")
want = [("Parent", "Material'%s/M_SDF_Honeycomb_UI.M_SDF_Honeycomb_UI'" % GAME)]
assert tags_of("MI_SDF_HoneycombGold_UI") == want, tags_of("MI_SDF_HoneycombGold_UI")
ui = [os.path.basename(f)[:-7] for f in glob.glob(os.path.join(PJ, "M_*_UI.uasset"))]
assert len(ui) == 64 and all(tags_of(n) == [("MaterialDomain", "MD_UI")] for n in ui)
# their surface twins (when they ship) read as MD_Surface
surface = [n[:-3] for n in ui if os.path.exists(os.path.join(PJ, n[:-3] + ".uasset"))]
assert all(tags_of(n) == [("MaterialDomain", "MD_Surface")] for n in surface), [(n, tags_of(n)) for n in surface]
print("ok  kept materials: MaterialDomain and Parent read off the cooked packages")

with tempfile.TemporaryDirectory() as tmp:
    tsv, out = os.path.join(tmp, "rows.tsv"), os.path.join(tmp, "AssetRegistry.bin")
    open(tsv, "w", newline="\n").write("/Game/X/M_X\tM_X\tMaterial\tMaterialDomain=MD_UI\n/Game/X/M_Y\tM_Y\tMaterial\n")
    proc = subprocess.run([ASSETGEN, "registry-rows", out, tsv], capture_output=True, encoding="utf-8")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    rows = {r["asset_name"]: [tuple(t) for t in r["tags"]] for r in dumpar.read(out)[2]}
    assert rows == {"M_X": [("MaterialDomain", "MD_UI")], "M_Y": []}, rows
print("ok  registry-rows: key=value columns become the row's tags")
