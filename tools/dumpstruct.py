#!/usr/bin/env python3
"""usage: dumpstruct.py <base-path-without-ext> <export-index>
The old name of `uasset.py struct`, kept for the commands and notes that cite it."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uasset  # noqa: E402


def main():
    uasset.main(['struct'] + sys.argv[1:])


if __name__ == "__main__":
    main()
