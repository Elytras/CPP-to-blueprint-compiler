"""Create a Blueprint class's .h + .cpp in a BpMods folder and put them in the VS project at once.

Meant as a Visual Studio External Tool: select any file in the target folder, press the bound key, type the
class. The files land in that folder, and the project's filter follows the folder (bpbuild's write_vs_filters),
so VS reloads the project with them already under the right filter.

usage: newbpclass.py <dir> <Class> [: | Base] [Base]     e.g. newbpclass.py BpMods/ECD2A UEnemyHandler : UAddonHandlerBase
"""
import glob
import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bpbuild import MOD_PACKAGE, write_vs_filters


def main():
    args = [a for a in sys.argv[1:] if a != ":"]
    if len(args) < 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    folder, name = os.path.abspath(args[0]), args[1]
    base = args[2] if len(args) > 2 else "UObject"

    # The BpMods root is the nearest folder above with a mods.yaml.
    bp = folder
    while not os.path.exists(os.path.join(bp, "mods.yaml")):
        parent = os.path.dirname(bp)
        if parent == bp:
            sys.exit("%s is not inside BpMods (no mods.yaml above it)" % folder)
        bp = parent

    h, cpp = (os.path.join(folder, name + ext) for ext in (".h", ".cpp"))
    for f in (h, cpp):
        if os.path.exists(f):
            sys.exit("%s already exists" % f)

    # A local base gets its own header; an engine one comes from Engine.h. A folder whose sources declare a
    # UE_MOD_PACKAGE gets a UE_CLASS line, as everything in a multi-file mod shares its classes through one.
    include = base + ".h" if os.path.exists(os.path.join(folder, base + ".h")) else "Engine.h"
    package = next((m.group(1) for s in glob.glob(os.path.join(folder, "*.cpp"))
                    for m in [MOD_PACKAGE.search(io.open(s, encoding="utf-8-sig").read())] if m), None)
    body = '    UE_CLASS("%s/%s", "%s_C");\n' % (package, name, name) if package else ""

    io.open(h, "w", encoding="utf-8", newline="\n").write(
        '#pragma once\n#include "%s"\n\nclass %s : public %s\n{\npublic:\n%s};\n' % (include, name, base, body))
    io.open(cpp, "w", encoding="utf-8", newline="\n").write('#include "%s.h"\n' % name)
    write_vs_filters(bp)
    print("created %s, %s" % (h, cpp))


if __name__ == "__main__":
    main()
