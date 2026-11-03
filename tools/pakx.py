#!/usr/bin/env python3
"""usage: pakx.py <file.pak> <out-dir> <path-substring>... [--oodle <oo2core_9_win64.dll>]
The old name of `pak.py extract`, kept for the commands and notes that cite it."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pak  # noqa: E402


def main():
    pak.main(["extract"] + sys.argv[1:])


if __name__ == "__main__":
    main()
