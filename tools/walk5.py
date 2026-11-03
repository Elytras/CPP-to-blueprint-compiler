#!/usr/bin/env python3
"""usage: walk5.py <base-path-without-ext> [function-name-substring...]
The old name of `uasset.py walk --ue5`, kept for the commands and notes that cite it."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uasset  # noqa: E402


def main():
    uasset.main(['walk', '--ue5'] + sys.argv[1:])


if __name__ == "__main__":
    main()
