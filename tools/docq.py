"""docq: ask what AssetGen can do, and keep the answer honest.

REFERENCE.md is the capability registry: every row of a `| You write | What it does | Status |` table is one construct,
and its first cell opens with an id anchor, `<a id="switch.on-int"></a>`. The suite says what proves each row: a test
calls `proves('switch.on-int', ...)` (test_bytecode.py) beside the check that exercises it. This tool reads both.

    python tools/docq.py switch jump table   rows whose id, spelling, text or section hold every word, with the tests
                                             that prove each and where the compiler prints a refusal's message; then
                                             matching notes, and the private journal and plans where they exist
    python tools/docq.py switch.switch-code  one row by id
    python tools/docq.py --check             fail on: a row with no id, a duplicate id, a proves() of an unknown id, a
                                             row neither proved nor in unproved.txt, an unproved.txt entry that is now
                                             proved or gone, a markdown link to a missing anchor
    python tools/docq.py --ids               give every row that has none an id (run after adding rows)
    python tools/docq.py --stats             proved / unproved rows per section

The suite runs --check at its end. unproved.txt only shrinks: a new row lands with its test.
"""
import os, re, sys, glob, collections

TOOLS = os.path.dirname(os.path.abspath(__file__))
AG = os.path.dirname(TOOLS)
REPO = os.path.dirname(AG)
REFERENCE = os.path.join(AG, 'REFERENCE.md')
SUITE = os.path.join(TOOLS, 'test_bytecode.py')
UNPROVED = os.path.join(TOOLS, 'unproved.txt')
MEMORY = os.path.expandvars(r'%USERPROFILE%\.claude\projects\D--Repos-CppModding-ue-mods\memory')
ANCHOR = re.compile(r'^<a id="([^"]+)"></a>\s*')
HEADER = ['You write', 'What it does', 'Status']

Row = collections.namedtuple('Row', 'id line section spelling does status')


def read(path):
    with open(path, encoding='utf-8') as f: return f.read().splitlines()


def cells(line):
    # a `|` inside a code span (`A | B`) is not a cell boundary
    out, cur, code = [], '', False
    for ch in line.strip()[1:-1]:
        if ch == '`': code = not code
        if ch == '|' and not code: out.append(cur.strip()); cur = ''
        else: cur += ch
    return out + [cur.strip()]


def rows(lines=None):
    """Every capability row of REFERENCE.md, in file order."""
    lines = lines if lines is not None else read(REFERENCE)
    out, section, table = [], '', False
    for i, l in enumerate(lines, 1):
        if l.startswith('#'): section = l.lstrip('# ').strip()
        if not l.startswith('|'): table = False; continue
        c = cells(l)
        if c == HEADER: table = True; continue
        if not table or set(l) <= set('|-: '): continue
        m = ANCHOR.match(c[0])
        out.append(Row(m.group(1) if m else None, i, section, ANCHOR.sub('', c[0]), c[1], c[-1]))
    return out


def slug(text, words, n):
    """The first `words` words of `text`, lower case, cut at a word boundary within n characters."""
    out = ''
    for w in re.findall(r'[a-z0-9]+', text.lower().replace('_', ' '))[:words]:
        if out and len(out) + 1 + len(w) > n: break
        out = (out + '-' + w).strip('-')
    return out


def assign_ids():
    """Give each row without an id one made of its section and its first code span; existing ids never change."""
    lines = read(REFERENCE)
    have = {r.id for r in rows(lines) if r.id}
    added = 0
    for r in rows(lines):
        if r.id: continue
        span = re.search(r'`([^`]+)`', r.spelling)
        base = '%s.%s' % (slug(r.section, 3, 24), slug(span.group(1) if span else r.spelling, 5, 32) or 'row')
        rid, n = base, 2
        while rid in have: rid, n = '%s-%d' % (base, n), n + 1
        have.add(rid)
        lines[r.line - 1] = lines[r.line - 1].replace('| ', '| <a id="%s"></a>' % rid, 1)
        added += 1
    with open(REFERENCE, 'rb') as f: eol = '\r\n' if b'\r\n' in f.read(4096) else '\n'
    with open(REFERENCE, 'w', encoding='utf-8', newline=eol) as f: f.write('\n'.join(lines) + '\n')
    print('gave %d rows an id' % added)


def proofs():
    """{id: [(line, test)]} from every proves(...) in the suite; `test` is the enclosing def, or the next ok line's
    name for a check at module level."""
    lines = read(SUITE)
    out = collections.defaultdict(list)
    for i, l in enumerate(lines):
        if not re.match(r'\s*proves\(', l): continue
        call, j = l, i
        while not call.rstrip().endswith(')') and j + 1 < len(lines): j += 1; call += lines[j]     # a wrapped list
        m = re.match(r'\s*proves\((.*)\)\s*$', call)
        if not m: continue
        test = None
        if l[0].isspace():
            for j in range(i, -1, -1):
                d = re.match(r'def (\w+)', lines[j])
                if d: test = d.group(1); break
        else:
            for j in range(i, min(i + 40, len(lines))):
                ok = re.search(r"print\(['\"]ok  ([^:'\"]+)", lines[j])
                if ok: test = ok.group(1).strip(); break
        for rid in re.findall(r"'([^']+)'", m.group(1)): out[rid].append((i + 1, test or '?'))
    return out


def unproved():
    if not os.path.exists(UNPROVED): return []
    return [l.strip() for l in read(UNPROVED) if l.strip() and not l.startswith('#')]


def github_slug(heading):
    return re.sub(r'[^\w\- ]', '', heading.strip().lower()).replace(' ', '-')


def anchors(path, cache={}):
    if path not in cache:
        found = set()
        for l in read(path):
            if l.startswith('#'): found.add(github_slug(l.lstrip('#')))
            found.update(re.findall(r'<a id="([^"]+)"></a>', l))
        cache[path] = found
    return cache[path]


def doc_files():
    """The markdown whose links are checked: ue-mods and the agents' memory dir, or, in the public AssetGen repo (no
    ue-mods around it), AssetGen alone."""
    in_ue_mods = os.path.exists(os.path.join(REPO, 'CAPABILITIES.md'))
    skip = re.compile(r'[\\/](SDK|vcpkg_installed|x64|build|node_modules|UeApi|UeAssets|\.git)[\\/]', re.I)
    files = [p for p in glob.glob(os.path.join(REPO if in_ue_mods else AG, '**', '*.md'), recursive=True) if not skip.search(p)]
    return files + (glob.glob(os.path.join(MEMORY, '**', '*.md'), recursive=True) if in_ue_mods else [])


def broken_links():
    """Markdown links `file.md#fragment` / `#fragment` whose target has no such heading or anchor."""
    bad = []
    for path in doc_files():
        for i, l in enumerate(read(path), 1):
            for target, frag in re.findall(r'\]\(([^)\s#]*\.md|)#([^)\s]+)\)', l):
                dest = os.path.normpath(os.path.join(os.path.dirname(path), target)) if target else path
                if os.path.exists(dest) and frag not in anchors(dest):
                    bad.append('%s:%d: no anchor #%s in %s' % (os.path.relpath(path, REPO), i, frag, os.path.basename(dest)))
    return bad


def check():
    """Every rule docq enforces; returns the failures."""
    rs, pr, un = rows(), proofs(), unproved()
    ids = [r.id for r in rs if r.id]
    errs = ['REFERENCE.md:%d: row has no id (run tools/docq.py --ids)' % r.line for r in rs if not r.id]
    errs += ['duplicate id %s' % i for i, n in collections.Counter(ids).items() if n > 1]
    known = set(ids)
    for rid, where in pr.items():
        if rid not in known: errs.append('test_bytecode.py:%d: proves(%r), which no REFERENCE row has' % (where[0][0], rid))
    for rid in un:
        if rid not in known: errs.append('unproved.txt: %s is not a REFERENCE row any more; delete the line' % rid)
        elif rid in pr: errs.append('unproved.txt: %s is proved now (test_bytecode.py:%d); delete the line' % (rid, pr[rid][0][0]))
    listed = set(un)
    for r in rs:
        if r.id and r.id not in pr and r.id not in listed:
            errs.append('REFERENCE.md:%d: %s has no proves() in the suite; add the test (unproved.txt only shrinks)' % (r.line, r.id))
    return errs + broken_links()


def where_refused(row):
    """For a refused or warning row, the compiler source lines that print a message the row quotes."""
    if not re.match(r'(Refused|Warns|Not yet)', row.status): return []
    quoted = [q for q in re.findall(r'\("([^"]{12,})"\)|\(`([^`]{12,})`\)', row.does) for q in q if q]
    hits = []
    for src in glob.glob(os.path.join(AG, '*.cpp')):
        text = read(src)
        for q in quoted:
            needle = re.split(r'<[^>]+>|\.\.\.', q)[0].strip()[:40]
            for i, l in enumerate(text, 1):
                if len(needle) >= 12 and needle in l: hits.append('%s:%d' % (os.path.basename(src), i)); break
    return hits


def show(r, pr):
    print('%s  [%s]  REFERENCE.md:%d  (%s)' % (r.id, r.status, r.line, r.section))
    print('  you write: %s' % r.spelling)
    print('  does:      %s' % (r.does if len(r.does) < 400 else r.does[:400] + ' ...'))
    tests = pr.get(r.id)
    print('  proved by: %s' % (', '.join('test_bytecode.py:%d (%s)' % t for t in tests) if tests else 'no test yet (listed in tools/unproved.txt)'))
    for h in where_refused(r): print('  message:   %s' % h)
    print()


def query(words):
    rs, pr = rows(), proofs()
    ws = [w.lower() for w in words]
    hit = [r for r in rs if r.id == words[0]] if len(words) == 1 else []
    hit = hit or [r for r in rs if all(w in ' '.join((r.id or '', r.spelling, r.does, r.section)).lower() for w in ws)]
    for r in hit[:40]: show(r, pr)
    if len(hit) > 40: print('... %d more rows; add a word to narrow it\n' % (len(hit) - 40))
    if not hit: print('no REFERENCE row matches %s\n' % ' '.join(words))
    section, notes = '', []
    for i, l in enumerate(read(REFERENCE), 1):
        if l.startswith('#'): section = l.lstrip('# ').strip()
        elif l.strip() and not l.startswith('|') and all(w in (section + ' ' + l).lower() for w in ws):
            notes.append((i, section, l.strip()))
    if notes:
        print('REFERENCE.md notes (current, but no row of their own, so no proof is checked):')
        for i, s, l in notes[:8]: print('  %d (%s): %s' % (i, s, l[:200]))
        if len(notes) > 8: print('  ... %d more' % (len(notes) - 8))
    for name, label in (('DESIGN.md', 'journal: why and what was tried; history, not current behaviour'),
                        (os.path.join('..', 'TODO.md'), 'plans: not built yet')):
        path = os.path.join(AG, name)
        if not os.path.exists(path): continue      # the public AssetGen repo has neither
        found =[(i, l) for i, l in enumerate(read(path), 1) if all(w in l.lower() for w in ws)]
        if found:
            print('%s (%s):' % (os.path.basename(name), label))
            for i, l in found[:8]: print('  %d: %s' % (i, l.strip()[:160]))
            if len(found) > 8: print('  ... %d more' % (len(found) - 8))


def stats():
    rs, pr = rows(), proofs()
    by = collections.OrderedDict()
    for r in rs: by.setdefault(r.section, [0, 0])[r.id in pr] += 1
    for s, (no, yes) in by.items(): print('%4d/%-4d %s' % (yes, yes + no, s))
    total = sum(r.id in pr for r in rs)
    print('%d of %d rows proved' % (total, len(rs)))


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a: print(__doc__); sys.exit(0)
    if a[0] == '--check':
        errs = check()
        for e in errs: print(e)
        print('docq: %d problem(s)' % len(errs) if errs else 'docq: ok, %d rows, %d proved' % (len(rows()), len(proofs())))
        sys.exit(1 if errs else 0)
    if a[0] == '--ids': assign_ids()
    elif a[0] == '--stats': stats()
    else: query(a)
