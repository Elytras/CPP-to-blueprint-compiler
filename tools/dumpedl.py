#!/usr/bin/env python3
"""usage: dumpedl.py <file.uasset>
The old name of `uasset.py deps`, kept for the commands and notes that cite it."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uasset  # noqa: E402
from uasset import read_tables  # noqa: E402,F401


def main():
    uasset.main(['deps'] + sys.argv[1:])


if __name__ == "__main__":
    main()
