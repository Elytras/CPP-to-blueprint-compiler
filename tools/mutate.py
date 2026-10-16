#!/usr/bin/env python3
"""Mutation testing of the AssetGen suite: does test_bytecode.py fail when the compiler is wrong?

usage: mutate.py apply <work>       copy mutants.json into <work> and write every mutant into the compiler sources as a
                                    switch (below); the sources must have no other changes. Then build AssetGen.
       mutate.py restore <work>     copy that build's assetgen.exe into <work> and put the sources back
       mutate.py run <work> <suite args> [-- id ...]
                                    the suite once per mutant that has no result yet in <work>/results.jsonl (or once for
                                    each id given), with AG_MUTANT naming it. The first run is 'base', the same compiler
                                    with no mutant on, which must pass. 'pristine' runs <work>/pristine.exe, a build
                                    with no switches, so a different output from 'base' shows the switches leak.
       mutate.py report <work>

Each mutant replaces one snippet of mutants.json: an "expr" with `(MUT(n) ? (new) : (old))`, a "stmt" with
`if (MUT(n)) { new } else { old }`. MUT(n) is true only for the mutant AG_MUTANT names, so with none named the switched
compiler is the real one. The first time a mutant's code runs, MUT appends n to the file AG_MUTANT_HITS names. That
splits the outcomes three ways:
  killed     the suite failed, at the check the result names
  unreached  the suite passed and no compile ran the mutated code: no test exercises it
  missed     the suite passed although the mutated code ran; `changed` lists the packages under tests/build whose bytes
             differ from base's, so an empty list means the change did not reach a package the suite keeps
"""
import ctypes, hashlib, json, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
AG = os.path.normpath(os.path.join(HERE, '..'))
REPO = os.path.normpath(os.path.join(AG, '..'))
SUITE = os.path.join(HERE, 'test_bytecode.py')
BUILD = os.path.join(AG, 'tests', 'build')
TIMEOUT_S = 1800            # a full --game --sdk run takes about 6 minutes; a mutant that loops forever is killed
MIN_FREE_MB, MIN_COMMIT_MB = 3000, 4000     # below either, stop: the suite's compiles need it, and the box leaks commit

PRELUDE = r'''#ifndef AG_MUT_DEFINED
#define AG_MUT_DEFINED
#include <cstdio>
#include <cstdlib>
// Written by AssetGen/tools/mutate.py apply, removed by restore: never commit. True only for the mutant AG_MUTANT names.
static bool MUT(int N)
{
#pragma warning(suppress : 4996)
    static const char* const Env = std::getenv("AG_MUTANT");
    static const int Active = Env ? std::atoi(Env) : 0;
    if (N != Active) return false;
    static bool bLogged = false;
    if (!bLogged)
    {
        bLogged = true;
#pragma warning(suppress : 4996)
        const char* Hits = std::getenv("AG_MUTANT_HITS");
#pragma warning(suppress : 4996)
        if (FILE* F = Hits ? std::fopen(Hits, "a") : nullptr) { std::fprintf(F, "%d\n", N); std::fclose(F); }
    }
    return true;
}
#endif
'''


def load(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def sources(cat):
    return sorted({m['file'] for m in cat})


def apply(work):
    cat = load(os.path.join(HERE, 'mutants.json'))
    ids = [m['id'] for m in cat]
    assert len(set(ids)) == len(ids), 'duplicate ids'
    rel = ['AssetGen/' + f for f in sources(cat)]
    if subprocess.run(['git', '-C', REPO, 'diff', '--quiet', '--'] + rel).returncode:
        sys.exit('the compiler sources have changes; commit or restore them first')
    os.makedirs(work, exist_ok=True)
    shutil.copyfile(os.path.join(HERE, 'mutants.json'), os.path.join(work, 'mutants.json'))
    for f in sources(cat):
        path = os.path.join(AG, f)
        with open(path, encoding='utf-8', newline='') as fh:
            text = fh.read()
        edits = []
        for n, m in enumerate(cat, 1):
            if m['file'] != f:
                continue
            at = text.find(m['old'])
            if at < 0 or text.find(m['old'], at + 1) >= 0:
                sys.exit('%s: its old snippet is not in %s exactly once' % (m['id'], f))
            form = {'expr': '(MUT(%d) ? (%s) : (%s))', 'stmt': 'if (MUT(%d)) { %s } else { %s }'}[m['kind']]
            edits.append((at, at + len(m['old']), form % (n, m['new'], m['old']), m['id']))
        edits.sort()
        for a, b in zip(edits, edits[1:]):
            if a[1] > b[0]:
                sys.exit('%s and %s overlap' % (a[3], b[3]))
        for start, end, new, _ in reversed(edits):
            text = text[:start] + new + text[end:]
        bom = '﻿' if text.startswith('﻿') else ''
        with open(path, 'w', encoding='utf-8', newline='') as fh:
            fh.write(bom + PRELUDE + text[len(bom):])
    print('%d mutants switched into %s. Build AssetGen, then: mutate.py restore %s' % (len(cat), ', '.join(sources(cat)), work))


def restore(work):
    cat = load(os.path.join(work, 'mutants.json'))
    exe = os.path.join(REPO, 'x64', 'Release', 'assetgen.exe')
    with open(exe, 'rb') as f:
        switched = b'AG_MUTANT_HITS' in f.read()
    if switched and os.path.getmtime(exe) > os.path.getmtime(os.path.join(work, 'mutants.json')):
        shutil.copyfile(exe, os.path.join(work, 'assetgen.exe'))
        print('saved the switched compiler as', os.path.join(work, 'assetgen.exe'))
    else:
        print('x64/Release/assetgen.exe is no build of these switches; nothing saved')
    subprocess.run(['git', '-C', REPO, 'checkout', '--'] + ['AssetGen/' + f for f in sources(cat)], check=True)


def memory():
    """(free physical MB, free commit MB)."""
    class Status(ctypes.Structure):
        _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong)] + \
                   [(n, ctypes.c_ulonglong) for n in ('total', 'avail', 'pagefile', 'pagefile_avail', 'virtual',
                                                      'virtual_avail', 'extended')]
    s = Status(dwLength=ctypes.sizeof(Status))
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(s))
    return s.avail >> 20, s.pagefile_avail >> 20


def digest(top):
    """relative path -> sha1 of every file under top."""
    out = {}
    for root, _, names in os.walk(top):
        for name in names:
            path = os.path.join(root, name)
            with open(path, 'rb') as f:
                out[os.path.relpath(path, top).replace(os.sep, '/')] = hashlib.sha1(f.read()).hexdigest()
    return out


def results(work):
    """id -> its last result."""
    path = os.path.join(work, 'results.jsonl')
    if not os.path.exists(path):
        return {}
    with open(path, encoding='utf-8') as f:
        return {r['id']: r for r in map(json.loads, f) if r}


def failure(text):
    """Where a failed run stopped: the suite's innermost frame, the line of the check, and the exception's text."""
    out = {}
    frames = [f for f in re.findall(r'File "([^"]+)", line (\d+), in (\S+)', text) if os.path.basename(f[0]) == 'test_bytecode.py']
    if frames:
        line = int(frames[-1][1])
        out['where'] = '%s:%d' % (frames[-1][2], line)
        with open(SUITE, encoding='utf-8') as f:
            out['check'] = f.read().splitlines()[line - 1].strip()[:200]
    tail = [l for l in text.splitlines() if l.strip()]
    out['message'] = tail[-1][:400] if tail else ''
    return out


def run_one(work, args, mid, cat):
    env = dict(os.environ)
    env.pop('AG_MUTANT', None)
    hits = os.path.join(work, 'hits', mid + '.txt')
    log = os.path.join(work, 'logs', mid + '.log')
    for d in (os.path.dirname(hits), os.path.dirname(log), os.path.join(work, 'digests')):
        os.makedirs(d, exist_ok=True)
    if os.path.exists(hits):
        os.remove(hits)
    if mid not in ('base', 'pristine'):
        env['AG_MUTANT'] = str(next(n for n, m in enumerate(cat, 1) if m['id'] == mid))
        env['AG_MUTANT_HITS'] = hits
    exe = os.path.join(work, 'pristine.exe' if mid == 'pristine' else 'assetgen.exe')
    start = time.time()
    with open(log, 'w', encoding='utf-8') as out:
        # Below normal, as the build is: its compiles inherit the class, and the machine stays usable.
        p = subprocess.Popen([sys.executable, SUITE, '--assetgen', exe] + args, stdout=out, stderr=subprocess.STDOUT,
                             env=env, cwd=AG, creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
        try:
            rc = p.wait(TIMEOUT_S)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(p.pid)], capture_output=True)
            p.wait()
            rc = None
    with open(log, encoding='utf-8', errors='replace') as f:
        text = f.read()
    r = {'id': mid, 'seconds': round(time.time() - start), 'oks': len(re.findall(r'(?m)^ok  ', text)),
         'when': time.strftime('%Y-%m-%d %H:%M')}
    if rc is None:
        r['status'] = 'timeout'
    elif rc:
        r['status'] = 'killed'
        r.update(failure(text))
    else:
        mine = digest(BUILD)
        with open(os.path.join(work, 'digests', mid + '.json'), 'w', encoding='utf-8') as f:
            json.dump(mine, f)
        if mid in ('base', 'pristine'):
            r['status'] = 'passed'
        else:
            r['status'] = 'missed' if os.path.exists(hits) else 'unreached'
        base = os.path.join(work, 'digests', 'base.json')
        if mid != 'base' and os.path.exists(base):
            theirs = load(base)
            r['changed'] = sorted(k for k in set(mine) | set(theirs) if mine.get(k) != theirs.get(k))
    if mid not in ('base', 'pristine'):
        r['reached'] = os.path.exists(hits)
    return r


def run(work, args, ids):
    cat = load(os.path.join(work, 'mutants.json'))
    done = results(work)
    todo = ids or [m['id'] for m in cat if m['id'] not in done]
    if 'base' not in done and 'base' not in todo and set(todo) - {'pristine'}:
        todo.insert(0, 'base')
    for mid in todo:
        free, commit = memory()
        if free < MIN_FREE_MB or commit < MIN_COMMIT_MB:
            sys.exit('stopping before %s: %d MB free, %d MB commit left' % (mid, free, commit))
        r = run_one(work, args, mid, cat)
        with open(os.path.join(work, 'results.jsonl'), 'a', encoding='utf-8') as f:
            f.write(json.dumps(r) + '\n')
        print(line(r, cat), flush=True)
        if mid == 'base' and r['status'] != 'passed':
            sys.exit('the baseline does not pass; see ' + os.path.join(work, 'logs', 'base.log'))


def line(r, cat):
    m = next((m for m in cat if m['id'] == r['id']), {})
    head = '%-9s %-9s %5ds %4d ok' % (r['id'], r['status'], r['seconds'], r['oks'])
    if r['status'] == 'killed':
        return '%s  %s  %s' % (head, r.get('where', '-'), (r.get('check') or r.get('message', ''))[:110])
    if 'changed' in r:
        head += '  %d files differ from base' % len(r['changed'])
    return '%s  %s' % (head, m.get('bug', ''))


def report(work):
    cat = load(os.path.join(work, 'mutants.json'))
    done = results(work)
    for mid in ['base', 'pristine'] + [m['id'] for m in cat]:
        if mid in done:
            print(line(done[mid], cat))
    counts = {}
    for m in cat:
        s = done.get(m['id'], {}).get('status', 'not run')
        counts[s] = counts.get(s, 0) + 1
    print(', '.join('%d %s' % (n, s) for s, n in sorted(counts.items())))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, work = sys.argv[1], os.path.abspath(sys.argv[2])
    if cmd == 'apply':
        apply(work)
    elif cmd == 'restore':
        restore(work)
    elif cmd == 'run':
        rest = sys.argv[3:]
        cut = rest.index('--') if '--' in rest else len(rest)
        run(work, rest[:cut], rest[cut + 1:])
    elif cmd == 'report':
        report(work)
    else:
        sys.exit(__doc__)
