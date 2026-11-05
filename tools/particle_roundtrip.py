#!/usr/bin/env python3
"""usage: particle_roundtrip.py <Content dir> <work dir> [--root /Game/Path]... [--only P_Name]... [--assetgen exe]
                             [--ueapi dir] [--jobs N]

The oracle for Cascade particle systems in AssetGen: each cooked UParticleSystem under the roots (DRG's 972 P_*) is
decompiled by bpdecomp to UE_ASSET_DEFAULTS source, compiled again by AssetGen, and the two packages are compared by
meaning (diffs()). Prints one line per package that differs or fails, then a tally; writes <work>/report.txt.

The suite uses the same pieces on its own particle systems (particle_decompile in test_bytecode.py)."""
import argparse, os, re, shutil, struct, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
AG = os.path.dirname(HERE)
REPO = os.path.dirname(AG)
sys.path.insert(0, HERE)
import bpdecomp
import invariants


def layout(work, ueapi):
    """<work>/BpMods/UeApi and <work>/AssetGen/include, which a decompiled mod's umbrella includes by relative path,
    as hard links where the volume allows (copies elsewhere). Never a junction: deleting the work folder would follow
    it into the real UeApi."""
    for src, dst in ((ueapi, os.path.join(work, 'BpMods', 'UeApi')), (os.path.join(AG, 'include'), os.path.join(work, 'AssetGen', 'include'))):
        if os.path.isdir(dst): continue
        for r, _, fs in os.walk(src):
            to = os.path.join(dst, os.path.relpath(r, src))
            os.makedirs(to, exist_ok=True)
            for f in fs:
                try: os.link(os.path.join(r, f), os.path.join(to, f))
                except OSError: shutil.copy2(os.path.join(r, f), os.path.join(to, f))


def decompile(content, root, mod, work, ueapi):
    """bpdecomp of the particle systems under root into <work>/BpMods/<mod>; the finished process."""
    return subprocess.run(
        [sys.executable, '-I', '-c',
         'import sys; sys.path.insert(0, %r); import bpdecomp_model; '
         'bpdecomp_model.Api.__init__.__defaults__ = (%r,); import bpdecomp_emit; bpdecomp_emit.main()'
         % (HERE, os.path.abspath(ueapi)), content, os.path.join(work, 'BpMods'), '--mod', '%s=%s' % (root, mod),
         '--kinds', 'particles'], capture_output=True, text=True, encoding='utf-8', errors='replace')


def compile_mod(work, mod, root, assetgen):
    """AssetGen's compile of the decompiled mod into <work>/out/<mod>/FSD/Content/<root>, against the UeApi layout()
    put beside it (the umbrella reaches it by relative path: another copy would declare everything twice); (process,
    out folder)."""
    out = os.path.join(work, 'out', mod, 'FSD', 'Content', *root.split('/')[2:])
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    proc = subprocess.run([assetgen, 'compile', os.path.join(work, 'BpMods', mod, mod + '.cpp'), os.path.join(work, 'BpMods', 'UeApi'), out],
                          capture_output=True, text=True, encoding='utf-8', errors='replace')
    return proc, out


# ---- comparing by meaning

def values(pkg, i):
    """Export i's tags as {name: value}: numbers, bools, names, object paths (None for null), arrays, structs as dicts.
    Names are lowercased: an FName compares without case, and the runtime name pool UeApi is dumped from can spell one
    otherwise than the editor that cooked the package (Lifetime, LifeTime)."""
    return {t['name'].lower(): value(pkg, t) for t in pkg.tags(i)}


V = bpdecomp.Values(None)
FIXED = {'Vector': '<3f', 'Vector2D': '<2f', 'Vector4': '<4f', 'Rotator': '<3f', 'LinearColor': '<4f', 'Quat': '<4f',
         'TwoVectors': '<6f', 'Box': '<6fB', 'Color': '<4B', 'Guid': '<4I', 'IntPoint': '<2i', 'IntVector': '<3i'}


def value(pkg, t):
    return item(pkg, t['type'], bytes(t['value']), 0, t)[0]


def item(pkg, ty, b, o, t=None, sname=None):
    """(value, offset after it) of one ty at b[o:]: a tag's value (t given) or a container's element."""
    if ty == 'FloatProperty': return struct.unpack_from('<f', b, o)[0], o + 4
    if ty in ('IntProperty', 'UInt32Property'): return struct.unpack_from('<i' if ty == 'IntProperty' else '<I', b, o)[0], o + 4
    if ty == 'BoolProperty': return (bool(t['bool']), o) if t else (bool(b[o]), o + 1)
    if ty in ('ObjectProperty', 'ClassProperty'):
        r = struct.unpack_from('<i', b, o)[0]
        return (pkg.path(r) if r else None), o + 4
    if ty == 'NameProperty' or (ty in ('ByteProperty', 'EnumProperty') and (len(b) - o >= 8 if t is None else t['size'] == 8)):
        return V.name_at(pkg, b, o).split('::')[-1], o + 8
    if ty == 'ByteProperty': return b[o], o + 1
    if ty == 'StructProperty':
        sname = t['struct'] if t else sname
        if sname in FIXED:
            n = struct.calcsize(FIXED[sname])
            return struct.unpack_from(FIXED[sname], b, o), o + n
        tags, o = V.read_tags(pkg, b, o)
        out = {x['name'].lower(): value(pkg, x) for x in tags}
        if sname == 'Transform':        # a member left out holds the identity's
            out = dict({'rotation': (0.0, 0.0, 0.0, 1.0), 'translation': (0.0, 0.0, 0.0), 'scale3d': (1.0, 1.0, 1.0)}, **out)
        return out, o
    if ty == 'ArrayProperty':
        n = struct.unpack_from('<i', b, o)[0]; o += 4
        inner, sn = t['inner'], None
        if inner == 'StructProperty' and n:
            tl = V.read_tag(pkg, b, o)          # an array of structs leads with one tag for its inner property
            sn, o = tl['struct'], tl['next'] - tl['size']
        if inner in ('ByteProperty', 'EnumProperty') and n and (len(b) - o) // n == 8: inner = 'NameProperty'
        out = []
        for _ in range(n):
            v, o = item(pkg, inner, b, o, None, sn)
            out.append(v)
        return out, o
    return ('raw', ty, b[o:]), len(b)


def close(a, b, tol=1e-4):
    if isinstance(a, dict): return isinstance(b, dict) and a.keys() == b.keys() and all(close(a[k], b[k], tol) for k in a)
    if isinstance(a, (list, tuple)) and not isinstance(a, bytes):
        return type(a) == type(b) and len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    if isinstance(a, float) or isinstance(b, float):
        return isinstance(b, (int, float)) and isinstance(a, (int, float)) and abs(a - b) <= tol * max(1.0, abs(a))
    if isinstance(a, str) and isinstance(b, str): return a.lower() == b.lower()     # FNames and paths: no case
    return a == b


def rel(pkg, path):
    """An object path made relative to its package (`P_X:Emitter.LOD`), so two packages' paths compare."""
    if path is None: return None
    top = pkg.path(next(k + 1 for k, e in enumerate(pkg.exports) if not e['outer']))
    return path[len(top):] if path.startswith(top) else path


def norm(pkg, v):
    if isinstance(v, str) and '.' in v and '/' in v: return rel(pkg, v)
    if isinstance(v, dict): return {k: norm(pkg, x) for k, x in v.items()}
    if isinstance(v, list): return [norm(pkg, x) for x in v]
    return v


def tree(pkg):
    """{relative path: export index} of every export but a distribution object nothing points at (DRG's cook keeps a
    baked one's, which nothing reads)."""
    refs = set()
    def walk(v):
        if isinstance(v, str): refs.add(v)
        elif isinstance(v, dict): [walk(x) for x in v.values()]
        elif isinstance(v, list): [walk(x) for x in v]
    vals = {i: values(pkg, i) for i in range(len(pkg.exports))}
    for v in vals.values(): walk(v)
    out = {}
    for i in range(len(pkg.exports)):
        p = pkg.path(i + 1)
        if pkg.class_of(i + 1).startswith('Distribution') and p not in refs: continue
        r = rel(pkg, p)
        out[(r if r else p if r is None else "<the asset>").lower()] = i     # FNames: no case
    return out, vals


def diffs(van, ours):
    """What differs, by meaning, between a cooked particle system and AssetGen's compile of its decompiled source: an
    object either side lacks, a class, a tag. A live distribution the cook kept (it never baked it) that AssetGen
    baked is not a difference when the table AssetGen wrote is the object's own value: reported as `baked`."""
    out = []
    vt, vv = tree(van)
    ot, ov = tree(ours)
    for p in sorted(set(vt) - set(ot)):
        if van.class_of(vt[p] + 1).startswith('Distribution'): continue      # baked by AssetGen: checked at its owner
        out.append('missing %s (%s)' % (p, van.class_of(vt[p] + 1)))
    for p in sorted(set(ot) - set(vt)): out.append('extra %s (%s)' % (p, ours.class_of(ot[p] + 1)))
    for p in sorted(set(vt) & set(ot)):
        i, j = vt[p], ot[p]
        if van.class_of(i + 1) != ours.class_of(j + 1):
            out.append('%s: class %s, ours %s' % (p, van.class_of(i + 1), ours.class_of(j + 1)))
            continue
        a, b = norm(van, vv[i]), norm(ours, ov[j])
        for k in sorted(set(a) | set(b)):
            x, y = a.get(k), b.get(k)
            if close(x, y): continue
            # read only by the editor (its LOD fixups are WITH_EDITOR); AssetGen derives it, and vanilla keeps stale bits
            if k == 'lodvalidity': continue
            if isinstance(x, dict) and isinstance(y, dict) and 'table' in y and x.get('distribution') and not y.get('distribution'):
                out.append('baked %s.%s (the cook kept %s live)' % (p, k, x['distribution']))
                continue
            out.append('%s.%s: %r, ours %r' % (p, k, short(x), short(y)))
    return out


def short(v):
    s = repr(v)
    return s if len(s) < 160 else s[:157] + '...'


# ---- the vanilla run

def one(args, path):
    """Round trip of one package (its /Game path): ('ok' | 'differs' | 'decompile' | 'compile', lines)."""
    folder, name = path.rsplit('/', 1)
    mod = 'RT_' + ''.join(c if c.isalnum() else '_' for c in name)
    work = os.path.join(args.work, mod)
    shutil.rmtree(work, ignore_errors=True)
    layout(work, args.ueapi)
    # the package alone under its own folder, so the mod holds just it
    content = os.path.join(work, 'in', 'Content')
    src = os.path.join(args.content, *path.split('/')[2:])
    dst = os.path.join(content, *path.split('/')[2:])
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    for ext in ('.uasset', '.uexp'):
        if os.path.exists(src + ext): shutil.copy2(src + ext, dst + ext)
    proc = decompile(content, folder, mod, work, args.ueapi)
    if proc.returncode or not os.path.exists(os.path.join(work, 'BpMods', mod, mod + '.cpp')):
        return 'decompile', [(proc.stdout + proc.stderr)[-600:]]
    notes = open(os.path.join(work, 'BpMods', 'NOTES.md'), encoding='utf-8').read().splitlines()[3:]
    proc, out = compile_mod(work, mod, folder, args.assetgen)
    if proc.returncode:
        text = (proc.stdout + proc.stderr).splitlines()
        errs = [l.split(' error: ', 1)[1] for l in text if ' error: ' in l]
        return 'compile', sorted(set(errs))[:6] or [l for l in text if 'FAILED' in l][:2] or text[-6:]
    # an asset name C++ cannot spell (P_Tri-Jaw_...) cooks under bpdecomp's respelling: assets have no __UeName marker
    spelled = re.sub(r'\W', '_', name)
    renamed = spelled != name and not os.path.exists(os.path.join(out, name + '.uasset'))
    d = diffs(invariants.Package(src), invariants.Package(os.path.join(out, spelled if renamed else name)))
    if renamed: notes.append('%s: cooks as %s, a name C++ can spell (no __UeName marker for an asset)' % (path, spelled))
    hard = [x for x in d if not x.startswith('baked ') and not (renamed and x.endswith('(ParticleSystem)'))]
    if not args.keep: shutil.rmtree(work, ignore_errors=True)
    # a member bpdecomp said it could not write (a C array) is a known gap, not a difference
    noted = {m.group(1).lower() for n in notes for m in [re.search(r': default (\w+) not rendered', n)] if m}
    unexplained = [x for x in hard if x.split(': ', 1)[0].rsplit('.', 1)[-1] not in noted]
    verdict = 'differs' if unexplained else 'gap' if hard or renamed else 'ok'
    return verdict, hard + [n for n in notes if n.strip()] + [x for x in d if x.startswith('baked ')]


def guarded(args, path):
    """one(), with a failure of the tool itself reported as that package's `error` instead of ending the run."""
    try: return one(args, path)
    except Exception as e: return 'error', ['%s: %s' % (type(e).__name__, e)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('content'); ap.add_argument('work')
    ap.add_argument('--root', action='append', default=[]); ap.add_argument('--only', action='append', default=[])
    ap.add_argument('--assetgen', default=os.path.join(REPO, 'x64', 'Release', 'assetgen.exe'))
    ap.add_argument('--ueapi', default=os.path.join(REPO, 'BpMods', 'UeApi'))
    ap.add_argument('--jobs', type=int, default=4); ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()
    # one spelling of every path: a header reached as D:/a and as D: is two files to #pragma once
    args.content, args.work, args.ueapi, args.assetgen = (os.path.abspath(x) for x in (args.content, args.work, args.ueapi, args.assetgen))
    paths = []
    for r, _, fs in os.walk(args.content):
        for f in fs:
            if not (f.startswith('P_') and f.endswith('.uasset')): continue
            path = '/Game/' + os.path.relpath(os.path.join(r, f[:-7]), args.content).replace(os.sep, '/')
            if args.root and not any(path.startswith(x.rstrip('/') + '/') for x in args.root): continue
            if args.only and f[:-7] not in args.only: continue
            try:
                p = invariants.Package(os.path.join(r, f[:-7]))
                if not any(not e['outer'] and p.class_of(k + 1) == 'ParticleSystem' for k, e in enumerate(p.exports)): continue
            except Exception: continue
            paths.append(path)
    os.makedirs(args.work, exist_ok=True)
    tally, lines = {}, []
    with ThreadPoolExecutor(args.jobs) as ex:
        for path, (verdict, detail) in zip(paths, ex.map(lambda p: guarded(args, p), paths)):
            tally[verdict] = tally.get(verdict, 0) + 1
            if verdict != 'ok' or args.only:
                lines.append('%s %s' % (verdict.upper(), path))
                lines += ['    ' + x for x in detail]
    lines.append('%d packages: %s' % (len(paths), ', '.join('%s %d' % kv for kv in sorted(tally.items()))))
    open(os.path.join(args.work, 'report.txt'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    print('\n'.join(lines[-60:]))


if __name__ == '__main__':
    main()
