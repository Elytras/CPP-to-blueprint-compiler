#!/usr/bin/env python3
"""usage: dumpexp.py <base-path-without-ext> <export-index> [count]
The old name of `uasset.py exports`, kept for the commands and notes that cite it."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uasset  # noqa: E402
from uasset_io import Imp, R, load, pidx, preload, summary  # noqa: E402,F401  BpMods/tools and tools/stubattach.py import these


def main():
    uasset.main(['exports'] + sys.argv[1:])


if __name__ == "__main__":
    main()
