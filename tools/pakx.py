#!/usr/bin/env python3
"""usage: pakx.py <file.pak> <out-dir> <path-substring>... [--oodle <oo2core_9_win64.dll>]
Extracts every file whose pak path contains one of the substrings from a legacy v11 .pak as UE 5 writes it (encoded
entries, full directory index, unencrypted index), which UE 4.27's UnrealPak lists but refuses to extract ("PakEntry
mismatch"). Oodle entries need oo2core_9_win64.dll (MINT and most UE5 games ship one): pass --oodle, or have it on PATH."""
import ctypes, os, struct, sys, zlib

def fstr(b, o):
    n = struct.unpack_from('<i', b, o)[0]; o += 4
    if n < 0: return b[o:o - 2 * n].decode('utf-16-le').rstrip('\0'), o - 2 * n
    return b[o:o + n].rstrip(b'\0').decode('utf-8', 'replace'), o + n

def decode(enc, p):
    """One FPakEntry from the encoded entry bytes at p (FPakFile::DecodePakEntry): (offset, size, method, blocks, block size)."""
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

def main():
    args, oodle_path = sys.argv[1:], 'oo2core_9_win64.dll'
    if '--oodle' in args:
        i = args.index('--oodle'); oodle_path = args[i + 1]; del args[i:i + 2]
    pak, out, needles = args[0], args[1], args[2:]
    oodle = None
    f = open(pak, 'rb'); f.seek(0, 2)
    f.seek(f.tell() - 221); foot = f.read(221)
    ver, idx_off, idx_size = struct.unpack_from('<IQQ', foot, 21)
    if ver != 11: sys.exit(f'pak version {ver}: only 11 is read')
    methods = [foot[61 + 32 * i:93 + 32 * i].rstrip(b'\0').decode() for i in range(5)]
    f.seek(idx_off); idx = f.read(idx_size)
    mount, o = fstr(idx, 0)
    _, _, has_phi = struct.unpack_from('<iQI', idx, o); o += 16
    if has_phi: o += 8 + 8 + 20
    o += 4                                              # bHasFullDirectoryIndex
    fdi_off, fdi_size = struct.unpack_from('<qq', idx, o); o += 8 + 8 + 20
    enc_size = struct.unpack_from('<i', idx, o)[0]; o += 4
    enc = idx[o:o + enc_size]

    f.seek(fdi_off); fdi = f.read(fdi_size)
    o = 4
    for _ in range(struct.unpack_from('<i', fdi, 0)[0]):
        d, o = fstr(fdi, o)
        nf = struct.unpack_from('<i', fdi, o)[0]; o += 4
        for _ in range(nf):
            name, o = fstr(fdi, o)
            loc = struct.unpack_from('<i', fdi, o)[0]; o += 4
            path = mount + d + name
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
            print(path, usize, m)

if __name__ == '__main__':
    main()
