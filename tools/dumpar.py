#!/usr/bin/env python3
"""usage: dumpar.py <AssetRegistry.bin>
The old name of `uasset.py registry`, kept for the commands and notes that cite it."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uasset  # noqa: E402
from uasset import read_registry as read  # noqa: E402,F401  test_bytecode.py reads rows through it


def main():
    uasset.main(['registry'] + sys.argv[1:])


if __name__ == "__main__":
    main()
