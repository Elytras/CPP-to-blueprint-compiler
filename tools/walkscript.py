#!/usr/bin/env python3
"""usage: walkscript.py <base-path-without-ext> <export-index>
The old name of `uasset.py walk`, kept for the commands and notes that cite it."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uasset  # noqa: E402
from uasset import NAMES, W  # noqa: E402,F401


def main():
    uasset.main(['walk'] + sys.argv[1:])


if __name__ == "__main__":
    main()
