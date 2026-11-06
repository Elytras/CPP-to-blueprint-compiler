#!/usr/bin/env python3
"""usage: dumptags.py <base-path-without-ext> <export-index> [byte-offset]
The old name of `uasset.py tags`, kept for the commands and notes that cite it."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uasset  # noqa: E402
from uasset import tags  # noqa: E402,F401  test_bytecode.py and tools/stubattach.py import it


def main():
    uasset.main(['tags'] + sys.argv[1:])


if __name__ == "__main__":
    main()
