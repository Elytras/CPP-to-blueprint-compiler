#!/usr/bin/env python3
"""The pak toolkit: where UE 4.27, UnrealPak and repak are on this machine, a legacy .pak's footer and index (its mount
point and file list), extraction (UnrealPak or repak, or this module's own reader for the v11 paks UE 5 writes) and
packing. bpbuild.py, genueassets.py, pak.py and the repo's tools/ (crash_triage.py, widgetviewer) go through it."""
import ctypes
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

# Off Windows, a `C:/...` path is one on that Windows drive as this machine mounts it (a dual boot's drives):
# BPBUILD_DRIVES="C=/mnt/c;D=/media/me/Data" names the mounts, else the first of /mnt/<x>, /mnt/win<x> that exists.
DRIVES = dict((d.strip().upper(), root.strip()) for d, _eq, root in
              (e.partition("=") for e in os.environ.get("BPBUILD_DRIVES", "").split(";") if "=" in e))
WINDOWS_PATH = re.compile(r"^([A-Za-z]):[\\/]")


def drive_path(path):
    """Off Windows, Windows drive path `path` on that drive's mount (DRIVES), or None when the drive is not mounted;
    any other path as it is."""
    m = WINDOWS_PATH.match(path) if os.name != "nt" else None
    if not m:
        return path
    drive = m.group(1).upper()
    root = DRIVES.get(drive) or next((r for r in ("/mnt/" + drive.lower(), "/mnt/win" + drive.lower())
                                      if os.path.isdir(r)), None)
    return os.path.join(root, *re.split(r"[\\/]+", path[3:])) if root else None


# UE 4.27's default install, whose Engine folder holds UnrealPak, the editor and AutomationTool.
# UnrealPak: UNREALPAK, else that install's. Off Windows, a Linux UnrealPak on PATH, else the default install's
# UnrealPak.exe on the mounted Windows drive; an .exe there runs under Wine. With none of those, repak
# (github.com/trumank/repak, what MINT packs with) on PATH packs and extracts instead; UNREALPAK=<repak> picks it.
ENGINE = r"C:\Program Files\Epic Games\UE_4.27\Engine"
UNREALPAK_DEFAULT = ENGINE + r"\Binaries\Win64\UnrealPak.exe"
UNREALPAK = os.environ.get("UNREALPAK") or (UNREALPAK_DEFAULT if os.name == "nt" else
                                            shutil.which("UnrealPak") or drive_path(UNREALPAK_DEFAULT) or "UnrealPak")
UNREALPAK_RUN = ([shutil.which("wine") or "wine", UNREALPAK]
                 if os.name != "nt" and UNREALPAK.lower().endswith(".exe") else [UNREALPAK])
REPAK = (UNREALPAK if os.path.basename(UNREALPAK).lower().startswith("repak") else
         shutil.which("repak") if not os.environ.get("UNREALPAK") and not os.path.exists(UNREALPAK) else None)

MAGIC = struct.pack("<I", 0x5A6F12E1)


class PakError(Exception):
    """A pak this module cannot read: no footer, an encrypted index, no full directory index."""


def fstring(b, o):
    """An FString in a pak index: (text, the offset past it), trailing NULs dropped."""
    n = struct.unpack_from("<i", b, o)[0]
    o += 4
    if n < 0:
        return b[o:o - 2 * n].decode("utf-16-le").rstrip("\0"), o - 2 * n
    return b[o:o + n].decode("utf-8", "replace").rstrip("\0"), o + n


def footer(f):
    """(version, index offset, index size, compression method names) of an open legacy .pak. The footer is found by
    its magic, as its size differs between pak versions."""
    f.seek(0, 2)
    f.seek(max(0, f.tell() - 300))
    tail = f.read()
    at = tail.rfind(MAGIC)
    if at < 1:
        raise PakError("no footer magic")
    if tail[at - 1]:
        raise PakError("encrypted index")
    version, index_offset, index_size = struct.unpack_from("<iqq", tail, at + 4)
    methods = [tail[at + 44 + 32 * i:at + 76 + 32 * i].rstrip(b"\0").decode("latin-1") for i in range(5)]
    return version, index_offset, index_size, methods


def mount(pak):
    """The mount point of a legacy .pak with an unencrypted index ("../../../" for most): the FString the index starts
    with."""
    with open(pak, "rb") as f:
        try:
            _version, index_offset, _size, _methods = footer(f)
        except PakError as e:
            sys.exit("%s is not a .pak this build reads (%s)" % (pak, e))
        f.seek(index_offset)
        n = struct.unpack("<i", f.read(4))[0]
        raw = f.read(-2 * n if n < 0 else n)
    return (raw.decode("utf-16-le") if n < 0 else raw.decode("utf-8", "replace")).rstrip("\0").replace("\\", "/")


def index(f):
    """An open pak's file entries as (path, encoded entry offset), each path its mount point plus its directory and
    name ("../../../FSD/Content/A.uasset"), and the encoded entries and compression methods to read them with. v10+
    reads the full directory index; older versions are scanned for their length-prefixed names, which is what their
    index holds, and give no entry offsets."""
    version, index_offset, index_size, methods = footer(f)
    f.seek(index_offset)
    idx = f.read(index_size)
    mnt, p = fstring(idx, 0)
    mnt = mnt.replace("\\", "/")
    if version < 10:
        names = [m.group(1).decode("utf-8", "replace") for m in
                 re.finditer(rb"[\x05-\xff]\x00\x00\x00([ -~]{4,}?\.\w{3,6})\x00", idx[p:])]
        return [(mnt + n, None) for n in names], b"", methods
    p += 4 + 8 + 4                                          # NumEntries, PathHashSeed, bReaderHasPathHashIndex
    if struct.unpack_from("<I", idx, p - 4)[0]:
        p += 8 + 8 + 20                                     # its offset, size, hash
    p += 4                                                  # bReaderHasFullDirectoryIndex
    if not struct.unpack_from("<I", idx, p - 4)[0]:
        raise PakError("no full directory index")
    fdi_offset, fdi_size = struct.unpack_from("<qq", idx, p)
    p += 8 + 8 + 20
    encoded_size = struct.unpack_from("<i", idx, p)[0]
    encoded = idx[p + 4:p + 4 + encoded_size]
    f.seek(fdi_offset)
    fdi = f.read(fdi_size)
    out, q = [], 4
    for _ in range(struct.unpack_from("<i", fdi, 0)[0]):
        folder, q = fstring(fdi, q)
        n, q = struct.unpack_from("<i", fdi, q)[0], q + 4
        for _ in range(n):
            name, q = fstring(fdi, q)
            loc, q = struct.unpack_from("<i", fdi, q)[0], q + 4
            out.append((mnt + folder.lstrip("/") + name, loc))
    return out, encoded, methods


def files(f):
    """Every file path in an open legacy .pak, mount point included. Raises PakError for a pak it cannot list."""
    return [path for path, _loc in index(f)[0]]


def decode(enc, p):
    """One FPakEntry from the encoded entry bytes at p (FPakFile::DecodePakEntry): (offset, size, method, blocks, block
    size)."""
    v = struct.unpack_from('<I', enc, p)[0]; p += 4
    method, blocks = (v >> 23) & 0x3f, (v >> 6) & 0xffff
    if (v & 0x3f) == 0x3f: bsize = struct.unpack_from('<I', enc, p)[0]; p += 4
    else: bsize = (v & 0x3f) << 11
    def num(safe):
        nonlocal p
        r = struct.unpack_from('<I' if safe else '<Q', enc, p)[0]; p += 4 if safe else 8
        return r
    off, usize = num(v & 1 << 31), num(v & 1 << 30)
    csize = num(v & 1 << 29) if method else usize
    # Block offsets are relative to the entry, past the FPakEntry header the data starts with.
    hdr = 8 + 8 + 8 + 4 + 20 + (4 + 16 * blocks if method else 0) + 1 + 4
    blk = []
    if blocks == 1 and not (v >> 22) & 1:
        blk = [(hdr, hdr + csize)]
    elif blocks:
        s = hdr
        for _ in range(blocks):
            n = struct.unpack_from('<I', enc, p)[0]; p += 4
            blk.append((s, s + n)); s += n
    return off, usize, method, blk, bsize or usize      # a single-block entry encodes block size 0


def extract(pak, out, needles, oodle_path='oo2core_9_win64.dll'):
    """Writes every file of a v11 .pak as UE 5 writes it (encoded entries, full directory index, unencrypted index)
    whose path contains one of the needles under out, and yields (path, size, compression) for each. UE 4.27's
    UnrealPak lists such a pak but refuses to extract it ("PakEntry mismatch"). Oodle entries need
    oo2core_9_win64.dll (MINT and most UE5 games ship one)."""
    oodle = None
    with open(pak, 'rb') as f:
        ver = footer(f)[0]
        if ver != 11: sys.exit(f'pak version {ver}: only 11 is read')
        entries, enc, methods = index(f)
        for path, loc in entries:
            if loc < 0 or not any(n in path for n in needles): continue
            off, usize, method, blk, bsize = decode(enc, loc)
            m = methods[method - 1] if method else 'stored'
            if not method:
                f.seek(off + 53); data = f.read(usize)
            else:
                data = b''
                for a, b in blk:
                    f.seek(off + a); c = f.read(b - a)
                    if m.lower() == 'zlib': data += zlib.decompress(c)
                    elif m == 'Oodle':
                        if oodle is None:
                            oodle = ctypes.WinDLL(oodle_path)
                            oodle.OodleLZ_Decompress.restype = ctypes.c_int64
                        want = min(bsize, usize - len(data))
                        buf = ctypes.create_string_buffer(want)
                        got = oodle.OodleLZ_Decompress(c, len(c), buf, want, 1, 0, 0, None, 0, None, None, None, 0, 3)
                        if got != want: sys.exit(f'{path}: Oodle gave {got} of {want} bytes')
                        data += buf.raw
                    else: sys.exit(f'{path}: unsupported compression {m}')
            dst = os.path.join(out, path.replace('../', '').lstrip('/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as w: w.write(data[:usize])
            yield path, usize, m


def unpack(pak, dest, filter=None):
    """UnrealPak's (or repak's) extraction of pak into dest, paths relative to the pak's mount point: the
    CompletedProcess, output captured as text. filter is UnrealPak's -Filter; repak extracts everything."""
    if REPAK:
        cmd = [REPAK, "unpack", pak, "-o", dest, "-f", "-q"]
    else:
        cmd = UNREALPAK_RUN + [pak, "-Extract", dest] + (["-Filter=" + filter] if filter else [])
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")


def unrealpak(*args):
    """UnrealPak run on args, output captured as text: for a response-file pack (-create=)."""
    return subprocess.run(UNREALPAK_RUN + list(args), capture_output=True, text=True)


def pack(src, pak_path, mount_point):
    """Packs folder src into pak_path, mounted at mount_point ("../../../FSD/"), as UE 4.27's UnrealPak writes it:
    its pak version, zlib. Prints why when it cannot; returns whether it packed."""
    if REPAK:
        if os.path.exists(pak_path):
            os.remove(pak_path)
        proc = subprocess.run([REPAK, "pack", "--version", "V11", "--compression", "Zlib", "-m", mount_point,
                               src, pak_path, "-q"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if proc.returncode != 0:
            print("  repak failed (%d):" % proc.returncode)
            print("    " + "\n    ".join(proc.stdout.strip().splitlines()[-6:]))
            return False
        return True
    if not os.path.exists(UNREALPAK):
        print("  UnrealPak not found at %s - skipping the pak." % UNREALPAK)
        return False

    response = os.path.join(os.path.dirname(src), "autogen.txt")
    io.open(response, "w", encoding="utf-8", newline="\n").write(
        '"%s" "%s*.*"\n' % (os.path.join(src, "*.*"), mount_point.replace("/", "\\")))

    cmd = UNREALPAK_RUN + [pak_path, "-platform=Windows", "-create=" + response, "-compress"]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        print("  UnrealPak failed (%d):" % proc.returncode)
        print("    " + "\n    ".join(proc.stdout.strip().splitlines()[-6:]))
        return False
    return True
