#!/usr/bin/env python3
"""usage: pak.py ls <file.pak>
       pak.py extract <file.pak> <out-dir> <path-substring>... [--oodle <oo2core_9_win64.dll>]
       pak.py find <path-substring> <file.pak or folder>...
       pak.py pack <folder> <out.pak> [--mount ../../../FSD/]

  ls       every file in a legacy .pak, its mount point included
  extract  the files whose pak path contains one of the substrings, from a v11 .pak as UE 5 writes it (encoded
           entries, full directory index, unencrypted index), which UE 4.27's UnrealPak lists but refuses to extract.
           Oodle entries need oo2core_9_win64.dll (MINT and most UE5 games ship one): pass --oodle, or have it on PATH
  find     each pak, of those given and the .pak files under each folder, holding a path with the substring
  pack     the folder as UE 4.27's UnrealPak packs it (repak when there is no UnrealPak), mounted at --mount"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pakkit  # noqa: E402


def option(args, flag, default):
    """The value after flag in args, removing both, else default."""
    if flag not in args:
        return default
    i = args.index(flag)
    value = args[i + 1]
    del args[i:i + 2]
    return value


def ls(args):
    with open(args[0], "rb") as f:
        for path in pakkit.files(f):
            print(path)


def extract(args):
    oodle = option(args, "--oodle", "oo2core_9_win64.dll")
    for path, size, method in pakkit.extract(args[0], args[1], args[2:], oodle):
        print(path, size, method)


def find(args):
    needle, paks = args[0], []
    for p in args[1:]:
        paks += sorted(glob.glob(os.path.join(p, "**", "*.pak"), recursive=True)) if os.path.isdir(p) else [p]
    for pak in paks:
        try:
            with open(pak, "rb") as f:
                hits = [path for path in pakkit.files(f) if needle in path]
        except pakkit.PakError as e:
            print("%s: %s" % (pak, e), file=sys.stderr)
            continue
        for path in hits:
            print("%s: %s" % (pak, path))


def pack(args):
    mount = option(args, "--mount", "../../../FSD/")
    if not pakkit.pack(os.path.abspath(args[0]), os.path.abspath(args[1]), mount):
        sys.exit(1)


COMMANDS = {"ls": ls, "extract": extract, "find": find, "pack": pack}


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv or argv[0] not in COMMANDS:
        sys.exit(__doc__)
    COMMANDS[argv[0]](argv[1:])


if __name__ == "__main__":
    main()
