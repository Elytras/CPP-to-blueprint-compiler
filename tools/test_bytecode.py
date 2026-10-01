#!/usr/bin/env python3
"""usage: test_bytecode.py [--assetgen <exe>] [--ueapi <UeApi dir>] [--cases <file>] [--game <folder /Game is in>]
                        [--sdk <Dumper-7 dump of the game>] [--no-prefetch | --check-prefetch]

Compiles every test mod in AssetGen/tests and every example mod in AssetGen/examples, then checks what a mod can
observe: its functions run offline (runscript.py, runvm.py for latent / delegate / cross-object code) against Python
oracles, return values and member writes both, and what the engine reads off the cooked assets (flags, property types,
defaults, references, which function a call reaches). Never the bytecode's shape: an optimization that keeps the behaviour must pass.

--assetgen defaults to the first build found (ue-mods x64/Release, this repo's x64/Release, a CMake build/);
--ueapi to ue-mods' BpMods/UeApi. Outside ue-mods, pass the UeApi of https://github.com/Elytras/DRG-Blueprint-Cpp-SDK.
--cases also writes each offline run as a JSON case, which ue-mods' `bpcheck` command replays in the running game.
--game (the extracted game pak's FSD/Content) adds the S38 edits of the game's own packages; without it they are skipped.
The compiles the tests make one at a time after build() are prefetched: the last run's list of them, in
assetgen-suite-prefetch.json in the temp folder (one for every checkout on the machine), runs on as many threads as
build() uses, each in a staging folder, and a test whose compile is ready takes the result (see assetgen_compile).
The line before the last says how many were. --no-prefetch compiles each one when the test asks, and still writes the
list; --check-prefetch also compiles every prefetched one in place and stops the run on any difference."""
import atexit, copy, glob, hashlib, itertools, json, os, posixpath, re, shutil, subprocess, sys, tempfile, threading, time
os.environ['PYTHONIOENCODING'] = 'utf-8'   # the dump tools print non-ASCII names; read back as UTF-8, not the code page
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runscript
from runscript import run, i32
from runvm import VM, Obj, latent_call
import dumpexp

HERE = os.path.dirname(os.path.abspath(__file__))
AG = os.path.normpath(os.path.join(HERE, '..'))
TESTS = os.path.join(AG, 'tests')
ROOT = os.path.join(TESTS, 'build')


def option(flag, candidates):
    if flag in sys.argv:
        return os.path.abspath(sys.argv[sys.argv.index(flag) + 1])   # Windows won't run a relative x64/Release/assetgen.exe
    return next((c for c in candidates if os.path.exists(c)), None)


ASSETGEN = option('--assetgen', [os.path.join(AG, '..', 'x64', 'Release', 'assetgen.exe'),
                                 os.path.join(AG, 'x64', 'Release', 'assetgen.exe'), os.path.join(AG, 'build', 'assetgen')])
UEAPI = option('--ueapi', [os.path.join(AG, '..', 'BpMods', 'UeApi')])
GAME = option('--game', [])
SDK = option('--sdk', [])       # invariants.py's rules read the engine's own classes off it; without, they skip them
os.environ['INVARIANTS_UEAPI'] = UEAPI or ''
if SDK: os.environ['INVARIANTS_SDK'] = SDK
if not ASSETGEN or not UEAPI:
    sys.exit(__doc__)


LOGS = {}      # what each test's compile printed
WORKERS = max(1, (os.cpu_count() or 2) // 2)    # a compile or a rule run is one busy core; leave the rest to the machine


def parallel(fn, items):
    """fn over items on WORKERS threads, results in items' order. Each fn runs a subprocess, which frees the GIL."""
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(WORKERS) as pool:
        return list(pool.map(fn, items))


COMPILE_MB, RESERVE_MB = 1024, 2048     # an FSD.h mod's clang and DOM peak near 1 GB; what is left to the machine
_START_LOCK, _STARTS = threading.Lock(), []


def free_mb():
    """Physical memory free for a new process, in MB, or None where it cannot be read."""
    if os.name == 'nt':
        import ctypes

        class Status(ctypes.Structure):
            _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong)] + \
                       [(n, ctypes.c_ulonglong) for n in ('total', 'avail', 'pagefile', 'pagefile_avail', 'virtual',
                                                          'virtual_avail', 'extended')]
        s = Status(dwLength=ctypes.sizeof(Status))
        return s.avail >> 20 if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(s)) else None
    try:
        with open('/proc/meminfo') as f:
            return next(int(l.split()[1]) >> 10 for l in f if l.startswith('MemAvailable:'))
    except (OSError, StopIteration):
        return None


def run_compile(cmd, cwd=None):
    """subprocess.run of one assetgen compile, started only when the machine has memory for it. Several suites on one
    machine, each with WORKERS compiles at once, ran it out of memory. A compile claims its memory over its first
    second or two, so the ones started in the last 2 s count as not claimed yet: one more starts while free memory
    covers RESERVE_MB plus COMPILE_MB for each of them and for itself."""
    with _START_LOCK:
        while True:
            now = time.monotonic()
            _STARTS[:] = [t for t in _STARTS if now - t < 2]
            free = free_mb()
            if free is None or free >= RESERVE_MB + COMPILE_MB * (len(_STARTS) + 1):
                break
            time.sleep(0.25)
        _STARTS.append(now)
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding='utf-8', cwd=cwd)
    out, err = proc.communicate()
    return subprocess.CompletedProcess(cmd, proc.returncode, out, err)


# ---- Compiles. Every `assetgen compile` a test makes goes through assetgen_compile(). The ~200 the tests make one at a
# time after build() are prefetched: each run records them in a manifest, and the next starts them all right after
# build(), WORKERS at once, each in a staging folder of its own, in the order the tests last made them. A test whose
# compile is one of them takes that result, moved into place, instead of compiling. The manifest only says what to
# compile: every result is this run's assetgen on the files the test itself wrote, used only when it is the same
# compile in everything a compile reads (_compile_entry). --no-prefetch compiles each one when the test asks, as before,
# and still records the manifest; --check-prefetch also compiles every prefetched one in its real place, once the moved
# result is taken out again, and stops the run on any difference.

PREFETCH_MANIFEST = os.path.join(tempfile.gettempdir(), 'assetgen-suite-prefetch.json')   # one per machine: any checkout's last run
PREFETCH_OFF, PREFETCH_CHECK = '--no-prefetch' in sys.argv, '--check-prefetch' in sys.argv
PREFETCH_VERSION = 1            # of the manifest's layout; a manifest of another reads as none
PREFETCH_MAX = 1000             # manifest entries kept; a run records about 200
MANIFEST_TRIES, MANIFEST_RETRY_S = 10, 0.05     # another run reading or replacing the manifest holds it this long at most
TREE_MAX_FILES, TREE_MAX_BYTES = 64, 1 << 20            # a test's source folder past either compiles directly, unrecorded
TREE_EXTS = ('.cpp', '.h', '.hpp', '.inl', '.inc')      # any other file in a test's source folder: not a fresh out
STABLE_DIRS = {os.path.normcase(os.path.join(AG, *d)) for d in (('tests',), ('tests', 'pending'), ('tests', 'ast'), ('examples',))}


def _stamp(path):
    """path, its size and its modification time: what tells two builds of a file apart without reading it."""
    try:
        st = os.stat(path)
        return [path, st.st_size, st.st_mtime_ns]
    except (OSError, TypeError):
        return [path]


# What every compile reads besides the files its entry names, in every key. The prefetch runs this run's assetgen and
# clang on this run's UeApi, so within a run these never tell two compiles apart; they keep the key whole. That they do
# not change during the run is _inputs_stamp's to check.
CLANG = shutil.which('clang++')
TOOLCHAIN = [_stamp(ASSETGEN), _stamp(CLANG), os.path.normcase(UEAPI)]
HEADER_EXTS = ('.h', '.hpp', '.inl', '.inc', '.json')   # what an include or LoadTables can name below UeApi's parent


def _inputs_stamp():
    """The size and time of every file a compile can read outside the folders its key holds, and of the files directly
    in the stable source folders, whose bytes the key holds: assetgen and clang++; every header and table below UeApi's
    parent (ClangCommand in Cpp.cpp passes -I<UeApi> and -I<its parent>, which holds UeAssets/ too; LoadTables reads
    UeApi/*.json); and AssetGen/include (tests/ include ../include/Objects.h). A write changes a file's time, so the same
    stamp at a call as at start() means nothing a staged compile could have read changed in between, a change and its
    undo included. os.scandir reads sizes and times off the folder on Windows: about 10 ms for UeApi's 7,300 headers."""
    stamp = {path: tuple(_stamp(path)[1:]) for path in (ASSETGEN, CLANG)}
    folders = [(os.path.dirname(UEAPI), HEADER_EXTS), (os.path.join(AG, 'include'), HEADER_EXTS)]
    folders += [(d, None) for d in sorted(STABLE_DIRS)]
    while folders:
        folder, exts = folders.pop()
        try:
            with os.scandir(folder) as it:
                for e in it:
                    if e.is_dir():
                        if exts: folders.append((e.path, exts))     # the stable folders' own files only
                    elif exts is None or e.name.lower().endswith(exts):
                        st = e.stat()
                        stamp[e.path] = (st.st_size, st.st_mtime_ns)
        except OSError:
            stamp[folder] = None
    return stamp


def _no_content_dir(path):
    """No folder on path is named Content. A compile writes its registry, and a package outside the mod's own path, into
    the Content folder its out ends in, or above (FCompiler::Run, ContentDir in Cpp.cpp): with no Content folder in the
    part a staging folder replaces, the staged compile writes each at the same place relative to the part it keeps."""
    return 'content' not in [c.lower() for c in re.split(r'[\\/]', path)]


def _place(path):
    """path as the manifest keeps it: inside AssetGen as <AG> and the rest, separators as written, so that a manifest
    one checkout wrote maps onto another's; anything else as it is."""
    return '<AG>' + path[len(AG):] if path == AG or path.startswith(AG + os.sep) else path


def _unplace(place):
    return AG + place[4:] if place.startswith('<AG>') else place


def _plain_rel(rel):
    """rel, a path the manifest puts below a staging folder, stays below it."""
    return ':' not in rel and not any(p in ('.', '..') for p in re.split(r'[\\/]', rel))


def _tree(top):
    """The folders (relative, '/'-separated) and files (relative name -> text) under top; None when a file there is no
    source - a compile's output, so top is no fresh out - or when there is more than a manifest should carry."""
    dirs, files, size = [], {}, 0
    for root, subdirs, names in os.walk(top):
        subdirs.sort()
        rel = os.path.relpath(root, top).replace(os.sep, '/')
        if rel != '.': dirs.append(rel)
        for name in sorted(names):
            if not name.lower().endswith(TREE_EXTS) or len(files) == TREE_MAX_FILES: return None
            with open(os.path.join(root, name), 'rb') as f: data = f.read()
            size += len(data)
            if size > TREE_MAX_BYTES: return None
            try: files[name if rel == '.' else rel + '/' + name] = data.decode('utf-8')
            except UnicodeDecodeError: return None
    return dirs, files


def _walk(top):
    """The folders and files under top, relative and '/'-separated, the files with their bytes."""
    dirs, files = set(), {}
    for root, _, names in os.walk(top):
        rel = os.path.relpath(root, top).replace(os.sep, '/')
        if rel != '.': dirs.add(rel)
        for name in names:
            with open(os.path.join(root, name), 'rb') as f: files[name if rel == '.' else rel + '/' + name] = f.read()
    return dirs, files


def _compile_entry(args, cwd):
    """The manifest entry of `assetgen compile <args>`: what to compile and, with _compile_key, everything it reads that
    two compiles in one run can differ in. None when a staged compile cannot stand in for it: only `<src> <UEAPI> <out>`
    qualifies, absolute, no flag (--game reads the game's packages, --api writes elsewhere), no cwd, in one of two shapes.
    tree    A source in a folder the test made, out that folder or one inside it. The entry holds the whole folder: the
            mod's own .h/.cpp, which ModSources (Cpp.cpp) reads beside the source, what clang includes or __EmbedFile__
            reads from there, and the folders out is in. Nothing else, so out is fresh: the one thing a compile reads
            back from where it writes, an AssetRegistry.bin to merge into (MergeAssetRegistry), is not there. The staged
            copy is made beside the real folder, so `../x.h` and absolute includes reach the same files.
    stable  A source in tests/, tests/pending/, tests/ast/ or examples/, compiled where it is into an empty out: one in
            AssetGen (tests/build/_pending/...), mirrored below the staging folder so that the registry lands at the
            same place relative to it, or one outside (a temp folder), staged beside it. The key adds the bytes of every
            file directly in the source's folder."""
    if cwd is not None or len(args) != 3 or args[1] != UEAPI: return None
    src, out = args[0], args[2]
    if not (os.path.isabs(src) and os.path.isabs(out)): return None
    srcdir = os.path.dirname(src)
    if os.path.normcase(srcdir) in STABLE_DIRS:
        if not src.startswith(AG + os.sep) or not os.path.isdir(out) or os.listdir(out): return None
        if out.startswith(AG + os.sep) and _no_content_dir(AG) and _no_content_dir(tempfile.gettempdir()):
            return {'kind': 'stable', 'src': src[len(AG):], 'out': ['ag', out[len(AG):]]}
        if _no_content_dir(out):
            return {'kind': 'stable', 'src': src[len(AG):], 'out': ['beside', _place(os.path.dirname(out))]}
        return None
    inside = out == srcdir or out.startswith(srcdir) and out[len(srcdir)] in (os.sep, os.altsep)
    tree = _tree(srcdir) if inside and _no_content_dir(srcdir) else None
    if tree is None: return None
    return {'kind': 'tree', 'dir': _place(os.path.dirname(srcdir)), 'src': src[len(srcdir):], 'out': out[len(srcdir):],
            'dirs': tree[0], 'files': tree[1]}


def _stageable(entry):
    """A manifest entry this checkout can stage: a shape it knows, its folders here, and no path that leaves them."""
    try:
        if entry['kind'] == 'tree':
            names = [entry['src'], entry['out']] + list(entry['dirs']) + list(entry['files'])
            ok = os.path.isdir(_unplace(entry['dir'])) and all(isinstance(t, str) for t in entry['files'].values())
        elif entry['kind'] == 'stable':
            names = [entry['src']] + ([entry['out'][1]] if entry['out'][0] == 'ag' else [])
            ok = (os.path.isfile(AG + entry['src']) and os.path.normcase(os.path.dirname(AG + entry['src'])) in STABLE_DIRS
                  and (entry['out'][0] == 'ag' or entry['out'][0] == 'beside' and os.path.isdir(_unplace(entry['out'][1]))))
        else:
            return False
        return ok and all(isinstance(n, str) and _plain_rel(n) for n in names)
    except (KeyError, TypeError, IndexError, AttributeError):
        return False


def _dir_digest(folder):
    """The names and bytes of the files directly in folder: what ModSources reads beside a stable source, and more."""
    h = hashlib.sha256()
    for name in sorted(os.listdir(folder)):
        path = os.path.join(folder, name)
        if os.path.isfile(path):
            with open(path, 'rb') as f: data = f.read()
            h.update(json.dumps([name, len(data)]).encode('utf-8') + data)
    return h.hexdigest()


def _compile_key(entry, digests=None):
    """What a staged compile and a test's must share for one to stand in for the other: the entry, the files beside a
    stable source (digests caches them for start(), which keys every entry at once) and the toolchain."""
    beside = None
    if entry['kind'] == 'stable':
        folder = os.path.dirname(AG + entry['src'])
        beside = (digests or {}).get(folder) or _dir_digest(folder)
        if digests is not None: digests[folder] = beside
    return hashlib.sha256(json.dumps([entry, beside, TOOLCHAIN], sort_keys=True).encode('utf-8')).hexdigest()


def _swap(text, stage, real):
    """A staged compile's stdout or stderr as the compile in real prints it: clang names the source by the path it was
    given, the compiler its source and its out and registry folders, the registry's with forward slashes."""
    text = text.replace(stage, real)
    return text.replace(stage.replace('\\', '/'), real.replace('\\', '/')) if os.sep == '\\' else text


def _wrote_outside(stdout, stage, outputs):
    """A staged compile wrote what its staging folder does not hold, so its result cannot be moved into place: a package
    outside the mod's own path (printed by its /Game path) that ContentDir put above the folder, or the registry."""
    lower = [o.lower() for o in outputs]
    if any(not any(o.endswith('/' + p.lower()) for o in lower) for p in re.findall(r'-> /Game/(\S+)', stdout)): return True
    return any(d != '.' and not d.startswith(stage.replace('\\', '/'))
               for d in re.findall(r'(?m)^\s*registry\s+-> (.+)/AssetRegistry\.bin', stdout))


def _retried(fn):
    """fn(), tried again for a while on PermissionError. Windows renames over no file another process has open, and
    opens no file while it is being renamed over: two checkouts' runs share the manifest, so one may hold it briefly."""
    for attempt in range(MANIFEST_TRIES):
        try:
            return fn()
        except PermissionError:
            if attempt == MANIFEST_TRIES - 1: raise
            time.sleep(MANIFEST_RETRY_S)


def _materialize(cmd, res, real):
    """A staged compile's result (res, from _staged) as the compile in real gives it: its new folders made in real and
    its outputs copied there, stdout and stderr with the staging folder swapped for real. None when that fails part
    way, on an OSError (a file an indexer or a scanner holds, a full disk): what it put in real is removed again, so
    real is as fresh as it was and the test compiles there directly."""
    made, copied = [], []
    try:
        for d in sorted(res['dirs']):       # a folder sorts before those inside it; the rest of the path is in real
            path = os.path.join(real, *d.split('/'))
            if not os.path.isdir(path):
                os.mkdir(path)
                made.append(path)
        for o in res['outputs']:
            dst = os.path.join(real, *o.split('/'))
            copied.append(dst)
            shutil.copyfile(os.path.join(res['stage'], *o.split('/')), dst)
    except OSError:
        for path in copied:
            try: os.remove(path)
            except OSError: pass
        for path in reversed(made):
            try: os.rmdir(path)
            except OSError: pass
        return None
    return subprocess.CompletedProcess(cmd, res['rc'], _swap(res['stdout'], res['stage'], real),
                                       _swap(res['stderr'], res['stage'], real))


def _read_manifest():
    """The last run's entries, in the order it made their compiles; none when there is no manifest of this layout.
    Any failure to read one means no prefetch, never a failed run: json.load raises RecursionError on deep nesting."""
    def load():
        with open(PREFETCH_MANIFEST, encoding='utf-8') as f:
            return json.load(f)
    try:
        data = _retried(load)
        if data.get('version') == PREFETCH_VERSION and isinstance(data.get('entries'), list):
            return [e for e in data['entries'] if isinstance(e, dict)]
    except Exception:
        pass
    return []


def _write_manifest(entries):
    """Into a temp file beside the manifest, then renamed over it, so a run never reads half of one. The last run to
    finish wins; a stale entry only costs a compile nobody takes. One that cannot be written keeps the old one."""
    tmp = None
    try:
        fd, tmp = tempfile.mkstemp(prefix='assetgen-suite-prefetch.', suffix='.tmp', dir=os.path.dirname(PREFETCH_MANIFEST))
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            json.dump({'version': PREFETCH_VERSION, 'entries': entries[:PREFETCH_MAX]}, f)
        _retried(lambda: os.replace(tmp, PREFETCH_MANIFEST))
    except OSError:
        try:
            if tmp: os.remove(tmp)
        except OSError:
            pass


def _call_site():
    """The suite's own frames that led to a compile, innermost first (any frames, for a driver that runs the helpers on
    their own): where a --check-prefetch difference came from."""
    import traceback
    inner = ('compile', '_prefetched', '_checked', '_call_site', 'assetgen_compile')
    stack = [f for f in traceback.extract_stack() if f.name not in inner]
    frames = [f for f in stack if f.filename == CompilePrefetch.compile.__code__.co_filename] or stack
    return ' <- '.join('%s (%s:%d)' % (f.name, os.path.basename(f.filename), f.lineno) for f in reversed(frames[-4:]))


class PrefetchMismatch(BaseException):
    """A prefetched compile that is not what the direct one gives (--check-prefetch). Not an Exception: pending() reads
    those as known gaps, and this must stop the run."""


class CompilePrefetch:
    """The prefetch: what this run records for the next, the staged compiles running or queued (by key, in the
    manifest's order), and the counts the summary line prints."""

    def __init__(self):
        self.started = self.finished = False
        self.lock = threading.Lock()                    # guards stages, which the prefetch threads add to
        self.pool, self.waiting, self.stages = None, {}, set()
        self.record, self.previous = [], []
        self.calls = self.hits = self.unused = 0
        self.inputs, self.changed_at = None, 0          # _inputs_stamp() at start(); the call that found it changed

    def start(self):
        """Right after build(), not before: build() wipes tests/build and keeps WORKERS busy, and its compiles, made all
        at once already, are not recorded. Queues every compile the manifest lists, in its order."""
        self.started, self.previous = True, _read_manifest()
        atexit.register(self.finish, complete=False)
        if PREFETCH_OFF or not self.previous: return
        from concurrent.futures import ThreadPoolExecutor
        self.pool = ThreadPoolExecutor(WORKERS)
        # Before the interpreter joins the pool's threads at exit, which drains the queue first: a run a failure stops
        # waits only for the compiles already running. (atexit runs after that join.)
        stop = getattr(threading, '_register_atexit', None)
        if stop: stop(lambda: self.pool.shutdown(wait=False, cancel_futures=True))
        self.inputs = _inputs_stamp()       # before the keys read the stable folders, and before any staged compile
        digests = {}
        for entry in self.previous:
            try:
                key = _compile_key(entry, digests) if _stageable(entry) else None
            except Exception:       # a file beside its source held open by an editor or a scanner: not staged
                key = None
            if key: self.waiting.setdefault(key, []).append(self.pool.submit(self._staged, entry))

    def compile(self, args, cwd=None):
        cmd = [ASSETGEN, 'compile'] + list(args)
        if self.started and not self.finished:
            self.calls += 1
            # The entry and key read every file beside the source. One held open by an editor or a scanner makes this
            # compile what it was before the prefetch: made directly, and not recorded.
            try:
                entry = _compile_entry(args, cwd)
                key = _compile_key(entry) if entry is not None and self.pool else None
            except OSError:
                entry = key = None
            if entry is not None:
                self.record.append(entry)
                proc = self._prefetched(cmd, entry, key, args) if key else None
                if proc is not None: return proc
        return run_compile(cmd, cwd)

    def _staged(self, entry):
        """entry's compile in a staging folder of its own, on a prefetch thread: what it printed and returned, and what
        it wrote, relative to the staging folder, which stands for the real one."""
        res = {'stage': None, 'usable': False}
        try:
            parent = (_unplace(entry['dir']) if entry['kind'] == 'tree' else
                      None if entry['out'][0] == 'ag' else _unplace(entry['out'][1]))
            stage = res['stage'] = tempfile.mkdtemp(dir=parent)
            with self.lock: self.stages.add(stage)
            if entry['kind'] == 'tree':
                for d in entry['dirs']: os.makedirs(os.path.join(stage, *d.split('/')), exist_ok=True)
                for rel, text in entry['files'].items():
                    with open(os.path.join(stage, *rel.split('/')), 'wb') as f: f.write(text.encode('utf-8'))
                src, out = stage + entry['src'], stage + entry['out']
            else:
                src, out = AG + entry['src'], stage + entry['out'][1] if entry['out'][0] == 'ag' else stage
                os.makedirs(out, exist_ok=True)
            dirs, files = _walk(stage)
            proc = run_compile([ASSETGEN, 'compile', src, UEAPI, out])
            dirs_after, files_after = _walk(stage)
            outputs = sorted(r for r, data in files_after.items() if files.get(r) != data)
            res.update(rc=proc.returncode, stdout=proc.stdout, stderr=proc.stderr, outputs=outputs,
                       dirs=sorted(dirs_after - dirs), usable=files.keys() <= files_after.keys()
                       and not any(r in files for r in outputs) and not _wrote_outside(proc.stdout, stage, outputs))
        except Exception:       # an OSError mostly: a compile that could not be staged is one the test makes itself
            res['usable'] = False
        return res

    def _prefetched(self, cmd, entry, key, args):
        """The prefetched result of entry's compile (key: _compile_key's), moved into place; None when there is none to
        take: never queued, still queued (cancelled: compiling here costs the same, and is the real thing), staged
        before a file it may have read changed (_inputs_changed), its outputs' places taken, or moving them there
        failed (_materialize)."""
        futures = self.waiting.get(key)
        if not futures: return None
        future = futures.pop(0)
        if future.cancel(): return None
        res = future.result()
        real = os.path.dirname(args[0]) if entry['kind'] == 'tree' else AG if entry['out'][0] == 'ag' else args[2]
        try:
            if (not res['usable'] or self._inputs_changed()
                    or any(os.path.lexists(os.path.join(real, *o.split('/'))) for o in res['outputs'])):
                self.unused += 1
                return None
            proc = self._checked(cmd, entry, real, res) if PREFETCH_CHECK else _materialize(cmd, res, real)
            if proc is None: self.unused += 1
            else: self.hits += 1
            return proc
        finally:
            self._drop(res)

    def _inputs_changed(self):
        """Whether a file a compile can read outside its key changed since start() (_inputs_stamp), checked once the
        staged compile is done, so its reads fall in between. Once one has, no prefetched result is taken any more and
        the queued ones are cancelled: each was or would be staged on the old files, or on some of each."""
        if not self.changed_at and _inputs_stamp() != self.inputs:
            self.changed_at = self.calls
            for future in itertools.chain.from_iterable(self.waiting.values()): future.cancel()
        return bool(self.changed_at)

    def _checked(self, cmd, entry, real, res):
        """--check-prefetch: the result moved into place as a plain run moves it (_materialize), what that put there
        taken out again, then the compile made directly in the same place, which must give the same: exit code, stdout
        and stderr, and every folder and file written, byte for byte. So the check covers the move a plain run relies
        on, not only the staged compile. In AssetGen only the folder holding all it wrote is compared, not the whole
        tree. None, as in a plain run, when the move fails; an error taking it out or walking the folder stops the run."""
        top = ''
        if entry['kind'] == 'stable' and entry['out'][0] == 'ag':
            top = posixpath.commonpath([posixpath.dirname(o) for o in res['outputs']] + list(res['dirs'])
                                       + [entry['out'][1].replace('\\', '/').strip('/')])
        root = os.path.join(real, *top.split('/')) if top else real
        site = _call_site()
        fail = lambda problems: PrefetchMismatch('--check-prefetch, at %s: %s' % (site, '; '.join(problems)))
        try:
            dirs, files = _walk(root)
            moved = _materialize(cmd, res, real)
            if moved is None: return None
            dirs_moved, files_moved = _walk(root)
            for r in files_moved.keys() - files.keys(): os.remove(os.path.join(root, *r.split('/')))
            for d in sorted(dirs_moved - dirs, reverse=True): os.rmdir(os.path.join(root, *d.split('/')))
            if _walk(root) != (dirs, files): raise fail(['taking the moved result out did not leave the folder as it was'])
            proc = run_compile(cmd)
            dirs_after, files_after = _walk(root)
        except OSError as e:
            raise fail(['%s: %s' % (type(e).__name__, e)])
        problems = [] if proc.returncode == moved.returncode else ['exit %d, prefetched %d' % (proc.returncode, moved.returncode)]
        for name in ('stdout', 'stderr'):
            got, staged = getattr(proc, name).splitlines(), getattr(moved, name).splitlines()
            if got != staged:
                at = next((i for i, (a, b) in enumerate(zip(got, staged)) if a != b), min(len(got), len(staged)))
                problems.append('%s line %d: %r, prefetched %r' % (name, at + 1, (got + [None])[at], (staged + [None])[at]))
        made = {r: data for r, data in files_after.items() if files.get(r) != data}
        want = {r: data for r, data in files_moved.items() if files.get(r) != data}
        if made.keys() != want.keys() or dirs_after - dirs != dirs_moved - dirs:
            problems.append('wrote %s, moved in %s' % (sorted(made) + sorted(dirs_after - dirs), sorted(want) + sorted(dirs_moved - dirs)))
        problems += ['%s differs' % r for r in sorted(made.keys() & want.keys()) if made[r] != want[r]]
        if problems: raise fail(problems)
        return proc

    def _drop(self, res):
        if res.get('stage'):
            shutil.rmtree(res['stage'], ignore_errors=True)
            with self.lock: self.stages.discard(res['stage'])

    def finish(self, complete=True):
        """Stops the prefetch, removes its staging folders and writes the manifest; at the end of a run (complete),
        prints the summary line. A run cut short (atexit) keeps, after its own, the last manifest's entries past as
        many as it recorded: its tests never got to those. Not every entry it lacks: an edited test's old compile,
        made before the run stopped, would then come back after every run cut short, and be prefetched each time."""
        if not self.started or self.finished: return
        self.finished = True
        if self.pool:
            self.pool.shutdown(wait=True, cancel_futures=True)
            for future in itertools.chain.from_iterable(self.waiting.values()):
                if not future.cancelled():
                    self.unused += 1
                    self._drop(future.result())
        with self.lock: stages = list(self.stages)
        for stage in stages: shutil.rmtree(stage, ignore_errors=True)
        entries = self.record
        if not complete:
            mine = {json.dumps(e, sort_keys=True) for e in entries}
            entries = entries + [e for e in self.previous[len(entries):] if json.dumps(e, sort_keys=True) not in mine]
        _write_manifest(entries)
        if not complete: return
        if PREFETCH_OFF:
            print('ok  prefetch: off, all %d compiles made directly (%d recorded for the next run)' % (self.calls, len(self.record)))
        else:
            print('ok  prefetch: %d of %d compiles were ready (%d compiled directly, %d prefetched and unused)%s%s' % (
                self.hits, self.calls, self.calls - self.hits, self.unused,
                '; each checked against a direct compile' if PREFETCH_CHECK else '',
                '; a header, table, source or the exe changed by compile %d, none taken after' % self.changed_at
                if self.changed_at else ''))


PREFETCH = CompilePrefetch()


def assetgen_compile(args, cwd=None):
    """`assetgen compile <args>`, run in cwd, its stdout and stderr captured as UTF-8 text, as subprocess.run returns
    it: every compile the suite makes goes through here. Before PREFETCH.start() it just compiles; after, a compile
    the prefetch made already is taken from it. The other verbs (roundtrip, registry, astcheck) run as they are."""
    return PREFETCH.compile(args, cwd)


def build():
    """Each test compiles into build/<Test>/FSD/Content/<its package>, the layout bpbuild stages a mod in. The examples
    the docs point at compile the same way, so one the compiler stops accepting fails here rather than for a reader."""
    shutil.rmtree(ROOT, ignore_errors=True)

    def compile_mod(src):
        mod = os.path.splitext(os.path.basename(src))[0]
        package = re.search(r'UE_MOD_PACKAGE\s*\(\s*"/Game/([^"]+)"', open(src, encoding='utf-8-sig').read()).group(1)
        out = os.path.join(ROOT, mod, 'FSD', 'Content', *package.split('/'))
        os.makedirs(out)
        game = ['--game', os.path.join(ROOT, 'AssetTest', 'FSD', 'Content')] if mod == 'EditTest' else []
        return mod, assetgen_compile([src, UEAPI, out] + game)
    srcs = sorted(glob.glob(os.path.join(TESTS, '*.cpp'))) + sorted(glob.glob(os.path.join(AG, 'examples', '*.cpp')))
    # EditTest edits AssetTest's cooked assets as if they were the game's, so it compiles once AssetTest has.
    edits = [s for s in srcs if os.path.basename(s) == 'EditTest.cpp']
    for mod, proc in parallel(compile_mod, [s for s in srcs if s not in edits]) + [compile_mod(s) for s in edits]:
        assert proc.returncode == 0, '%s:\n%s%s' % (mod, proc.stdout, proc.stderr)
        LOGS[mod] = proc.stdout
    print('ok  every test and example compiles')
    # The reader behind S38 refuses any layout the game's own 52,645 cooked packages do not share, and writes each one
    # back byte for byte: so every package here must come back unchanged, name order and hashes included. Each tag's
    # value is read into the model and encoded again, which must give the same bytes.
    proc = subprocess.run([ASSETGEN, 'roundtrip', ROOT], capture_output=True, encoding='utf-8')
    assert proc.returncode == 0, proc.stdout + proc.stderr
    print('ok  every package reads back and writes out byte for byte, and every tag value re-encodes (assetgen roundtrip)')


def astcheck():
    """A compile throws away what its AST parser never reads before nlohmann lexes it (FDumpFilter, DESIGN.md "Compile
    time: the UeApi header cost"). `assetgen astcheck` proves that changes nothing on three dumps: an FSD.h mod's, a mod
    with a Game/ header and a GetSubsystem<T> instance, and tests/ast/Edge.cpp, the shapes a filter could get wrong. The
    filter's output must be the same however the dump is cut, and parse to the same tree as the dump itself, read the
    unfiltered fallback's way. That reference read drops a frozen copy of DroppedAstKey's keys, not the list the filter
    and the compile's parser share, so an edit to that list fails here too until the copy gets the same edit on purpose.
    One at a time: each holds about 1 GB."""
    for src in ('FlowTest.cpp', 'SubsystemTest.cpp', os.path.join('ast', 'Edge.cpp')):
        proc = subprocess.run([ASSETGEN, 'astcheck', os.path.join(TESTS, src), UEAPI], capture_output=True, encoding='utf-8')
        assert proc.returncode == 0 and proc.stdout.startswith('same ('), '%s:\n%s%s' % (src, proc.stdout, proc.stderr)
    print('ok  the AST dump filter changes nothing: same bytes however the dump is cut, and the same tree (assetgen astcheck)')


build()
PREFETCH.start()
astcheck()


# --cases <file>: also write every run() below as a case for BpMods' `bpcheck` command, which replays it in the game
# (the same call on the same members) and compares what comes back. A case the game cannot mean the same way says why
# in 'skip': raw memory (runscript's MEM stands in for this process's), a stand-in call, a value JSON cannot carry.
if '--cases' in sys.argv:
    import atexit, json, math
    CASES, MATH0, RAW = [], set(runscript.MATH), [False]
    STANDINS = {'RandomInteger', 'GetFSDGameState', 'Conv_ObjectToString'}   # the game answers these differently

    def _noting_raw(f):
        def g(*a):
            RAW[0] = True
            return f(*a)
        return g
    runscript.mem_read, runscript.mem_write = _noting_raw(runscript.mem_read), _noting_raw(runscript.mem_write)

    def _plain(v):
        """v as JSON; a dict whose keys are not all strings (a map's) as {"__pairs__": [[k, v], ...]}."""
        if isinstance(v, runscript.Holey): raise ValueError('a map with free slots')
        if isinstance(v, float) and not math.isfinite(v): raise ValueError('a non-finite float')
        if v is None or isinstance(v, (bool, int, float, str)): return v
        if isinstance(v, (list, tuple)): return [_plain(x) for x in v]
        if isinstance(v, dict):
            if all(isinstance(k, str) for k in v): return {k: _plain(x) for k, x in v.items()}
            return {'__pairs__': [[_plain(k), _plain(x)] for k, x in v.items()]}
        raise ValueError('a ' + type(v).__name__)

    def _recording(real):
        def run(base, function, self_vars=None, **parms):
            mine = self_vars if self_vars is not None else {}
            where = base.replace(os.sep, '/').split('/FSD/Content/')
            case = {'mod': os.path.basename(where[0]), 'class': '/Game/' + where[-1], 'fn': function}
            try: case.update(args=_plain(parms), self=_plain(mine))
            except ValueError as e: case['skip'] = str(e)
            RAW[0], first = False, len(runscript.CALLS)
            ret, env = real(base, function, mine, **parms)
            try: case.update(ret=_plain(ret), after=_plain(mine))
            except ValueError as e: case.setdefault('skip', str(e))
            case['env'] = {}
            for k, v in env.items():
                try: case['env'][k] = _plain(v)
                except ValueError: pass
            called = {c[0] for c in runscript.CALLS[first:]}
            odd = sorted(called & STANDINS | called - MATH0)
            if RAW[0]: case['skip'] = 'raw memory'
            elif odd: case.setdefault('skip', 'stand-in ' + ', '.join(odd))
            CASES.append(case)
            return ret, env
        return run
    run = _recording(run)
    atexit.register(lambda: json.dump(CASES, open(option('--cases', []), 'w', encoding='utf-8')))


def asset(mod):
    return os.path.join(ROOT, mod, 'FSD', 'Content', '_ElytrasMods', mod, mod)


def refused(mod, body, why, top=''):
    """A mod (the class body given, `top` before the class) the compiler must refuse, saying why."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, mod + '.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/%s");\n%s'
                    'class %s : public AActor {\npublic:\n%s};\n' % (mod, top, mod, body))
        proc = assetgen_compile([src, UEAPI, tmp])
        assert proc.returncode != 0 and why in proc.stdout, (mod, proc.stdout)


# ---- Known gaps: what AssetGen does not do yet, as tests that fail today. A pending test has its mod in tests/pending;
# a rule of invariants.py the suite's own packages still break is listed in KNOWN_RULES with where it is tracked. Each
# prints as `gap` while it fails, and the run fails the day one passes, until it moves in with the others.

PENDING = os.path.join(TESTS, 'pending')
GAPS, FIXED, REFUSALS = [], [], {}
KNOWN_RULES = {     # sweep rules the suite's own packages still break, each with the AssetGen defect (TODO.md, S33)
}


def pending_asset(mod, cls=None):
    """tests/pending/<mod>.cpp compiled into build/_pending/<mod>, and the base path of its class `cls` (default: the
    mod's own). A refusal raises, carrying the compiler's reason, so the gap reads as what the compiler said."""
    src = os.path.join(PENDING, mod + '.cpp')
    package = re.search(r'UE_MOD_PACKAGE\s*\(\s*"/Game/([^"]+)"', open(src, encoding='utf-8-sig').read()).group(1)
    out = os.path.join(ROOT, '_pending', mod, 'FSD', 'Content', *package.split('/'))
    if mod not in REFUSALS:
        os.makedirs(out, exist_ok=True)
        proc = assetgen_compile([src, UEAPI, out])
        LOGS[mod] = proc.stdout
        failed = re.findall(r'(?m)^\s*FAILED: (.*)$', proc.stdout)
        REFUSALS[mod] = (failed or [proc.stdout.strip() or 'exit %d' % proc.returncode])[0] if proc.returncode else None
    if REFUSALS[mod]: raise AssertionError('refused: ' + REFUSALS[mod])
    return os.path.join(out, cls or mod)


def keeps_invariants(base):
    """The package at base breaks none of invariants.py's rules but the known ones; a pending mod that compiles is checked
    by them too."""
    import invariants
    found = [f for f in invariants.check(invariants.Package(base)) if f[0] not in KNOWN_RULES]
    assert not found, '%s breaks %s' % (os.path.basename(base), '; '.join('%s %s: %s' % f for f in found[:3]))


def pending(name, test):
    """A test of something AssetGen does not do yet. It fails today - refused, or compiled but not behaving as the
    engine needs - and prints as a known gap. The day it passes, the feature has landed and the run fails until the
    test moves in with the others (its mod to tests/, its check above): a gap never closes unnoticed, and none is
    reported open that is closed."""
    try:
        test()
    except (Exception, SystemExit) as e:        # runscript stops on an op it cannot run with SystemExit
        GAPS.append(name)
        why = str(e).strip().splitlines()[0] if str(e).strip() else type(e).__name__
        print('gap %s: %s' % (name, why[:200]))
        return
    FIXED.append(name)
    print('FIXED %s: it passes now; move it out of tests/pending' % name)

dump = runscript.tool_output   # what `python <tool> <args>` prints, without the process


def exports_of(base):
    return [e['name'] for e in dumpexp.load(base)[5]]


def import_paths(base, classes=False):
    """Each import as its full path, /Game/Pkg.Class_C:Function. dumpexp keeps Class'Name' only, and one package
    can import two objects of one name (SuperTest: SuperBase_C:Bump for the parent call, its own SuperTest_C:Bump).
    With `classes`, (path, the ClassPackage.ClassName the linker checks the object against) pairs."""
    import struct
    ua, names = open(base + '.uasset', 'rb').read(), dumpexp.load(base)[3]
    r = dumpexp.R(ua, 4)                                        # the summary, as dumpexp.load walks it
    if r.i32() != -4: r.i32()
    r.i32(); r.i32()
    custom = r.i32()
    r.o += 20 * custom
    r.i32(); r.fstr(); flags = r.u32()
    r.o += 8
    if not flags & 0x80000000: r.fstr()
    r.o += 16
    count, off = r.i32(), r.i32()
    stride = 28 if flags & 0x80000000 else 36
    rows = [struct.unpack_from('<7i', ua, off + stride * i) for i in range(count)]

    def path(i):
        outer, obj, num = rows[i][4:]
        name = names[obj] + ('_%d' % (num - 1) if num else '')
        if outer == 0: return name
        return path(-outer - 1) + ('.' if rows[-outer - 1][4] == 0 else ':') + name
    return [(path(i), names[rows[i][0]] + '.' + names[rows[i][2]]) if classes else path(i) for i in range(count)]


def ref(base, index):
    """An FPackageIndex off a tag or an export header, as an import's full path or this package's export name."""
    return import_paths(base)[-index - 1] if index < 0 else exports_of(base)[index - 1]


def registry_of(mod):
    """A pak's one AssetRegistry.bin sits beside its Content folder, as the game's FSD/AssetRegistry.bin does."""
    return os.path.join(ROOT, mod, 'FSD', 'AssetRegistry.bin')


def registry_rows(path):
    return set(re.findall(r'^\s+(/Game/\S+)\s+(\S+)$', dump('dumpar.py', path), re.M))


def registry_layout():
    """Each test's registry is FSD/AssetRegistry.bin, none sits in a package folder, and `assetgen registry`
    folds several into one - what bpbuild does for an embedded dependency - replacing a package it already has.
    EditTest only edits, and a pak with no rows carries no registry: mounted at startup it would replace the game's."""
    import tempfile
    for mod in os.listdir(ROOT):
        assert os.path.exists(registry_of(mod)) == (mod != 'EditTest'), mod
        assert not glob.glob(os.path.join(ROOT, mod, 'FSD', 'Content', '**', 'AssetRegistry.bin'), recursive=True), mod
    a, b = registry_rows(registry_of('AssetTest')), registry_rows(registry_of('IfaceTest'))
    assert a and b and not a & b, (a, b)
    with tempfile.TemporaryDirectory() as tmp:
        merged = os.path.join(tmp, 'AssetRegistry.bin')
        for twice in range(2):
            proc = subprocess.run([ASSETGEN, 'registry', merged, registry_of('AssetTest'), registry_of('IfaceTest')],
                                  capture_output=True, encoding='utf-8')
            assert proc.returncode == 0 and registry_rows(merged) == a | b, (proc.stdout, registry_rows(merged))
    print('ok  every registry is FSD/AssetRegistry.bin; `assetgen registry` merges them, once per package')


def registry_non_ascii():
    """A non-ASCII class name reaches the registry as the loader reads it - UTF-16, since ANSI widens byte by byte -
    and a recompile and `assetgen registry` merge it back. The package path's odd length puts the UTF-16 package name
    on an odd offset, so its alignment pad is read too."""
    import tempfile
    import dumpar
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'Umlaut.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/Umlaute");\n'
                    'class Größe : public AActor {\npublic:\n  int32 Get() { return 1; }\n};\n')
        want = [('/Game/_ElytrasMods/Umlaute/Größe.Größe_C', '/Game/_ElytrasMods/Umlaute', 'BlueprintGeneratedClass',
                 '/Game/_ElytrasMods/Umlaute/Größe', 'Größe_C')]
        rows = lambda path: [(r['object_path'], r['package_path'], r['asset_class'], r['package_name'], r['asset_name'])
                             for r in dumpar.read(path)[2]]
        for twice in range(2):
            proc = assetgen_compile([src, UEAPI, tmp])
            assert proc.returncode == 0, proc.stdout + proc.stderr
            assert rows(os.path.join(tmp, 'AssetRegistry.bin')) == want, rows(os.path.join(tmp, 'AssetRegistry.bin'))
        merged = os.path.join(tmp, 'Merged.bin')
        proc = subprocess.run([ASSETGEN, 'registry', merged, os.path.join(tmp, 'AssetRegistry.bin'), registry_of('AssetTest')],
                              capture_output=True, encoding='utf-8')
        assert proc.returncode == 0 and set(want) <= set(rows(merged)), (proc.stdout, rows(merged))
    print('ok  a non-ASCII asset name reads back from the registry as written, and merges')


def export_index(base, name):
    return exports_of(base).index(name)


def game_findings(bases):
    """invariants.check over the game's packages, in WORKERS shards run by invariants.py --json (a process each: the
    rules are pure Python, one core per process). Sorted, so the first ones reported do not depend on the shards."""
    import json, tempfile

    def shard(part):
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False, encoding='utf-8') as f:
            f.write('\n'.join(part))
        try:
            proc = subprocess.run([sys.executable, os.path.join(HERE, 'invariants.py'), '--json', '--from', f.name,
                                   '--game', GAME], capture_output=True, encoding='utf-8')
        finally:
            os.remove(f.name)
        lines = proc.stdout.splitlines()
        # every package of the shard read, or the shard's silence would pass for a clean one
        assert lines[-1:] == ['done %d' % len(part)], 'invariants.py stopped on a game shard:\n' + proc.stdout[-2000:] + proc.stderr[-3000:]
        return [tuple(json.loads(l)) for l in lines[:-1]]
    return sorted(f for part in parallel(shard, [bases[i::WORKERS] for i in range(WORKERS)]) for f in part)


def sweep():
    """Every package built above keeps each engine invariant of invariants.py, whose rules cite the engine source that
    makes them one: every function decodes to exactly its header's sizes, every jump lands on a statement, ... With
    --game, the same rules first run on every 9th package of the game's own content: a rule Epic's cooked Blueprints
    break is a wrong rule, not a finding."""
    import invariants
    invariants.GAME_CONTENT[:] = [GAME] if GAME else []
    if GAME:
        found = game_findings(invariants.packages([GAME], 9))
        assert not found, 'a rule the game breaks:\n' + '\n'.join('%s  %s %s: %s' % f for f in found[:30])
        print('ok  the %d rules of invariants.py hold on the game\'s own packages' % len(invariants.RULES))
    bases = invariants.packages([ROOT])
    found = [(os.path.relpath(b, ROOT), *f) for b in bases for f in invariants.check(invariants.Package(b))]
    for rule, why in KNOWN_RULES.items():
        mine = [f for f in found if f[1] == rule]
        if mine:
            GAPS.append('sweep ' + rule)
            print('gap sweep %s: %d findings (%s), e.g. %s' % (rule, len(mine), why, '%s  %s %s: %s' % mine[0]))
        else:
            FIXED.append('sweep ' + rule)
            print('FIXED sweep %s: no package breaks it now; take it out of KNOWN_RULES' % rule)
    found = [f for f in found if f[1] not in KNOWN_RULES]
    assert not found, '\n'.join('%s  %s %s: %s' % f for f in found[:30])
    print('ok  %d packages keep the %d engine invariants of invariants.py' % (len(bases), len(invariants.RULES) - len(KNOWN_RULES)))


def check(mod, fn, oracle, cases):
    for parms in cases:
        got = run(asset(mod), fn, **parms)[0]
        want = oracle(**parms)
        assert got == want, '%s.%s(%s) = %r, want %r' % (mod, fn, parms, got, want)
    print('ok  %s.%s  (%d cases)' % (mod, fn, len(cases)))


def range_self(**kw):
    return dict(Items=list(kw.get('Items', [])), Seen=list(kw.get('Seen', [])), Scores=dict(kw.get('Scores', {})))


def check_self(mod, fn, oracle, cases):
    """Like check, but each case is the object's own fields; the oracle gets a copy."""
    for fields in cases:
        mine, theirs = range_self(**fields), range_self(**fields)
        got = run(asset(mod), fn, self_vars=mine)[0]
        want = oracle(theirs)
        assert got == want, '%s.%s(%s) = %r, want %r' % (mod, fn, fields, got, want)
    print('ok  %s.%s  (%d cases)' % (mod, fn, len(cases)))


def cdiv(a, b): q = abs(a) // abs(b); return q if (a < 0) == (b < 0) else -q
def cmod(a, b): return a - cdiv(a, b) * b
def clamp(v, lo, hi): return lo if v < lo else hi if v > hi else v
def wrap(v): return (v + 2**31) % 2**32 - 2**31          # Kismet int32 arithmetic wraps
EDGE = (-2**31, -2, 0, 9, 2**30, 2**31 - 1)


# ---- FlowTest

def sum_skipping(Count, Skip):
    t = 0
    for i in range(Count):
        if i == Skip: continue
        if i > 7: break
        t += i
    return t


def first_over(Limit):
    n = 0
    while True:
        n += 1
        if n * n > Limit: break
    return n


def nested(Size):
    h = 0
    for y in range(Size):
        for x in range(Size):
            if x == y: continue
            if x > y: break
            h += 1
    return h


sweep()
registry_layout()
registry_non_ascii()
check('FlowTest', 'SumSkipping', sum_skipping, [dict(Count=c, Skip=k) for c in (0, 1, 5, 10, 20) for k in (-1, 0, 3, 9)])
check('FlowTest', 'FirstOver', first_over, [dict(Limit=l) for l in (0, 1, 5, 99, 100)])
check('FlowTest', 'Nested', nested, [dict(Size=s) for s in (0, 1, 2, 4, 7)])


def classify(Code):
    return {1: 10, 2: 25, 3: 25, 4: 5}.get(Code, -1)


def no_default(Code):
    return {0: 0, 9: 99}.get(Code, 7)


def default_first(Code):
    return {5: 5, 6: 6}.get(Code, 105)


def switch_in_loop(Count):
    s = 0
    for i in range(Count):
        m = i % 3
        if m == 0: continue
        s += 1 if m == 1 else 10
        s += 100
        if s > 1000: break
    return s


check('FlowTest', 'CommaStmt', lambda N: wrap((N + 200) * 100 + 10 - N), [dict(N=n) for n in (-5, 0, 7, 2**30)])
check('FlowTest', 'CommaIf', lambda N: N + 1 if N + 1 <= 2 else -1 if N + 11 == 13 else (N + 11) * 10,
      [dict(N=n) for n in (-3, 0, 1, 2, 3, 50)])
for n in (-3, 0, 4):
    me = {'Total': 1}
    run(asset('FlowTest'), 'ForwardVoid', self_vars=me, N=n)
    assert me['Total'] == 1 + (-n * 100 if n < 0 else n), (n, me)
print('ok  FlowTest.ForwardVoid: `return F();` of a void F calls it, then returns')
refused('CommaExpr', '  int32 F(int32 N) { return (N += 1, N * 2); }\n', 'the comma operator inside an expression')
refused('CommaWhile', '  int32 F(int32 N) { while (N += 1, N < 9) {} return N; }\n', 'the comma operator inside an expression')
print('ok  the comma operator inside an expression or a loop condition is refused by name')
check('FlowTest', 'Classify', classify, [dict(Code=c) for c in range(-2, 8)])
check('FlowTest', 'NoDefault', no_default, [dict(Code=c) for c in (-1, 0, 1, 9, 10)])
check('FlowTest', 'NameSet', lambda N: 2 if N.lower() == 'none' else 1, [dict(N=n) for n in ('None', 'none', 'IntProperty', 'x', 'None_1')])
check('FlowTest', 'NameKind', lambda Kind: {'intproperty': 1, 'floatproperty': 2, 'doubleproperty': 2, 'structproperty': 3}.get(Kind.lower(), 0),
      [dict(Kind=k) for k in ('IntProperty', 'intproperty', 'FloatProperty', 'DoubleProperty', 'StructProperty', 'None', 'Int')])
check('FlowTest', 'DefaultFirst', default_first, [dict(Code=c) for c in (0, 4, 5, 6, 7)])
check('FlowTest', 'SwitchInLoop', switch_in_loop, [dict(Count=c) for c in (0, 1, 2, 3, 7, 30)])
check('FlowTest', 'ByteSwitch', lambda Mode: {2: 20, 255: 1}.get(Mode, 0), [dict(Mode=m) for m in (0, 2, 254, 255)])
check('FlowTest', 'ByteSwitchStray', lambda Mode: 1 if Mode == 7 else 0, [dict(Mode=m) for m in (0, 7, 44, 255)])
check('FlowTest', 'DenseHoles', lambda Code: {10: 1, 11: 2, 13: 4, 14: 5}.get(Code, -9), [dict(Code=c) for c in range(8, 17)])
check('FlowTest', 'Negative', lambda Code: {-2: -20, -1: -10, 0: 0}.get(Code, 50), [dict(Code=c) for c in range(-4, 3)])


def clusters():
    # every hole and both edges of each dense run, the outliers' neighbours, and the int32 extremes
    check('FlowTest', 'Clusters', lambda Code: CLUSTERS.get(Code, -1),
          [dict(Code=c) for c in list(range(-2, 13)) + list(range(98, 113)) + [49, 50, 51, 6999, 7000, 7001, -2**31, 2**31 - 1]])


def flow_members():
    """Member side effects: BeginPlay's store, Slots[NextSlot()] += By locating its slot once, the Cursor a template
    reads, and locals named Total leaving the member alone."""
    from runscript import i32
    base = asset('FlowTest')
    exports = dumpexp.load(base)[5]
    imports = dumpexp.load(base)[4]
    bp = next(e for e in exports if e['name'] == 'ReceiveBeginPlay')
    assert imports[-bp['super'] - 1] == "Function'ReceiveBeginPlay'", imports[-bp['super'] - 1]   # overrides the Actor event
    me = dict(Total=-1)
    assert run(base, 'ReceiveBeginPlay', self_vars=me)[0] is None and me == dict(Total=25 + 3 + 6), me
    for cur in (0, 2):
        me = dict(Slots=[10, 20, 30], Cursor=cur)
        run(base, 'BumpSlot', self_vars=me, By=-7)
        want = [10, 20, 30]
        want[cur] -= 7
        assert me == dict(Slots=want, Cursor=cur + 1), me
    me = dict(Cursor=2**31 - 1)
    assert run(base, 'NextSlot', self_vars=me)[0] == 2**31 - 1 and me == dict(Cursor=-2**31), me
    for cur in (0, 5, -4):
        for x in (-2, 7, 10**9):
            me = dict(Cursor=cur)
            got = run(base, 'TemplateMember', self_vars=me, X=x)[0]
            assert got == i32(2 * i32(3 * x + cur)) and me == dict(Cursor=cur), (cur, x, got)
    for fn, want in (('FreshLocals', 1000), ('FreshInLoop', 600)):
        me = dict(Total=777, Cursor=5, Slots=[1])
        assert run(base, fn, self_vars=me, N=3)[0] == want and me == dict(Total=777, Cursor=5, Slots=[1]), (fn, me)
    print('ok  FlowTest: BeginPlay, BumpSlot, NextSlot, TemplateMember and local Totals, on the members')


def assign_order():
    """C++17 sequences the right side of `=` / `op=` before the left, and the engine's Let locates its destination
    (object, array, index) before the value: what the value runs must not move past what locates the store."""
    from runscript import i32
    base = asset('FlowTest')
    for cur in (0, 1, 2):
        me = dict(Slots=[9] * 4, Cursor=cur)
        run(base, 'StoreSlot', self_vars=me)
        want = [9] * 4
        want[cur + 1] = cur
        assert me == dict(Slots=want, Cursor=cur + 2), ('StoreSlot', cur, me)
    for cur in (0, 1, 2**31 - 1):
        me = dict(SlotMap={}, Cursor=cur)
        run(base, 'StoreSlotMap', self_vars=me)
        assert me == dict(SlotMap={i32(cur + 1): cur}, Cursor=i32(cur + 2)), ('StoreSlotMap', cur, me)
    for i in (0, 1, 2):
        me = dict(Slots=[9] * 4)
        assert run(base, 'StorePostInc', self_vars=me, I=i)[0] == i + 1 and me['Slots'][i] == i, ('StorePostInc', i, me)
    for cur in (0, 1, 2):
        me = dict(Slots=[10, 20, 30, 40], Cursor=cur)
        run(base, 'StoreAtCursor', self_vars=me)
        want = [10, 20, 30, 40]
        want[cur + 1] = cur
        assert me == dict(Slots=want, Cursor=cur + 1), ('StoreAtCursor', cur, me)
        me = dict(Slots=[10, 20, 30, 40], Cursor=cur)
        run(base, 'BumpAtCursor', self_vars=me, By=100)
        want = [10, 20, 30, 40]
        want[cur + 1] += cur + 100
        assert me == dict(Slots=want, Cursor=cur + 1), ('BumpAtCursor', cur, me)
    for fn in ('StorePeer', 'StorePeerField'):
        vm = VM(base)
        near, far = vm.new(Cursor=0), vm.new(Cursor=0)
        vm.self.vars.update(Peer=near, Spare=far)
        vm.call(fn)
        assert near.vars['Cursor'] == 0 and far.vars['Cursor'] == 5 and vm.self.vars['Peer'] is far, (fn, near.vars, far.vars)
    print('ok  FlowTest: `=` / `op=` take the value before locating the destination\'s index, key or object')


def call_object_order():
    """C++17 sequences a call's object (and E1 of E1[E2]) before the arguments: what an argument hoists (an inline
    body, a && / ?: arm) must not run before the object is taken."""
    base = asset('FlowTest')
    for fn, args, cursor, ret in (('CallPeerInline', {}, 5, None), ('CallPeerBranch', dict(C=True), 5, None),
                                  ('CallPeerBranch', dict(C=False), 1, None), ('CallPeerField', {}, 5, None),
                                  ('PeerPlus', {}, 3, 8), ('PeerSlot', {}, 3, 10), ('StorePeerSlot', {}, 3, None)):
        vm = VM(base)
        near, far = vm.new(Cursor=3, Slots=[10, 11]), vm.new(Cursor=4, Slots=[20, 21])
        vm.self.vars.update(Peer=near, Spare=far)
        got = vm.call(fn, **args)
        swapped = fn != 'CallPeerBranch' or args['C']
        assert (got == ret and near.vars['Cursor'] == cursor and far.vars['Cursor'] == 4
                and near.vars['Slots'] == ([7, 11] if fn == 'StorePeerSlot' else [10, 11]) and far.vars['Slots'] == [20, 21]
                and vm.self.vars['Peer'] is (far if swapped else near)), (fn, args, got, near.vars, far.vars)
    print('ok  FlowTest: a call\'s object is taken before what its arguments hoist  (7 cases)')
    # ... and the object holding the container or dispatcher a Kismet call works on (its argument 0).
    for fn, args, slots, smap, cursor, ret in (
            ('AddPeerSlot', {}, [10, 11, 5], {5: 1}, 3, None), ('AddPeerSlotBranch', dict(C=True), [10, 11, 5], {5: 1}, 3, None),
            ('AddPeerSlotBranch', dict(C=False), [10, 11, 1], {5: 1}, 3, None), ('AddPeerSlotRaw', {}, [10, 11, 5], {5: 1}, 3, None),
            ('AddPeerMap', {}, [10, 11], {5: 7}, 3, None), ('PeerMapAt', {}, [10, 11], {5: 1}, 3, 1),
            ('StorePeerMap', {}, [10, 11], {5: 7}, 3, None), ('FirePeer', {}, [10, 11], {5: 1}, 5, None)):
        vm = VM(base)
        near = vm.new(Cursor=3, Slots=[10, 11], SlotMap={5: 1})
        far = vm.new(Cursor=4, Slots=[20, 21], SlotMap={5: 9})
        vm.self.vars.update(Peer=near, Spare=far)
        vm.binds += [(near, 'OnPeerHit', 'SetCursor', near), (far, 'OnPeerHit', 'SetCursor', far)]
        got = vm.call(fn, **args)
        swapped = fn != 'AddPeerSlotBranch' or args['C']
        assert (got == ret and near.vars == dict(Cursor=cursor, Slots=slots, SlotMap=smap)
                and far.vars == dict(Cursor=4, Slots=[20, 21], SlotMap={5: 9})
                and vm.self.vars['Peer'] is (far if swapped else near)), (fn, args, got, near.vars, far.vars)
    print('ok  FlowTest: a container or dispatcher call\'s object is taken before what its arguments hoist  (8 cases)')


def call_member():
    """A member of a returned struct: Translation.Y of the transform GetTransform returns."""
    import runscript
    runscript.MATH['GetTransform'] = lambda: {'Translation': {'X': 1.0, 'Y': 2.5, 'Z': -3.0}, 'Scale3D': {'X': 7.0, 'Y': 8.0, 'Z': 6.0}}
    try:
        assert run(asset('FlowTest'), 'CallMember')[0] == 2.5
    finally:
        del runscript.MATH['GetTransform']
    print('ok  FlowTest.CallMember: Translation.Y of GetTransform()')


def float_step():
    import math
    check('FlowTest', 'FloatStep', lambda F: -(F + 1.5), [dict(F=f) for f in (0.0, 2.25, -1.5, -0.5, 100.75)])
    r = run(asset('FlowTest'), 'FloatStep', F=-1.5)[0]
    assert math.copysign(1, r) == -1, 'FloatStep(-1.5) = %r: C++ -F of +0.0 is -0.0' % r
    print('ok  FlowTest.FloatStep: -F flips the sign of zero too')


def flow_exports():
    """Inline and template members are expanded at their calls: none of them is a UFunction."""
    names = {e['name'] for e in dumpexp.load(asset('FlowTest'))[5]}
    for name in ('TwicePlus', 'SumTo', 'TwoOf', 'FirstSquareAbove', 'StopBelow', 'Scaled', 'WidthOf'):
        assert name not in names, name + ' became a UFunction'
    print('ok  FlowTest: no inline or template member is a UFunction')


CLUSTERS = {0: 1, 1: 2, 2: 3, 5: 6, 9: 10, 50: 500, 100: 1000, 103: 1003, 104: 1004, 110: 1010, 7000: 7}
clusters()
check('FlowTest', 'DenseByte', lambda Mode: {0: 3, 1: 4, 2: 5}.get(Mode, 0), [dict(Mode=m) for m in (0, 1, 2, 3, 255)])


def compound(N):
    acc = 1
    for i in range(N):
        acc += i; acc *= 2; acc -= 1; acc = cmod(acc, 1000)
    return acc + N - 2


def while_and(Limit):
    i = hits = 0
    while i < Limit and cdiv(100, Limit - i) > 1:
        hits += 1; i += 1
    return hits * 1000 + i


check('FlowTest', 'Arith', lambda A, B: cdiv(A, B) + cmod(A, B) * 10 + A + ~B,
      [dict(A=a, B=b) for a in (-7, 0, 7, 100) for b in (-3, 1, 3)])
check('FlowTest', 'PostInc', lambda X: X * 100 + X + 1, [dict(X=x) for x in (-1, 0, 5)])
check('FlowTest', 'PreInc', lambda X: (X + 1) * 101, [dict(X=x) for x in (-1, 0, 5)])
check('FlowTest', 'UpdateChain', lambda X: (1 + X) * 11, [dict(X=x) for x in (-1, 0, 4)])
check('FlowTest', 'OrAssign', lambda N: 11 if N > 0 else 0, [dict(N=n) for n in (-1, 0, 3)])
check('FlowTest', 'TemplateMember', lambda X: X * 6, [dict(X=x) for x in (-2, 0, 7)])
check('FlowTest', 'ConstexprPick', lambda: 84, [dict()])
check('FlowTest', 'CoalesceLoop', lambda N: sum((2 * i + 1) + (2 * (i + 1) + 1) for i in range(2 * N + 1)) * 1000 + 2 * N + 1,
      [dict(N=n) for n in (-1, 0, 1, 3)])
check('FlowTest', 'LoopInline', lambda N: max(N, 0) ** 2 * 100 + max(2 * N + 1, 0) ** 2, [dict(N=n) for n in (-3, 0, 1, 4)])
check('FlowTest', 'FreshLocals', lambda N: 400 + 200 * max(N, 0), [dict(N=n) for n in (0, 1, 3)])
check('FlowTest', 'FreshInLoop', lambda N: 200 * max(N, 0), [dict(N=n) for n in (0, 1, 3)])
check('FlowTest', 'SafeRatio', lambda X: X != 0 and cdiv(10, X) > 2, [dict(X=x) for x in (-2, 0, 1, 3, 4)])
check('FlowTest', 'EitherZero', lambda X, Y: X == 0 or cdiv(100, X) == Y, [dict(X=x, Y=y) for x in (0, 10, 3) for y in (0, 10, 33)])
check('FlowTest', 'Pick', lambda X: X * 2 if X > 0 else (-1 if X < -5 else 7), [dict(X=x) for x in (-9, -5, 0, 4)])
check('FlowTest', 'ConstBreak', lambda X: 205, [dict(X=0)])
vm = VM(asset('FlowTest'))
assert [vm.call('UseDefault', X=x) for x in (-2, 0, 5)] == [(x * 3 + x * 10 + (x + 7) * 1000) for x in (-2, 0, 5)]
print('ok  FlowTest.UseDefault: a defaulted argument is the parameter\'s default, to a method and inlined')
check('FlowTest', 'RefRvalue', lambda X: 5 + (2 * X + 1) + (X + 1), [dict(X=x) for x in (-4, 0, 9)])   # runvm refuses a non-variable
check('FlowTest', 'Empty', lambda N: max(N, 1) + {1: 30, 2: 20}.get(N, 0) + (100 if N >= 0 else 0), [dict(N=n) for n in (-3, 0, 1, 2, 5)])
check('FlowTest', 'ForParts', lambda N: max(N, 0) + 10 * (sum(j for j in range(N) if j != 1) + 1000 * max(N, 0)), [dict(N=n) for n in (-2, 0, 1, 2, 6)])
check('FlowTest', 'ForCondVar', lambda N: sum(l for l in range(1, N + 1) if l != 2), [dict(N=n) for n in (0, 1, 2, 5)])
check('FlowTest', 'Compound', compound, [dict(N=n) for n in (0, 1, 5, 40)])
check('FlowTest', 'WhileAnd', while_and, [dict(Limit=l) for l in (0, 1, 50, 99, 150)])


def do_continue(Limit):
    i = s = 0
    while True:
        i += 1
        if i % 2 != 0:
            if i > 9: break
            s += i
        if not i < Limit: break
    return s * 100 + i


def goto_out(Size, Want):
    for y in range(Size):
        for x in range(Size):
            if x * y == Want: return y * 100 + x
    return -2


def if_init(V):
    twice = V * 2
    r = twice if twice > 10 else -twice
    return r + cmod(V, 3) * 1000


def switch_init(V):
    k, m = V + 1, cmod(V, 4)
    if k == 1: return 10
    if k == 2: return 20 + k
    return 300 + m if m == 3 else m


def while_var(Start):
    steps = 0
    while Start - steps != 0:
        left = Start - steps
        steps += 1
        if left < 0: break
    return steps


check('FlowTest', 'DoOnce', lambda Limit: max(Limit, 1), [dict(Limit=l) for l in (-3, 0, 1, 2, 9)])
check('FlowTest', 'DoContinue', do_continue, [dict(Limit=l) for l in (0, 1, 2, 6, 7, 30)])
check('FlowTest', 'GotoLoop', lambda N: sum(range(max(N, 0))), [dict(N=n) for n in (-1, 0, 1, 5, 40)])
check('FlowTest', 'GotoOut', goto_out, [dict(Size=s, Want=w) for s in (0, 1, 4) for w in (0, 6, 7)])
def first_square_above(floor): return next(n for n in range(1, 100) if n * n > floor)
check('FlowTest', 'GotoInlined', lambda A, B: first_square_above(A) * 100 + first_square_above(B), [dict(A=a, B=b) for a, b in ((0, 0), (10, 50), (99, 3))])
check('FlowTest', 'GotoInlinedLive', lambda N: 3 * N * 3 + 3, [dict(N=n) for n in (-4, 0, 1, 5)])
check('FlowTest', 'GotoRedeclares', lambda Rounds: 5 * max(Rounds, 1), [dict(Rounds=r) for r in (0, 1, 3)])
check('FlowTest', 'IfInit', if_init, [dict(V=v) for v in (-4, 0, 3, 5, 6, 8)])
check('FlowTest', 'SwitchInit', switch_init, [dict(V=v) for v in (-1, 0, 1, 2, 3, 7)])
check('FlowTest', 'WhileVar', while_var, [dict(Start=s) for s in (-2, 0, 1, 5)])
flow_members()
assign_order()
call_object_order()
call_member()
float_step()
flow_exports()


# ---- RangeTest

def double_in_place(f):
    items = f['Items']
    for i, x in enumerate(items):
        if x < 0: continue
        items[i] = x * 2
        if items[i] > 100: break
    s = 0
    for x in items: s = s * 3 + x
    return s


def bump_scores(stop):
    def o(f):
        count = 0
        for k in list(f['Scores']):
            f['Scores'][k] += 10
            count += 1
            if f['Scores'][k] > stop: break
        return sum(f['Scores'].values()) * 100 + count
    return o


arrays = [[], [1], [3, -1, 7], [60, 2, -5, 9], [1, 2, 3, 4, 5]]
check_self('RangeTest', 'SumArray', lambda f: sum(f['Items']), [dict(Items=a) for a in arrays])
check_self('RangeTest', 'SumScaled', lambda f: 3 * sum(f['Items']), [dict(Items=a) for a in arrays])
check_self('RangeTest', 'DoubleInPlace', double_in_place, [dict(Items=a) for a in arrays])
check_self('RangeTest', 'CopyDoesNotWrite', lambda f: len(f['Items']), [dict(Items=a) for a in arrays])
check_self('RangeTest', 'NestedPairs', lambda f: sum(1 for a in f['Items'] for b in f['Items'] if a < b), [dict(Items=a) for a in arrays])
check_self('RangeTest', 'SumSet', lambda f: sum(f['Seen']), [dict(Seen=a) for a in ([], [4], [1, 5, 9])])
for stop in (0, 15, 1000):
    fn = bump_scores(stop)
    maps = [{}, {'a': 1}, {'a': 1, 'b': 20, 'c': 3}]
    for m in maps:
        mine = range_self(Scores=m)
        got = run(asset('RangeTest'), 'BumpScores', self_vars=mine, Stop=stop)[0]
        theirs = range_self(Scores=m)
        want = fn(theirs)
        assert (got, mine) == (want, theirs), 'BumpScores(%s, %s) = %r %r, want %r %r' % (m, stop, got, mine, want, theirs)
print('ok  RangeTest.BumpScores  (9 cases)')


def bump_until(f, stop):
    for k in f['Scores']:
        f['Scores'][k] += 10
        for i in range(2):
            if f['Scores'][k] + i > stop: return f['Scores'][k] * 10 + i
    return -1


def cap_scores(f, cap):
    for k in f['Scores']:
        f['Scores'][k] += 1
        if f['Scores'][k] > cap:
            f['Scores'][k] = cap
            return None


def bump_inlined(f, stop):
    for k in f['Scores']:
        f['Scores'][k] += 1
        if f['Scores'][k] > stop: return f['Scores'][k] * 2 + 1
    return -1 * 2 + 1


n = 0
for fn, oracle in (('BumpScoresUntil', bump_until), ('CapScores', cap_scores), ('BumpScoresInlined', bump_inlined)):
    for stop in (-100, 0, 5, 15, 25, 1000):
        for m in ({}, {'a': 1}, {'a': 1, 'b': 20, 'c': 3}, {'a': -50, 'b': 6}):
            mine, theirs = range_self(Scores=m), range_self(Scores=m)
            got = run(asset('RangeTest'), fn, self_vars=mine, **{'Cap' if fn == 'CapScores' else 'Stop': stop})[0]
            want = oracle(theirs, stop)
            assert (got, mine) == (want, theirs), '%s(%s, %s) = %r %r, want %r %r' % (fn, m, stop, got, mine, want, theirs)
            n += 1
print('ok  RangeTest: a return from a reference TMap loop writes the changed value back  (%d cases)' % n)


def seen_in_body(f, d):
    s = 0
    for k in f['Scores']:
        f['Scores'][k] = i32(f['Scores'][k] + d)
        s = i32(s + f['Scores'][k])
        f['Scores'][k] = i32(i32(f['Scores'][k] * 2) + 1)
        s = i32(s + i32(f['Scores'][k] * 100))
    return s


n = 0
for d in (0, 2, -7, 2**31 - 1):
    for m in ({}, {'a': 30}, {'a': 1, 'b': 20, 'c': 3}):
        mine, theirs = range_self(Scores=m), range_self(Scores=m)
        got = run(asset('RangeTest'), 'SeenInBody', self_vars=mine, D=d)[0]
        want = seen_in_body(theirs, d)
        assert (got, mine) == (want, theirs), ('SeenInBody', m, d, got, mine, want, theirs)
        n += 1
print("ok  RangeTest.SeenInBody: `auto& [K, V]` is the map's own value, a write through either name read through the other  (%d cases)" % n)
for m in ({}, {'a': {'X': 1, 'Y': 2}}, {'a': {'X': -1, 'Y': 0}, 'b': {'X': 5, 'Y': 7}}):
    f = dict(Spots={k: dict(v) for k, v in m.items()})
    got = run(asset('RangeTest'), 'ShiftSpots', self_vars=f)[0]
    want = {k: dict(v, X=v['X'] + 1) for k, v in m.items()}
    assert (got, f['Spots']) == (sum(v['X'] * 10 + v['Y'] for v in want.values()), want), (m, got, f)
print('ok  RangeTest.ShiftSpots: a struct value changed through `.` goes back to the map')
for hits in ((), (0,), (4, -1)):
    peers = {'p%d' % i: Obj('RangeTest_C', Hits=h) for i, h in enumerate(hits)}
    vm = VM(asset('RangeTest'), Peers=dict(peers))
    vm.call('PokePeers')
    assert vm.self.vars['Peers'] == peers and [p.vars['Hits'] for p in peers.values()] == [h + 1 for h in hits], hits
print('ok  RangeTest.PokePeers: `Peer->Hits += 1` writes each object, the map keeps its pointers')
for items, scores in (([], {}), ([1, 2, 3], {'a': 5}), ([4, -7], {'a': 1, 'b': -2})):
    def peers():
        return (Obj('RangeTest_C', Items=list(items), Scores=dict(scores)),
                Obj('RangeTest_C', Items=[100, 200, 300, 400], Scores={'a': 50, 'z': 9}))
    near, far = peers()
    vm = VM(asset('RangeTest'), Near=near, Far=far, Picks=0)
    want = 0
    for x in items: want = want * 10 + x
    assert vm.call('SumPicked') == want * 100 + 1, (items, vm.self.vars['Picks'])
    for fn, field, change in (('DoublePicked', 'Items', lambda: [x * 2 for x in items]),
                              ('BumpPicked', 'Scores', lambda: {k: v + 1 for k, v in scores.items()})):
        near, far = peers()
        vm = VM(asset('RangeTest'), Near=near, Far=far, Picks=0)
        vm.call(fn)
        got = (near.vars['Items'], near.vars['Scores'], far.vars, vm.self.vars['Picks'])
        want = dict(Items=list(items), Scores=dict(scores))
        want[field] = change()
        assert got == (want['Items'], want['Scores'], peers()[1].vars, 1), (fn, items, scores, got)
print('ok  RangeTest: a range expression with a call is evaluated once  (9 cases)')
for items, scores in (([], {}), ([1, 2, 3], {'a': 5, 'b': 7}), ([4, -7], {'b': -2})):
    def peers():
        return (Obj('RangeTest_C', Items=list(items), Scores=dict(scores)),
                Obj('RangeTest_C', Items=[100, 200, 300, 400], Scores={'z': 9}))
    near, far = peers()
    vm = VM(asset('RangeTest'), Near=near, Far=far, Cur=near)
    want = 0
    for x in items + items: want = want * 10 + x
    got = vm.call('SumReseat')
    assert got == want, ('SumReseat', items, got, want)
    for fn, field, change in (('DoubleReseat', 'Items', lambda: [x * 2 for x in items]),
                              ('BumpReseat', 'Scores', lambda: {k: v + 1 for k, v in scores.items()})):
        near, far = peers()
        vm = VM(asset('RangeTest'), Near=near, Far=far, Cur=near)
        vm.call(fn)
        got = (near.vars['Items'], near.vars['Scores'], far.vars)
        want = dict(Items=list(items), Scores=dict(scores))
        want[field] = change()
        assert got == (want['Items'], want['Scores'], peers()[1].vars), (fn, items, scores, got)
print('ok  RangeTest: a pointer reseated in a range-for body leaves the range where it was  (9 cases)')


def lists_in_place(f, d):
    s = 0
    for k in f['Lists']:
        f['Lists'][k].append(i32(k * d))
        more = list(f['Lists'][k])
        s = i32(s + len(more))
        f['Lists'][k] = v = more + [7]
        v[:] = [i32(x + 1) for x in v]
        s = i32(s + len(v) * 10 + i32(v[-1] * 100))
    return s


def const_sees_store(f):
    s = 0
    for k in f['Lists']:
        f['Lists'][k] = f['Lists'][k] + [1]
        s += len(f['Lists'][k])
    return s


def add_grown(f, k):
    f['Lists'][k].append(k)                 # Grow(K), the argument, first
    f['Lists'][k].append(len(f['Lists'][k]))
    return len(f['Lists'][k]) * 100 + f['Lists'][k][-1]


def nudge_spots(f, by):
    s = 0
    for p in f['Spots'].values():
        p['X'], p['Y'] = i32(p['X'] + by), i32(p['Y'] - by)
        s = i32(s + i32(p['X'] * 10) + p['Y'])
    return s


def drop_negatives(f):
    kept = 0
    for k in list(f['Scores']):
        if f['Scores'][k] < 0:
            del f['Scores'][k]
            continue
        f['Scores'][k] = i32(f['Scores'][k] + 1)
        kept += 1
    return kept


def walk_self(**kw):
    return dict(Scores=copy.deepcopy(kw.get('Scores', {})), Lists=copy.deepcopy(kw.get('Lists', {})),
                Spots=copy.deepcopy(kw.get('Spots', {})))


n = 0
for fn, oracle, parms, cases in (
        ('ListsInPlace', lists_in_place, [dict(D=d) for d in (0, 3, -2)], [dict(Lists=m) for m in ({}, {1: []}, {1: [5], 4: [2, 9]})]),
        ('ConstSeesStore', const_sees_store, [{}], [dict(Lists=m) for m in ({}, {3: []}, {1: [5], 4: [2, 9]})]),
        ('AddGrown', add_grown, [dict(K=4)], [dict(Lists=m) for m in ({4: []}, {1: [5], 4: [2, 9]})]),
        ('NudgeSpots', nudge_spots, [dict(By=b) for b in (0, 3)],
         [dict(Spots=m) for m in ({}, {'a': {'X': 1, 'Y': 2}, 'b': {'X': -5, 'Y': 7}})]),
        ('DropNegatives', drop_negatives, [{}], [dict(Scores=m) for m in ({}, {'a': -1}, {'a': 1, 'b': -20, 'c': 3, 'd': -4})])):
    for p in parms:
        for c in cases:
            mine, theirs = walk_self(**c), walk_self(**c)
            got = run(asset('RangeTest'), fn, self_vars=mine, **p)[0]
            want = oracle(theirs, *p.values())
            assert (got, mine) == (want, theirs), (fn, c, p, got, mine, want, theirs)
            n += 1
# A map that lost elements has free slots in its sparse array: the walk packs it first, and never reads one.
for m, holes in (({'a': 1, 'b': 20, 'c': 3}, (0, 2)), ({'a': 5}, (1,)), ({}, (0,))):
    mine, theirs = range_self(), range_self(Scores=m)
    mine['Scores'] = runscript.Holey(m, holes)
    got = run(asset('RangeTest'), 'SeenInBody', self_vars=mine, D=2)[0]
    assert (got, mine) == (seen_in_body(theirs, 2), theirs), (m, holes, got, mine)
    n += 1
mine, theirs = walk_self(), walk_self(Lists={1: [5], 4: [2, 9]})
mine['Lists'] = runscript.Holey({1: [5], 4: [2, 9]}, (1,))
assert (run(asset('RangeTest'), 'ListsInPlace', self_vars=mine, D=3)[0], mine) == (lists_in_place(theirs, 3), theirs), mine
# V is the value itself, so a T& binds it: no copy, and no warning that there is one.
assert 'Blueprint has no reference to it' not in LOGS['RangeTest'], LOGS['RangeTest']
print("ok  RangeTest: `auto& [K, V]` over a TMap walks its slots in place, a container value and a T& to V included;"
      " `Lists[K].Add(X)` runs on a copy stored back, X first  (%d cases)" % (n + 1))


def range_members():
    """Range-for side effects on the members: a by-value loop variable writes nothing, a reference one writes
    exactly the elements the loop reached."""
    for a in arrays + [[51, 1], [200, 7]]:
        for fn in ('SumArray', 'SumScaled', 'CopyDoesNotWrite', 'NestedPairs'):
            mine = range_self(Items=a)
            run(asset('RangeTest'), fn, self_vars=mine)
            assert mine == range_self(Items=a), (fn, a, mine)
        mine, theirs = range_self(Items=a), range_self(Items=a)
        got = run(asset('RangeTest'), 'DoubleInPlace', self_vars=mine)[0]
        assert got == double_in_place(theirs) and mine == theirs, (a, got, mine, theirs)
    for s in ([], [4], [1, 5, 9]):
        mine = range_self(Seen=s)
        assert run(asset('RangeTest'), 'SumSet', self_vars=mine)[0] == sum(s) and mine == range_self(Seen=s)
    print('ok  RangeTest: members written only through a reference loop variable')


range_members()


# ---- InlineTest

def first_above(limit): return next((i for i in range(100) if i * i > limit), -1)
def nest(v): return v * 4 + clamp(v, 0, 10)


check('InlineTest', 'UseClamp', lambda V: clamp(V, -5, 5) + V * 2 + cdiv(V, 2), [dict(V=v) for v in (-9, -1, 0, 3, 7)])
check('InlineTest', 'UseLoop', lambda L: sum(first_above(L + i) for i in range(3)), [dict(L=l) for l in (0, 5, 50, 9990)])
check('InlineTest', 'UseNest', lambda V: nest(V) + nest(V + 1), [dict(V=v) for v in (-2, 0, 4, 12)])
check('InlineTest', 'UseStatic', lambda V: i32(V * 2 + V - 1 + 6), [dict(V=v) for v in (-3, 0, 7, 2**30)])
check('InlineTest', 'StaticFromInst', lambda V: i32(V * 2 + V - 1), [dict(V=v) for v in (-3, 0, 7, 2**30)])
check('InlineTest', 'TwiceTwice', lambda V: i32(4 * V + 12), [dict(V=v) for v in (-2, 0, 5, 2**29, -2**31)])
check('InlineTest', 'ClampInArg', lambda V: max(4, clamp(V, 0, 10)), [dict(V=v) for v in (-3, 0, 2, 4, 5, 8, 9, 10, 11, 15)])
check('InlineTest', 'InCond', lambda V: 1 if i32(V * 2) > 10 and clamp(V, 0, 3) == 3 else 0,
      [dict(V=v) for v in (-7, 0, 5, 6, 9, 2**30)])
check('InlineTest', 'InPlace', lambda V: V * 3 + 1 + V + 1, [dict(V=v) for v in (-3, 0, 7)])
check('InlineTest', 'ConstThenVar', lambda V: 6 + V * 2 + clamp(V, 0, 5) + clamp(2, V, 9) + 4 + V - 1, [dict(V=v) for v in (-3, 0, 4, 12)])


def inline_members():
    """Side effects through an inline body land on self, once per expansion, whatever Counter started at."""
    for v in (0, 2, -4, 2**30):
        for c in (0, 5):
            fields = dict(Counter=c)
            got = run(asset('InlineTest'), 'UseBump', self_vars=fields, V=v)[0]
            assert got == i32(i32(v + 3 + v) * 100 + c + 2) and fields == dict(Counter=c + 2), ('UseBump', v, c, got, fields)
            fields = dict(Counter=c)
            got = run(asset('InlineTest'), 'KeptLocal', self_vars=fields, V=v)[0]
            assert got == i32(v * 3) and fields == dict(Counter=c + 1), ('KeptLocal', v, c, got, fields)
    print('ok  InlineTest: UseBump / KeptLocal side effects on Counter')


def inline_statics():
    """A static function stays FUNC_Static (callable with no instance); an instance one does not."""
    base = asset('InlineTest')
    flags = lambda fn: int(re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base, export_index(base, fn))).group(1), 16)
    assert flags('UseStatic') & 0x2000 and not flags('StaticFromInst') & 0x2000, (hex(flags('UseStatic')), hex(flags('StaticFromInst')))
    print('ok  InlineTest: static stays static')


def no_inline_ufunctions():
    exports = [e['name'] for e in dumpexp.load(asset('InlineTest'))[5]]
    for name in ('Clamp', 'Half', 'Twice', 'Bump', 'FirstAbove', 'Nest', 'Dec', 'Plus1', 'Late', 'SDouble', 'SPred', 'Pick'):
        assert name not in exports, name + ' became a UFunction'
    print('ok  InlineTest: no inline function is a UFunction')


def inline_regressions():
    """Miscompiles that once shipped, run: a by-value argument read after the body changed its source, a T& bound to an
    array element (its index a call, run once), and a do/while whose test is an inline call."""
    for c in (0, 5):
        f = dict(Counter=c)
        assert run(asset('InlineTest'), 'LateMember', self_vars=f)[0] == c * 100 + c + 1 and f == dict(Counter=c + 1), (c, f)
    for by in (-3, 0, 4):
        f = dict(Counter=5, Calls=0, Arr=[])
        got = run(asset('InlineTest'), 'BumpElem', self_vars=f, By=by)[0]
        assert got == (10 + by) * 100 + 10 + 6 and f == dict(Counter=6, Calls=1, Arr=[10 + by]), (by, got, f)
    check('InlineTest', 'DoInline', lambda N: next(i for i in range(1, 100) if 2 * i >= N), [dict(N=n) for n in (-3, 0, 1, 2, 4, 5, 12)])
    check('InlineTest', 'PickOverloads', lambda V, B: i32((100 if B else 200) + (V + 1) * 1000 + (V + 1) * 3 * 10),
          [dict(V=v, B=b) for v in (-5, 0, 7, 2**20) for b in (True, False)])
    print('ok  InlineTest: LateMember, BumpElem, DoInline and PickOverloads run as C++ does')
    Ls = [dict(L=l) for l in (-2, 0, 1, 3, 5, 9)]
    check('InlineTest', 'WhileFresh', lambda L: max(0, L - 1), Ls)
    check('InlineTest', 'ForFresh', lambda L: sum(range(L - 1)), Ls)
    check('InlineTest', 'DoFresh', lambda L: max(1, L - 1), Ls)
    print('ok  InlineTest: an inline in a loop test makes its locals afresh on every trip')


def inline_mixed_overloads():
    """A call to the non-inline overload of a name Generate skips as inline is refused: it once compiled to a call to a
    UFunction that was never made, fatal at run time. An inline overload beside the UFunction one still runs."""
    import tempfile
    head = '#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/%s");\nclass %s : public AActor {\npublic:\n'
    mods = {'MixShort': ('  int32 Get(int32 A) { return A + 7; }\n  inline int32 Get(int32 A, int32 B) { return A * B; }\n'
                         '  int32 CallShort(int32 V) { return Get(V); }\n};\n'),
            'MixLong': ('  int32 Get(int32 A, int32 B) { return A * B; }\n  int32 Get(int32 A);\n'
                        '  int32 CallLong(int32 V) { return Get(V, 3); }\n};\ninline int32 MixLong::Get(int32 A) { return A + 1; }\n'),
            'MixOk': ('  int32 Get(int32 A, int32 B) { return A * B; }\n  inline int32 Get(int32 A) { return A + 1; }\n'
                      '  int32 Both(int32 V) { return Get(V) * 10 + Get(V, 2); }\n};\n')}
    with tempfile.TemporaryDirectory() as tmp:
        for mod, body in mods.items():
            with open(os.path.join(tmp, mod + '.cpp'), 'w') as f:
                f.write(head % (mod, mod) + body)
            proc = assetgen_compile([os.path.join(tmp, mod + '.cpp'), UEAPI, tmp])
            if mod == 'MixOk':
                assert proc.returncode == 0, proc.stdout + proc.stderr
                for v in (-3, 0, 5):
                    assert run(os.path.join(tmp, mod), 'Both', V=v)[0] == (v + 1) * 10 + v * 2, v
            else:
                assert proc.returncode != 0 and 'may not mix inline and non-inline' in proc.stdout, (mod, proc.stdout)
    print('ok  InlineTest: an overload set mixing inline and non-inline is refused where it would call no UFunction')


def copy_back():
    """A written T& bound to what Blueprint has no reference to (a map element, `C ? X : Y`) gets a copy, stored back
    after the call into the place picked at the call, and a warning says it is a copy."""
    t = asset('InlineTest')
    for c in (0, 5):
        for k in (1, 2):
            f, m = dict(Counter=c), {1: 10, 2: 20}
            m[k] += 8
            got = run(t, 'RefMap', self_vars=f, K=k)[0]
            assert got == m[1] * 100 + m[2] and f == dict(Counter=c + 1), ('RefMap', k, c, got, f)
        for sel in (True, False):
            f = dict(Counter=c)
            got = run(t, 'RefSel', self_vars=f, C=sel)[0]
            assert got == (1602 if sel else 117) and f == dict(Counter=c + 1), ('RefSel', sel, c, got, f)
        for by in (-3, 4):
            vm = VM(t, Counter=c, Calls=7)
            got = vm.call('RefObj', By=by)
            assert got == (7 + by) * 10 + c + 1 and vm.self.vars == dict(Counter=c + 1, Calls=7 + by), ('RefObj', by, c, got, vm.self.vars)
    # An inline's T& bound to `O->A` is A itself, through O pinned at the call: no copy, so no warning.
    vm = VM(t, Calls=7)
    assert vm.call('RefObjLive') == 12 and vm.self.vars == dict(Calls=12), vm.self.vars
    for by in (-3, 4):
        vm = VM(t, Counter=5, Calls=0, Arr=[])
        got = vm.call('RefDeep', By=by)
        assert got == (10 + by) * 100 + 10 + 6 and vm.self.vars == dict(Counter=6, Calls=1, Arr=[10 + by]), ('RefDeep', by, got, vm.self.vars)
    assert 'InlineTest::RefObj' not in LOGS['InlineTest'] and 'InlineTest::RefDeep' not in LOGS['InlineTest'], LOGS['InlineTest']
    f = dict(Key=9)
    got = run(t, 'RefPinned', self_vars=f)[0]
    assert got == 15 * 1000000 + 15 * 10000 + 40 * 100 + 3 and f == dict(Key=5), ('RefPinned', got, f)
    for fn, callee, what in (('RefMap', 'Add5', 'a map element'), ('RefMap', 'InlineTest::Bump', 'a map element'),
                             ('RefSel', 'Add5', '`C ? X : Y`'), ('RefSel', 'InlineTest::Bump', '`C ? X : Y`'),
                             ('RefPinned', 'AddKey', 'a map element')):
        line = "warning: InlineTest::%s: %s's reference parameter V is bound to %s" % (fn, callee, what)
        assert line in LOGS['InlineTest'], (line, LOGS['InlineTest'])
    # Both sides of `C ? X : Y` are located before the call, so one found by a call, which C++ runs only when picked, is refused.
    refused('RefSelCall', '  void Add5(int32& V) { V += 5; }\n  int32 K() { return 1; }\n'
            '  int32 F(bool C) { TMap<int32, int32> M; int32 X = 1; Add5(C ? M[K()] : X); return X; }\n', 'found by a call')
    print('ok  InlineTest: a written reference bound to a map element or `C ? X : Y` is copied in and back, and warned; '
          'an inline one bound to `O->A` is A itself')


inline_members()
inline_statics()
no_inline_ufunctions()
inline_regressions()
inline_mixed_overloads()
copy_back()


# ---- NsTest

def namespaces():
    """A namespace is the asset's folder: one under the mod's package, or a /Game path itself when it starts at Game.
    What names a namespaced class - a child, a call, a property's type, the registry - names that path."""
    content, mine = os.path.join(ROOT, 'NsTest', 'FSD', 'Content'), '/Game/_ElytrasMods/NsTest'
    base = lambda package: os.path.join(content, *package[len('/Game/'):].split('/'))
    where = {mine + '/NsTest', mine + '/Weapons/Rifle', mine + '/Weapons/ITrigger', mine + '/Weapons/FAmmo',
             mine + '/Weapons/EKind', '/Game/NsTestAbs/Pistol'}
    for package in where:
        assert os.path.exists(base(package) + '.uasset'), package
    rows = registry_rows(registry_of('NsTest'))
    assert {path.rsplit('.', 1)[0] for path, _ in rows} == where, rows
    pistol, ns = import_paths(base('/Game/NsTestAbs/Pistol')), import_paths(base(mine + '/NsTest'))
    assert mine + '/Weapons/Rifle.Rifle_C:Pull' in pistol, pistol          # the parent's Pull, in the parent's folder
    for want in (mine + '/Weapons/Rifle.Rifle_C:Load', '/Game/NsTestAbs/Pistol.Pistol_C:Pull', mine + '/Weapons/FAmmo.FAmmo'):
        assert want in ns, (want, ns)
    assert run(base(mine + '/Weapons/Rifle'), 'Pull', self_vars=dict(Shots=5), Times=3)[0] == 8
    # `Game::<the mod's own path>::X` and a plain X are one package.
    refused('NsTwice', '  int32 F() { return 1; }\n', 'would both be cooked as /Game/_ElytrasMods/NsTwice/NsTwice',
            top='namespace Game::_ElytrasMods::NsTwice { class NsTwice : public AActor { public: int32 X; }; }\n')
    print("ok  NsTest: a namespace is the asset's folder, under the mod's or a /Game path of its own; a child, a call, "
          "a type and the registry name it there, and two names for one package are refused")


namespaces()


# ---- OptTest

check('OptTest', 'Drop', lambda A, B: i32(A * B * 2), [dict(A=a, B=b) for a, b in ((2, 3), (-4, 5), (0, 0), (2**16, 2**15), (-2**31, 1))])
check('OptTest', 'Consts', lambda A: A * 3 + 3 + (10 + A), [dict(A=a) for a in (0, 4, -7)])
check('OptTest', 'Logic', lambda X, Y: (1 if X != 0 and cdiv(10, X) > 2 else 0) + (2 if X != 0 and Y != 0 else 0)
      + (10 if X > 0 and cdiv(Y, 2) > 0 else 0) + (100 if X == 0 or Y == 0 else 0),
      [dict(X=x, Y=y) for x in (0, 3, 4, -4, 20) for y in (0, 1, 2, 5, -3)])
check('OptTest', 'Raw', lambda X, Y: 1 if X != 0 and Y != 0 else 0, [dict(X=x, Y=y) for x in (0, 3) for y in (0, 5)])


def opt_locals():
    """Locals: the self property is written and the impure RandomInteger runs once with A; the result is A."""
    for a in (0, 5, -3):
        fields = {}
        del runscript.CALLS[:]
        got = run(asset('OptTest'), 'Locals', self_vars=fields, A=a)[0]
        assert got == a and fields == dict(Kept=a - 1), (a, got, fields)
        assert [c for c in runscript.CALLS if c[0] == 'RandomInteger'] == [('RandomInteger', (a,))], runscript.CALLS
    print('ok  OptTest.Locals: Kept written, RandomInteger(A) called once')


def opt_bools():
    """Bool `|` / `&` / `^` give C++'s values; both sides of `Bump(A) | Bump(B)` run, whatever A is, and a constant
    side that decides the value still leaves a side that acts to run (`Bump(B) | true`)."""
    for a, b in itertools.product((False, True), repeat=2):
        me = dict(Calls=0)
        got = run(asset('OptTest'), 'Bools', self_vars=me, A=a, B=b)[0]
        want = int(a or b) + 2 * int(a and b) + 4 * int(a != b) + 8 + 16 * int(b) + 32 * int(a or b) + 64 + 128 * 3
        assert got == want and me == dict(Calls=3), (a, b, got, want, me)
    print('ok  OptTest.Bools: bool | & ^ and their constant sides, both sides run')


def opt_forward():
    """A value moves to its one read, never past a store that reads what it changes: B is the Calls the first Tick left."""
    for a, c in itertools.product((0, 3, -4), (0, 5)):
        me = dict(Calls=c)
        got = run(asset('OptTest'), 'Forward', self_vars=me, A=a)[0]
        want = (c + 1) * 1000000 + (c + 1) * 10000 + (c + 2) * 100 + 2 * a + 1
        assert got == want and me == dict(Calls=c + 2), (a, c, got, want, me)
    print('ok  OptTest.Forward: a value moves to its one read, never past a store that reads what it changes')


def opt_in_place():
    """An inline's parameter is the caller's local only while nothing can change it: AddTo's V is L before Acc, bound to
    L, adds to it; Order, whose other argument writes L, agrees with its unoptimized twin, in an order C++ allows."""
    for x, c in itertools.product((0, 4, -3), (0, 2)):
        me = dict(Calls=c)
        got = run(asset('OptTest'), 'InPlace', self_vars=me, X=x)[0]
        want = x * 1000 + c + 1 + 3 * (x * 1000 + 2 * x)
        assert got == want and me == dict(Calls=c + 1), (x, c, got, want, me)
        order, raw = (run(asset('OptTest'), f, X=x)[0] for f in ('Order', 'OrderRaw'))
        assert order == raw and order in (x * 1000 + x + 1, (x + 1) * 1000 + x + 1), (x, order, raw)
    print("ok  OptTest.InPlace / Order: a caller's local stands in for a parameter only while nothing can change it")


def opt_slots():
    """Locals sharing a property keep their values, and an array declared without a value starts empty."""
    for n in (0, 1, 4, 7):
        total = sum(i * i + i * i // 2 for i in range(n))
        assert run(asset('OptTest'), 'Slots', N=n)[0] == (2 * total + 1) * 1000 + 3 * total, n
        assert run(asset('OptTest'), 'Arrays', N=n)[0] == 21, n
    print('ok  OptTest.Slots / Arrays: locals sharing a property keep their values; an array declared bare starts empty')


def opt_threads():
    """An `if` over an inline bool function takes the branch each return picks, from inside its loop too."""
    sq = lambda v: v >= 0 and int(v ** 0.5) ** 2 == v
    for v in (-1, 0, 1, 2, 3, 4, 8, 9, 15, 18, 24, 25):
        r = int(sq(v)) + (4 if sq(v + 1) else 2) + 8 * int(sq(v) and sq(v + 1))
        want = r if sq(2 * v) else r + 16
        assert run(asset('OptTest'), 'Threads', V=v)[0] == want, (v, want)
    print('ok  OptTest.Threads: an if over an inline bool function takes the branch each return picks')


def opt_raw():
    """UE_NO_OPTIMIZE / #pragma clang optimize off: what the source says runs, unused pure calls and the
    short-circuit included (an execution trace, not the layout)."""
    for x, y in ((0, 5), (3, 5), (3, 0), (-2, 7)):
        del runscript.CALLS[:]
        assert run(asset('OptTest'), 'Raw', X=x, Y=y)[0] == (1 if x != 0 and y != 0 else 0)
        assert ('Multiply_IntInt', (x, y)) in runscript.CALLS and ('Add_IntInt', (x, 3)) in runscript.CALLS, runscript.CALLS
        assert (('NotEqual_IntInt', (y, 0)) in runscript.CALLS) == (x != 0), runscript.CALLS      # Y != 0 runs only when X != 0
    for x in (-4, 0, 9):
        del runscript.CALLS[:]
        assert run(asset('OptTest'), 'RawPragma', X=x)[0] == x and ('Abs_Int', (x,)) in runscript.CALLS, runscript.CALLS
    print('ok  OptTest.Raw / RawPragma: everything the source says runs')


def opt_flags():
    """UE_NO_OPTIMIZE / #pragma clang optimize off change the body only, never what the engine sees of the function."""
    base = asset('OptTest')
    flags = lambda f: re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base, export_index(base, f))).group(1)
    assert flags('RawPragma') == flags('Raw') == flags('Drop'), (flags('RawPragma'), flags('Drop'))
    print('ok  OptTest: an unoptimized function has the flags of an optimized one')


opt_locals()
opt_bools()
opt_forward()
opt_in_place()
opt_slots()
opt_threads()
opt_raw()
opt_flags()


# ---- FinalTest

def calls_in(base, fn):
    """(callee, op) of each call to a script function fn's script makes, the call inside a context included."""
    out = []

    def walk(n):
        if n.op in (0x1B, 0x1C, 0x45, 0x46): out.append((n.val, n.op))
        for k in n.kids: walk(k)
    for n in runscript.script_of(base, fn): walk(n)
    return out


def final_calls():
    """`final`: a call whose body is known here runs that body in place, and the function stays for other callers.
    Recursion, noinline, UE_NO_OPTIMIZE, a body that resumes later and a call on another object stay calls, reaching
    the one function they are bound to (EX_LocalFinalFunction), not a function found by name."""
    base = asset('FinalTest')
    fact = lambda v: 1 if v <= 1 else v * fact(v - 1)
    for c in (0, 5):
        for v in (-3, 0, 7):
            f = dict(Counter=c)
            got = run(base, 'UseBump', self_vars=f, V=v)[0]
            assert got == (c + v) * 100 + c + 2 * v and f == dict(Counter=c + 2 * v), ('UseBump', c, v, got, f)
    check('FinalTest', 'Fact', lambda V: fact(V), [dict(V=v) for v in (-2, 0, 1, 5, 10)])
    check('FinalTest', 'Parity', lambda N: 2 if N % 2 == 0 else 1, [dict(N=n) for n in (0, 1, 2, 5, 8)])
    check('FinalTest', 'UseAround', lambda A: (A - 1) * 1000 + A + 1, [dict(A=a) for a in (-5, 0, 9)])
    check('FinalTest', 'UseKept', lambda V: (V + 1) * 10 + V * 2, [dict(V=v) for v in (-5, 0, 9)])
    check('FinalTest', 'UseTwice', lambda V: V * 2 + (V + 1) * 2, [dict(V=v) for v in (-5, 0, 9)])
    reached = lambda fn: {name for name, op in calls_in(base, fn) if name in exports_of(base) and op == 0x46}
    assert not reached('UseBump') and not reached('UseAround') and not reached('UseTwice'), 'an expandable call stayed a call'
    for fn, want in (('Fact', {'Fact'}), ('UseKept', {'Kept', 'KeptRaw'}), ('CallsWait', {'Wait'}), ('PeerBump', {'Bump'})):
        assert reached(fn) == want, (fn, calls_in(base, fn))
    for fn in ('Bump', 'Around', 'Twice', 'IsEven', 'IsOdd', 'Wait'):
        assert fn in exports_of(base), fn + ' is no function any more'
    vm = VM(base, Counter=0, Peer=None)
    assert vm.call('PeerBump', By=3) == -1
    peer = vm.new(Counter=10)
    vm.self.vars['Peer'] = peer
    assert vm.call('PeerBump', By=3) == 13 and peer.vars['Counter'] == 13 and vm.self.vars['Counter'] == 0, vm.self.vars
    print('ok  FinalTest: expanded calls run their body; recursion, noinline, UE_NO_OPTIMIZE, latent and Peer calls stay bound calls')

    folder = os.path.dirname(base)
    fb, fk, fw = (os.path.join(folder, c) for c in ('FinalBase', 'FinalKid', 'FinalWorld'))
    for c in (0, 4):
        for k in (0, 1, 3):
            f = dict(N=c)
            got = run(fb, 'Loop', self_vars=f, K=k)[0]
            assert got == k * c + k * (k + 1) // 2 and f == dict(N=c + k), ('Loop', c, k, got, f)
    assert run(fk, 'Use')[0] == 101 and not calls_in(fk, 'Use') and not calls_in(fb, 'Loop'), (calls_in(fk, 'Use'), calls_in(fb, 'Loop'))
    print("ok  FinalBase / FinalKid: a final method's calls expand beside a subclass; an inherited body's call reaches FinalKid's Hook")

    vm = VM(fw, {'GetPlayerPawn': lambda vm, ctx, wco, i: wco})
    other = Obj('Other_C')
    assert vm.call('Ask', Other=other) is other and vm.call('Mine') is vm.self and vm.call('PawnOf', WorldContextObject=other) is other
    assert vm.call('ViaNoContext') is vm.self and not calls_in(fw, 'ViaNoContext')
    print("ok  FinalWorld: an expanded static's calls get the world context the caller passed it, or the caller's own")

    flags = lambda b, fn: int(re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', b, export_index(b, fn))).group(1), 16)
    for b, fn in ((base, 'Bump'), (base, 'Kept'), (fb, 'Step'), (fk, 'Use')):
        assert flags(b, fn) & 0x1 and not flags(b, fn) & 0x8000000, (fn, hex(flags(b, fn)))      # Final, no BlueprintEvent
    assert flags(fb, 'Loop') & 0x8000000 and not flags(fb, 'Loop') & 0x1, hex(flags(fb, 'Loop'))
    assert not flags(base, 'Kept') & 0x4, hex(flags(base, 'Kept'))                                # noinline is not authority only
    print('ok  FinalTest: a final function is Final and not BlueprintEvent; noinline sets no flag')

    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'FinalHide.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/FinalHide");\n'
                    'class FinalHideBase : public AActor {\npublic:\n  virtual int32 F() final { return 1; }\n};\n'
                    'class FinalHide : public FinalHideBase {\npublic:\n  int32 F(int32 X) { return X; }\n};\n')
        proc = assetgen_compile([src, UEAPI, tmp])
        assert proc.returncode != 0 and 'FinalHideBase::F is final' in proc.stdout, proc.stdout
    print("ok  FinalTest: a subclass function of a final method's name is refused")


final_calls()


def final_as():
    """UE_FINAL_AS: the base's calls on `this` expand as a final class's do, and one that stays a call is bound to the
    base's function, Final as a final class's; the base is cooked Abstract and the leaf, the one class made, is not; any
    other subclass of the base is refused."""
    folder = os.path.dirname(asset('FinalAsTest'))
    base, leaf = os.path.join(folder, 'UFinalAsBase'), asset('FinalAsTest')
    for c in (0, 5):
        for v in (-3, 7):
            f = dict(Counter=c)
            got = run(base, 'UseBump', self_vars=f, V=v)[0]
            assert got == (c + v) * 100 + c + 2 * v and f == dict(Counter=c + 2 * v), ('UseBump', c, v, got, f)
    assert not [n for n, op in calls_in(base, 'UseBump') if n in exports_of(base)], calls_in(base, 'UseBump')
    # A call that stays a call - noinline, recursion - is bound to the base's own function (EX_LocalFinalFunction), as
    # a final class's is, and those functions are Final and not BlueprintEvent.
    fact = lambda v: 1 if v <= 1 else v * fact(v - 1)
    for v in (-2, 0, 5):
        assert run(base, 'UseKept', V=v)[0] == (v + 1) * 10 + v and run(base, 'Fact', V=v)[0] == fact(v), v
    reached = lambda fn: {name for name, op in calls_in(base, fn) if name in exports_of(base) and op == 0x46}
    assert reached('UseKept') == {'Kept'} and reached('Fact') == {'Fact'}, (calls_in(base, 'UseKept'), calls_in(base, 'Fact'))
    flags = lambda fn: int(re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base, export_index(base, fn))).group(1), 16)
    for fn in ('Bump', 'Kept', 'Fact'):
        assert flags(fn) & 0x1 and not flags(fn) & 0x8000000, (fn, hex(flags(fn)))
    abstract = lambda b: int(re.search(r'ClassFlags (\S+)', dump('dumpstruct.py', b, 0)).group(1), 16) & 0x1
    assert abstract(base) and not abstract(leaf)
    refused('FinalAsTwo', '', 'derives from FaBase, which is UE_FINAL_AS FaLeaf',
            top='class FaBase : public AActor {\npublic:\n  int32 X;\n};\nUE_FINAL_AS(FaBase, FaLeaf);\n'
                'class FaOther : public FaBase {};\n')
    # A base with UE_CLASS, as a header shared by mods declares it, is a mod class like any other.
    refused('FinalAsHdr', '', 'derives from FaHdr, which is UE_FINAL_AS FaHdrLeaf',
            top='class FaHdr : public AActor {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/FinalAsHdr/FaHdr", "FaHdr_C");\n};\n'
                'UE_FINAL_AS(FaHdr, FaHdrLeaf);\nclass FaHdrOther : public FaHdr {};\n')
    refused('FinalAsTwice', '', 'FaTwo has two UE_FINAL_AS leaves, FaTwoA and FaTwoB',
            top='class FaTwo : public AActor {\npublic:\n  int32 X;\n};\nUE_FINAL_AS(FaTwo, FaTwoB);\nUE_FINAL_AS(FaTwo, FaTwoA);\n')
    print('ok  FinalAsTest: UE_FINAL_AS compiles the base as final, cooks it Abstract, and refuses a second subclass '
          'or a second leaf')


final_as()


def final_as_pure():
    """UE_FINAL_AS on a base with a `= 0` method is refused: the leaf, the one class made, would be abstract too
    (IsAbstract walks its chain to the method), so nothing could be spawned, and the base's calls to the method are
    bound to its empty stub."""
    refused('FinalAsPure', '', 'UE_FINAL_AS(FaPureBase, FaPureLeaf): FaPureBase::Need is `= 0`',
            top='class FaPureBase : public AActor {\npublic:\n  virtual int32 Need(int32 V) = 0;\n'
                '  int32 Use(int32 V) { return Need(V) + 1; }\n};\nUE_FINAL_AS(FaPureBase, FaPureLeaf);\n')


final_as_pure()
print('ok  FinalAsTest: UE_FINAL_AS on a base with a `= 0` method is refused')


def func_stub_super():
    """FuncStubSuper: FssRoot leaves IFssTell's Tell out and gets its empty stub. FssKid's Tell and the UE_FINAL_AS
    base FssBase's override that stub: each has it as its super (func_super_link) and its flags, so FssBase's is not
    Final; and a call by name from FssRoot's code reaches each class's own."""
    leaf = asset('FuncStubSuper')
    p = lambda c: os.path.join(os.path.dirname(leaf), c)
    root, kid, base = p('FssRoot'), p('FssKid'), p('FssBase')
    for b in (root, kid, base, leaf): keeps_invariants(b)
    flags = lambda b: int(re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', b, export_index(b, 'Tell'))).group(1), 16)
    assert flags(kid) == flags(base) == flags(root) and not flags(base) & 0x1, (hex(flags(root)), hex(flags(kid)), hex(flags(base)))
    for chain, fn, want in (([kid, root], 'RootCall', 41), ([base, root], 'UseTell', 82), ([leaf, base, root], 'RootCall', 81)):
        got = run_as(chain, fn, {}, V=4)
        assert got == want, (fn, got, want)
    print('ok  FuncStubSuper: an override of an interface stub a mod ancestor got has that stub as its super')


def func_import_call():
    """FuncImportUser calls into FuncImportOwner's classes through a shared header. A Blueprint function is no
    native whose thunk dispatches: EX_FinalFunction runs exactly the one it names (ScriptCore.cpp 3005-3009). So Via's
    call to Bump, which FicKid overrides, goes by name, as the editor calls a function without FUNC_Final, and reaches
    FicKid's on a FicKid; the final Fixed and the static Twice are bound to FicBase's, and FicUserKid's parent call is
    FicBase's own."""
    asset('FuncImportOwner')
    user = asset('FuncImportUser')
    kid = os.path.join(os.path.dirname(user), 'FicUserKid')
    for b in (user, kid): keeps_invariants(b)
    assert calls_in(user, 'Via') == [('Bump', 0x1B)], calls_in(user, 'Via')
    for b, fn, callee in ((user, 'ViaFixed', 'Fixed'), (user, 'ViaStatic', 'Twice'), (kid, 'Bump', 'Bump')):
        assert calls_in(b, fn) == [(callee, 0x1C)], (fn, calls_in(b, fn))
        assert '/Game/_ElytrasMods/FuncImportOwner/FicBase.FicBase_C:' + callee in import_paths(b), (fn, import_paths(b))


func_import_call()
print('ok  FuncImportCall: a call to another mod\'s Blueprint function goes by name unless it is final, static or a '
      'parent call')


def final_as_shared():
    """FinalAsShared.h declares a UE_CLASS base and its UE_FINAL_AS beside it. FinalAsOwner, whose path the base's is,
    cooks both; FinalAsUser includes the header and cooks neither: its cast to the leaf and its call through one name
    the owner's FaShLeaf, the class every leaf object is, as a cast to the base names the owner's FaShBase."""
    leaf = os.path.join(os.path.dirname(asset('FinalAsOwner')), 'FaShLeaf')
    user = asset('FinalAsUser')
    assert os.path.exists(leaf + '.uasset') and os.path.exists(os.path.join(os.path.dirname(leaf), 'FaShBase.uasset'))
    made = sorted(f for f in os.listdir(os.path.dirname(user)) if f.endswith('.uasset'))
    assert made == ['FinalAsUser.uasset'], made
    paths = import_paths(user)
    assert '/Game/_ElytrasMods/FinalAsOwner/FaShLeaf.FaShLeaf_C' in paths, paths
    keeps_invariants(user)


final_as_shared()
print('ok  FinalAsTest: a UE_FINAL_AS in a shared header makes the base owner\'s leaf, imported by every other mod')


def final_as_foreign():
    """UE_FINAL_AS over a base this source does not cook. A game Blueprint stays as the game has it - not final, its own
    subclasses kept - so it is refused. Another mod's UE_CLASS base is final only where its owner says so, in the header
    it shares (FinalAsShared): written in a mod's own source, the leaf would be pinned beside the owner's base, imported,
    and never cooked by the owner, so it is refused there."""
    refused('FinalAsGame', '', 'BP_TutorialComponent_C is the game\'s Blueprint',
            top='#include "UeApi/Game/BP_TutorialComponent_C.h"\nUE_FINAL_AS(BP_TutorialComponent_C, FagLeaf);\n')
    refused('FinalAsForeign', '', 'only the UE_FINAL_AS in the header that declares it',
            top='class FafBase : public AActor {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/FafOwner/FafBase", "FafBase_C");\n'
                '  int32 Bump(int32 V);\n};\nUE_FINAL_AS(FafBase, FafLeaf);\n')


final_as_foreign()
print('ok  FinalAsTest: UE_FINAL_AS over a base this mod does not cook is refused, unless written in the header its '
      'owner shares')


# ---- NestedTest

def nested_containers():
    fields = {}
    got = run(asset('NestedTest'), 'Build', self_vars=fields)[0]
    # Found and Again hold 2; Grid ends [[7], [1]] after Grid[1].Add(5), Grid[0][0] = 7 and Grid[1] = Row.
    assert got == 2 + 2 + 2 + 7, got
    assert fields == {'Groups': {'first': ['a', 'b']}, 'Grid': [[7], [1]]}, fields
    out = dump('dumpstruct.py', asset('NestedTest'), 0)
    assert "UserDefinedStruct'FNC_TArray_FName'" in out and "UserDefinedStruct'FNC_TArray_int'" in out, out
    check('NestedTest', 'MapIndex', lambda Seed: (Seed + 2) * 10 + 1, [dict(Seed=v) for v in (0, 5, -3)])
    print('ok  NestedTest: containers inside containers through wrapper structs')


def map_index_members():
    for seed in (0, 5, -3):
        for start in ({}, {'b': 4, 'a': 99}):
            fields = {'Counts': dict(start)}
            got = run(asset('NestedTest'), 'MapIndex', self_vars=fields, Seed=seed)[0]
            assert got == (seed + 2) * 10 + 1 and fields == {'Counts': dict(start, a=seed + 2)}, (seed, start, got, fields)
    print('ok  NestedTest.MapIndex: Counts[a] ends at Seed + 2, other keys untouched')


def nested_map_range():
    for start in ({}, {1: [5, 6], 2: [7]}, {3: [], 9: [1, 2, 3, 4]}):
        fields = {'Buckets': {k: list(v) for k, v in start.items()}}
        got = run(asset('NestedTest'), 'CountBuckets', self_vars=fields)[0]
        assert got == sum(len(v) * 10 + k for k, v in start.items()) and fields == {'Buckets': start}, (start, got, fields)
        run(asset('NestedTest'), 'AppendZeroBuckets', self_vars=fields)
        assert fields == {'Buckets': {k: v + [0] for k, v in start.items()}}, (start, fields)
    print('ok  NestedTest: a range-for over a map of arrays reads and writes back each array')


def nested_find_types():
    """execMap_Find writes its out-parm in place only when that property's class matches the map's value property
    (ScriptCore / KismetArrayLibrary): so every Map_Find on Groups must write a FNC_TArray_FName-typed local."""
    base = asset('NestedTest')
    props = dict(re.findall(r"^\s+StructProperty (\w+) .*?(UserDefinedStruct'\w+')", dump('dumpstruct.py', base, export_index(base, 'Build')), re.M))
    finds = []
    def walk(n):
        if n.op in (0x1C, 0x46, 0x68) and n.val == 'Map_Find' and n.kids[0].op == 1 and n.kids[0].val == 'Groups':
            finds.append(n.kids[2].val)
        for k in n.kids: walk(k)
    for st in runscript.script_of(base, 'Build'): walk(st)
    assert len(finds) == 2 and all(props.get(o) == "UserDefinedStruct'FNC_TArray_FName'" for o in finds), (finds, props)
    print("ok  NestedTest: Map_Find on Groups writes a value of the map's own value type")


nested_containers()
map_index_members()
nested_map_range()
nested_find_types()


# ---- CompTest

def comp_test():
    base = asset('CompTest')
    for t in (0, 41):
        fields = dict(Ticks=t)
        assert run(base, 'ReceiveBeginPlay', self_vars=fields)[0] is None and fields == dict(Ticks=t + 1), fields
    orange = VM(base).call('Orange')     # an EX_StructConst lists the members in their reflected order: B, G, R, A
    assert orange == [0, 128, 255, 255], orange
    # The engine dispatches it: an override of Actor's BlueprintImplementableEvent, not final.
    fn = dump('dumpstruct.py', base, export_index(base, 'ReceiveBeginPlay'))
    flags = int(re.search(r'FunctionFlags (\S+)', fn).group(1), 16)
    assert "SuperStruct imp[" in fn and "Function'ReceiveBeginPlay'" in fn and flags & 0x08000800 == 0x08000800 and not flags & 1, fn
    cls = dump('dumpstruct.py', base, 0)
    for var, klass in (('Root', 'SceneComponent'), ('Mesh', 'StaticMeshComponent'), ('Lamp', 'PointLightComponent')):
        assert re.search(r"ObjectProperty %s .*Class'%s'" % (var, klass), cls), (var, cls)
    assert re.search(r'IntProperty Ticks ', cls), cls
    tags = lambda name: dump('dumptags.py', base, export_index(base, name))
    assert 'bVisible [0] BoolProperty size=0 value=0' in tags('Mesh_GEN_VARIABLE')
    lamp = tags('Lamp_GEN_VARIABLE')
    assert 'Intensity [0] FloatProperty size=4: 1500.0' in lamp and 'LightColor [0] StructProperty size=4 struct=Color: 0080ffff' in lamp, lamp
    # The root's own transform moved onto the components attached to it: (10, 0, 0), yaw 90 and (2, 2, 3) on Mesh, whose
    # roll 90 composes to (0, 90, 90); Lamp's (5, 0, 50) scaled by the root's, turned by its yaw and offset by it, to
    # (10, 10, 150), at yaw 90.
    root, mesh = tags('Root_GEN_VARIABLE'), tags('Mesh_GEN_VARIABLE')
    assert 'Relative' not in root, root
    for t, loc, rot in ((mesh, '000020410000000000000000', '000000000000b4420000b442'),
                        (lamp, '000020410000204100001643', '000000000000b44200000000')):
        assert ('RelativeLocation [0] StructProperty size=12 struct=Vector: ' + loc in t
                and 'RelativeRotation [0] StructProperty size=12 struct=Rotator: ' + rot in t
                and 'RelativeScale3D [0] StructProperty size=12 struct=Vector: 000000400000004000004040' in t), t
    # After its tags' None and UObject's HasGuid, an archetype carries what its class's native Serialize reads. Rocks's
    # 32 bytes are the ones DRG's one cooked ISM archetype (BP_SpacerigTrashCompactor) ends in; Grass adds the
    # hierarchical one's empty ClusterTree.
    ua, ue, total, names, imports, exports = dumpexp.load(base)
    end = names.index('None').to_bytes(4, 'little') + bytes(8)
    ism = '00000000' '01000000' '4000000000000000' '0400000000000000' '0000000000000000'
    for name, tail in (('Root', ''), ('Lamp', ''), ('Mesh', '00000000'), ('Rocks', ism), ('Grass', ism + '4000000000000000')):
        e = next(e for e in exports if e['name'] == name + '_GEN_VARIABLE')
        p = ue[e['off'] - total: e['off'] - total + e['size']]
        assert p.endswith(end + bytes.fromhex(tail)), (name, p[-48:].hex())
    refused('ModelComp', '  UE_COMPONENT(UModelComponent, Bsp);\n', 'cannot be a component template')
    print('ok  CompTest: BeginPlay override, component variables, archetype defaults and native tails')


def scs_tree(base):
    """An actor class's SCS as the engine walks it: {node's variable: [its ChildNodes' variables]}, and the root nodes."""
    names = exports_of(base)
    var, tags = {}, {}
    for i, n in enumerate(names):
        if n.startswith('SCS_Node_'):
            tags[n] = dump('dumptags.py', base, i)
            var[n] = re.search(r'InternalVariableName \[0\] NameProperty size=8: (\w+)', tags[n]).group(1)

    def objs(t, prop):
        m = re.search(prop + r' \[0\] ArrayProperty size=\d+ inner=ObjectProperty: (\w+)', t)
        raw = bytes.fromhex(m.group(1)) if m else bytes(4)
        return [var[names[int.from_bytes(raw[4 + 4 * k: 8 + 4 * k], 'little', signed=True) - 1]]
                for k in range(int.from_bytes(raw[:4], 'little'))]
    scs = dump('dumptags.py', base, next(i for i, n in enumerate(names) if n.startswith('SimpleConstructionScript')))
    return {var[n]: objs(t, 'ChildNodes') for n, t in tags.items()}, objs(scs, 'RootNodes'), tags


def comp_attachment():
    """Mesh and Lamp attach to Root in the spawned actor. A cooked SCS's PostLoad runs FixupRootNodeParentReferences
    (not WITH_EDITOR), which clears a root node's ParentComponentOrVariableName unless it is native
    (bIsParentComponentNative) or names an ANCESTOR Blueprint's node via ParentComponentOwnerClassName; so a node
    parented to a sibling in the same SCS must be one of that node's ChildNodes."""
    children, roots, tags = scs_tree(asset('CompTest'))
    assert roots == ['Root'] and children == {'DefaultSceneRoot': [], 'Root': ['Mesh', 'Lamp', 'Rocks', 'Grass'], 'Mesh': [],
                                              'Lamp': [], 'Rocks': [], 'Grass': []}, (roots, children)
    assert not any('ParentComponentOrVariableName' in t for t in tags.values()), tags
    print('ok  CompTest: Mesh and Lamp stay attached to Root after a cooked load')


comp_test()
comp_attachment()


# ---- TypesTest, StringTest, StructTest, PointerTest

def mod_enum():
    import re, struct, subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    base = asset('TypesTest')
    enum = os.path.join(os.path.dirname(base), 'EMood')
    r = dumpexp.load(enum)
    names, raw = r[3], r[1]
    assert [e['name'] for e in r[5]] == ['EMood'] and "Class'UserDefinedEnum'" in r[4], r[4]
    count = struct.unpack_from('<i', raw, 12)[0]
    pairs = [(names[struct.unpack_from('<i', raw, 16 + i * 16)[0]], struct.unpack_from('<q', raw, 24 + i * 16)[0]) for i in range(count)]
    assert pairs == [('EMood::Calm', 0), ('EMood::Angry', 5), ('EMood::Sleepy', 6), ('EMood::EMood_MAX', 7)], pairs
    exports = [e['name'] for e in dumpexp.load(base)[5]]
    cls = dump('dumpstruct.py', base, '0')
    assert re.search(r'ByteProperty Mood .*EMood', cls), cls
    cdo = dump('dumptags.py', base, str(exports.index('Default__TypesTest_C')))
    assert 'EMood::Angry' in cdo, cdo
    print('ok  TypesTest: UE_ENUM cooks EMood with its C++ names and values; the default is its enumerator')
    for enum, pairs_want in (('ESpan', [('ESpan::Tiny', -3), ('ESpan::Wide', 70000), ('ESpan::Huge', 70001), ('ESpan::Vast', 70002), ('ESpan::ESpan_MAX', 70003)]),
                             ('EAge', [('EAge::Epoch', 0), ('EAge::Eon', 5000000000), ('EAge::EAge_MAX', 5000000001)])):
        r = dumpexp.load(os.path.join(os.path.dirname(base), enum))
        names, raw = r[3], r[1]
        count = struct.unpack_from('<i', raw, 12)[0]
        got = [(names[struct.unpack_from('<i', raw, 16 + i * 16)[0]], struct.unpack_from('<q', raw, 24 + i * 16)[0]) for i in range(count)]
        assert got == pairs_want, got
    assert re.search(r'EnumProperty Span .*size=4 .*\n\s+IntProperty UnderlyingType .*size=4', cls), cls
    assert re.search(r'EnumProperty Age .*size=8 .*\n\s+Int64Property UnderlyingType .*size=8', cls), cls
    assert 'ESpan::Wide' in cdo and 'EAge::Eon' in cdo, cdo
    print('ok  TypesTest: int32 / int64 enums cook as EnumProperty over Int / Int64Property')


def fnv(s):
    h = 2166136261
    for c in s.encode(): h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h & 0x7FFFFFFF


def constants():
    import re, subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    base = asset('TypesTest')
    exports = [e['name'] for e in dumpexp.load(base)[5]]
    cdo = dump('dumptags.py', base, str(exports.index('Default__TypesTest_C')))
    for want in ('Seed [0] IntProperty size=4: %d' % fnv('types'), 'Budget [0] IntProperty size=4: 25', 'Reach [0] FloatProperty size=4: 125.0', 'Bits [0] IntProperty size=4: 236',
                 'Halfway [0] BoolProperty size=0 value=1'):
        assert want in cdo, (want, cdo)
    print('ok  TypesTest: a member default is what its constant expression comes to')
    import struct
    six, pair = struct.pack('<6f', 1, 2, 3, 4, 5, 6).hex(), struct.pack('<3f', 7, 8, 9).hex()
    points = re.search(r'Points \[0\] ArrayProperty size=77 inner=StructProperty: (\w+)', cdo).group(1)   # count, inner tag (53), 2 x 12 raw
    assert points.startswith('02000000') and points.endswith(six), points
    assert re.search(r'Spots \[0\] MapProperty size=28 .*: 0000000001000000\w{16}' + pair, cdo), cdo
    print('ok  TypesTest: a container default holds struct elements, raw or tagged as the struct serializes')
    names = dumpexp.load(base)[3]
    name_of = lambda hx: names[struct.unpack('<i', bytes.fromhex(hx[:8]))[0]]
    by_mood = re.search(r'MoodNames \[0\] MapProperty size=56 key=ByteProperty value=NameProperty: 0{8}03000000(\w+)', cdo).group(1)
    pairs = [(name_of(by_mood[i:i + 16]), name_of(by_mood[i + 16:i + 32])) for i in range(0, 96, 32)]
    assert pairs == [('EMood::Calm', 'Calm'), ('EMood::Angry', 'Angry'), ('EMood::Sleepy', 'Sleepy')], pairs
    by_name = re.search(r'MoodsByName \[0\] MapProperty size=62 key=StrProperty value=ByteProperty: 0{8}03000000(\w+)', cdo).group(1)
    assert by_name.startswith('05000000' + b'Calm'.hex() + '00') and name_of(by_name[18:34]) == 'EMood::Calm', by_name
    print('ok  TypesTest: UE_ENUM_MAP fills a map from the enum, either way round')
    base = asset('TypesTest')
    exports = [e['name'] for e in dumpexp.load(base)[5]]
    tool = lambda t, i: dump(t, base, str(i))
    assert re.search(r"ObjectProperty Aimed .*Class'Actor'", tool('dumpstruct.py', 0)), 'a `using` alias of a class is still an object reference'
    assert re.search(r"ObjectProperty Spotted .*Class'Pawn'", tool('dumpstruct.py', 0)), 'and so is a class-scope one'
    forget = ' '.join(tool('walkscript.py', exports.index('Forget')).split())
    step = r' \+ *\d+ mem \d+ disk \d+ mem \d+ '
    assert re.search(r'InstanceVariable Health@\S+' + step + 'NoInterface', forget) and re.search(r'InstanceVariable Aimed@\S+' + step + 'NoObject', forget), forget
    print('ok  TypesTest: a class alias stays an object; nullptr is EX_NoInterface for an interface')
    cls = tool('dumpstruct.py', 0)
    for var in ('kHold', 'kTag', 'kPrimes', 'kMoods', 'kRates', 'kKinds'):
        assert var not in cls and var not in cdo, var
    print('ok  TypesTest: an inline class variable cooks no property and no default')
    for fn in ('PrimeSum', 'PrimeFold', 'FirstPrimeOver'):
        assert 'ArrayProperty' not in tool('dumpstruct.py', exports.index(fn)), fn
    print('ok  TypesTest: a range-for over an inline array of constants makes no array')
    refused('StaticVar', '  static inline float Loose = 0.25f;\n  float Get() { return Loose; }\n', 'no static storage')
    refused('StaticVar', '  static inline float Loose = 0.25f;\n  void Set() { Loose = 1; }\n', 'no static storage')
    refused('StaticVar', '  static inline const float kHold = 0.5f;\n  StaticVar *Me() { return this; }\n'
            '  float Get() { return Me()->kHold; }\n', 'is static: name it without the object')
    print('ok  a static that is not const, and an inline variable read through a call, are refused')


def types_behaviour():
    # Cpp20: every enumerator, the values between and past them (no case: `return kSeed`), int32 wrap.
    check('TypesTest', 'Cpp20', lambda M, N: wrap(N + N) if M == 0 else wrap(N + fnv('angry')) if M == 5 else fnv('types'),
          [dict(M=m, N=n) for m in (0, 1, 4, 5, 6, 7, 255) for n in EDGE])
    check('TypesTest', 'ConstSum', lambda N: wrap(N * 3 + 31), [dict(N=n) for n in EDGE])
    check('TypesTest', 'HalfOf', lambda V: V * 0.5, [dict(V=v) for v in (-3.0, 0.0, 8.0, -0.25)])
    # Inline class variables: each use is the initializer, a container one made where it is used.
    primes = [2, 3, 5, 7, 11]
    check('TypesTest', 'HoldFor', lambda N: N * 0.75, [dict(N=n) for n in (-3, 0, 4)])
    check('TypesTest', 'TagOf', lambda: 'types', [dict()])
    check('TypesTest', 'PrimeAt', lambda I: primes[I] + 5, [dict(I=i) for i in range(5)])
    check('TypesTest', 'IsPrime', lambda N: N in primes, [dict(N=n) for n in range(13)])
    check('TypesTest', 'PrimeSum', lambda: 28, [dict()])
    check('TypesTest', 'PrimesBelow', lambda N: sum(p < N for p in primes), [dict(N=n) for n in (0, 3, 6, 12)])
    check('TypesTest', 'IsMood', lambda M: M in ('calm', 'angry'), [dict(M=m) for m in ('calm', 'angry', 'sleepy')])
    check('TypesTest', 'RateOf', lambda K: {1: 0.5, 3: 3.0}.get(K, -1.0), [dict(K=k) for k in (0, 1, 2, 3)])
    check('TypesTest', 'KindCount', lambda: 2, [dict()])
    check('TypesTest', 'PrimeFold', lambda: (((2 * 3 + 3) * 3 + 5) * 3 + 7) * 3 + 11, [dict()])       # in order
    check('TypesTest', 'FirstPrimeOver', lambda N: next((p for p in primes if p > N), -1), [dict(N=n) for n in (-5, 2, 4, 10, 11, 20)])
    del runscript.CALLS[:]
    check('TypesTest', 'RollSum', lambda: 2, [dict()])            # the stand-in RandomInteger is 0
    assert [c[0] for c in runscript.CALLS].count('RandomInteger') == 2, runscript.CALLS
    print('ok  TypesTest.RollSum: an inline array of calls is made once for the loop, not once a pass')
    check('TypesTest', 'LocalList', lambda I: [4, 5, 6][I] + 3, [dict(I=i) for i in range(3)])
    check('TypesTest', 'ShrBy', lambda X, M: X >> (M if M in (1, 4) else 31),   # Python >> floors, as C++'s does
          [dict(X=x, M=m) for x in EDGE + (-3, -1, -17) for m in (1, 4, 31)])
    check('TypesTest', 'Shr64', lambda X, M: X >> (1 if M == 1 else 63),
          [dict(X=x, M=m) for x in (-2**63, -3, -1, 0, 5, 2**63 - 1) for m in (1, 63)])
    # int64 -> float keeps only the low 32 bits (UE 4.27 has no int64 -> float): 2**32 + 5 is 5.
    check('TypesTest', 'I64ToFloat', lambda X: float(wrap(X)), [dict(X=x) for x in (0, -3, 7, -2**31, 2**32 + 5, -2**32 - 7)])
    check('TypesTest', 'TruncViaI64', lambda G: float(int(G)), [dict(G=g) for g in (0.0, 2.75, -2.75, 1e6 + 0.5)])
    check('TypesTest', 'AllOnes', lambda: -1, [dict()])
    check('TypesTest', 'Huge', lambda: float('inf'), [dict()])
    check('TypesTest', 'PlusChar', lambda X: wrap(X + 128), [dict(X=x) for x in EDGE])
    check('TypesTest', 'ShiftByLL', lambda X, M: wrap(X << 2) if M == 0 else X >> (1 if M == 1 else 3),
          [dict(X=x, M=m) for x in EDGE + (-8, -7, -1) for m in (0, 1, 2)])
    import struct
    f32 = lambda v: struct.unpack('<f', struct.pack('<f', v))[0]
    below = [f32(v) for v in (0.99999994, 7.9999995, -0.99999994, 3.9999998, 2.75, -2.75, 0.0, -1e9)]
    check('TypesTest', 'TruncOf', lambda X, M: int(X * 2 if M == 2 else X), [dict(X=x, M=m) for x in below for m in (0, 1, 2)])
    check('TypesTest', 'Trunc64Of', lambda X: int(X), [dict(X=x) for x in below + [f32(5e9), f32(-7.5e12)]])
    s32 = lambda v: (v + 2**31) % 2**32 - 2**31
    check('TypesTest', 'NarrowOf', lambda X, Y, M: [X & 0xFF, Y & 0xFF, s32(Y)][M],
          [dict(X=x, Y=y, M=m) for x, y in zip(EDGE, (511, -1, 2**32 + 5, -2**63, 2**63 - 1, 300, 5000000000)) for m in range(3)])
    # Constants of an enum with no fixed type keep their value (they were bytes: 1000 read back as 232).
    check('TypesTest', 'AnonConst', lambda X, M: [min(X, 1000), wrap(X - 5), wrap(X * 70000), int(X == 70000), 1000][M],
          [dict(X=x, M=m) for x in EDGE + (500, 999, 1000, 1001, 5000, 69999, 70000) for m in range(5)])
    check('TypesTest', 'WideConst', lambda X: X + 5000000000, [dict(X=x) for x in (-5000000000, -1, 0, 7, 1 << 32)])
    # A constant float is true when nonzero (0.5f truncated to 0 first and folded to false).
    check('TypesTest', 'FloatTruth', lambda X, M: [0, int(X > 0), 7, 1][M], [dict(X=x, M=m) for x in EDGE for m in range(4)])
    # An int64 enum compares as int64: a value sharing only Eon's / Epoch's low 32 bits is neither.
    check('TypesTest', 'AgeOf', lambda A: 1 if A == 5000000000 else 2 if A == 0 else 0,
          [dict(A=a) for a in (0, 5000000000, 7, 5000000001, 5000000000 & 0xFFFFFFFF, 1 << 32, -(1 << 32))])
    # A value-changing cast inside a chain of casts: bool, uint8, int32 from int64, int32 from float.
    nan, floats = float('nan'), (0.0, -0.0, 0.5, -0.25, 2.75, -2.75, 255.9, 300.5, -1.5)
    ints = EDGE + (-1, 255, 256, 300)
    check('TypesTest', 'CastBool', lambda F, X: wrap((F != 0) + X), [dict(F=f, X=x) for f in floats + (nan,) for x in (1, 2**31 - 1)])
    check('TypesTest', 'CastBoolK', lambda X: wrap(1 + X), [dict(X=x) for x in EDGE])
    check('TypesTest', 'CastBoolInt', lambda V, X: wrap((V != 0) + X), [dict(V=v, X=x) for v in ints for x in (1, -2**31)])
    check('TypesTest', 'CastBoolEnum', lambda M, X: wrap((M != 0) + X), [dict(M=m, X=x) for m in (0, 5, 6, 255) for x in (1, 2**31 - 1)])
    for fn in ('CastByte', 'CastByteStatic'):
        check('TypesTest', fn, lambda V, X: wrap((V & 0xFF) + X), [dict(V=v, X=x) for v in ints for x in (0, 2**31 - 1)])
    check('TypesTest', 'CastByteK', lambda X: wrap(44 + X), [dict(X=x) for x in EDGE])
    check('TypesTest', 'CastInt64', lambda V: wrap(V), [dict(V=v) for v in (7, -1, 2**31, -2**31 - 1, 5000000001, -(2**40) - 5)])
    check('TypesTest', 'CastWide', lambda V: V & 0xFF, [dict(V=v) for v in ints])
    check('TypesTest', 'CastTrunc', lambda F: float(int(F)), [dict(F=f) for f in floats])
    check('TypesTest', 'CastChain', lambda F, X: wrap((int(F) & 0xFF) + X), [dict(F=f, X=x) for f in floats for x in (0, 2**31 - 1)])
    check('TypesTest', 'CastTest', lambda F: F != 0, [dict(F=f) for f in floats + (nan,)])
    # Past the switch, `M == Mood ? 10 : 0` / `S == Span ? 10 : 0` read the member.
    for mood in (0, 5, 7):
        for m in (0, 1, 4, 5, 6, 7, 255):
            got = run(asset('TypesTest'), 'MoodScore', self_vars={'Mood': mood}, M=m)[0]
            assert got == {0: 1, 5: 2, 6: 3}.get(m, 10 if m == mood else 0), ('MoodScore', mood, m, got)
    for span in (70000, 5, -3):
        for s in (-4, -3, -2, 5, 69999, 70000, 70001, 70002, 70003):
            got = run(asset('TypesTest'), 'SpanScore', self_vars={'Span': span}, S=s)[0]
            assert got == {-3: 1, 70000: 2, 70001: 3, 70002: 4}.get(s, 10 if s == span else 0), ('SpanScore', span, s, got)
    print('ok  TypesTest.MoodScore / SpanScore: the switch, then the member compare')
    # TEnum<EMood> is an EMood; Name() / String() hand the engine EMood's UEnum and the value.
    calls, names = [], {0: 'Calm', 5: 'Angry', 6: 'Sleepy'}
    vm = VM(asset('TypesTest'), {'GetEnumeratorName': lambda vm, ctx, e, v: calls.append(('Name', e, v)) or names[v],
                                 'GetEnumeratorUserFriendlyName': lambda vm, ctx, e, v: calls.append(('String', e, v)) or names[v] + '!'},
            Mood=5, Tagged=6)
    assert (vm.call('TaggedName', 5), vm.call('TaggedString', 0), vm.call('MemberString')) == ('Angry', 'Calm!', 'Sleepy!')
    assert calls == [('Name', 'EMood', 5), ('String', 'EMood', 0), ('String', 'EMood', 6)], calls
    assert [vm.call('TaggedScore', m) for m in (0, 5, 6, 7)] == [0, 10, 3, 0] and vm.self.vars['Tagged'] == 7, vm.self.vars
    print('ok  TypesTest: TEnum<E> switches, compares and assigns as E; Name() / String() are the engine\'s lookups on E')
    assert [vm.call('SleepyCount', l) for l in ([], [6], [0, 6, 5, 6])] == [0, 1, 2]
    print('ok  TypesTest: a global `using` of a template type is that type')
    check('TypesTest', 'GetIsTargetable', lambda: True, [dict()])
    for r in (0, 4):
        f = {}
        run(asset('TypesTest'), 'ReceiveEndPlay', self_vars=f, Reason=r)
        assert f == {'LastReason': r}, f
    f = {'Scores': [3]}
    run(asset('TypesTest'), 'HandleScored', self_vars=f, Points=7, By=None)
    assert f == {'Scores': [3, 7]}, f
    f = {'Health': 'H', 'Aimed': 'A'}
    run(asset('TypesTest'), 'Forget', self_vars=f)
    assert f == {'Health': None, 'Aimed': None}, f
    print('ok  TypesTest: ReceiveEndPlay, HandleScored and Forget write their members')
    # The class implements ITargetable, and each interface function it leaves out exists and returns the zero value,
    # so a call through the interface finds the Blueprint function rather than the interface's native one. It returns
    # a value of its own: runscript's None is `Return Nothing`, which leaves a script caller's destination as it was.
    here = os.path.dirname(os.path.abspath(__file__))
    cls = dump('dumpstruct.py', asset('TypesTest'), '0')
    assert re.search(r"""Interfaces \[\("imp\[\d+\]:Class'Targetable'", 0, 1\)\]""", cls), cls
    for fn, zero in (('GetTargetCenterMass', 0), ('GetTargetHealthComponent', 0), ('ShowDamageEffects', None)):
        assert run(asset('TypesTest'), fn)[0] == zero, fn
    print('ok  TypesTest: implements Targetable; the functions it leaves out return zero')


def types_defaults():
    """Every element of the tagged-struct array and both maps the source initialises (replaces the size-only 414)."""
    import struct
    from dumptags import tags
    here, base = os.path.dirname(os.path.abspath(__file__)), asset('TypesTest')
    names, exports = dumpexp.load(base)[3], [e['name'] for e in dumpexp.load(base)[5]]
    cdo = dump('dumptags.py', base, str(exports.index('Default__TypesTest_C')))
    raw = bytes.fromhex(re.search(r'Spans \[0\] ArrayProperty size=\d+ inner=StructProperty: (\w+)', cdo).group(1))
    o, spans = 4 + 49, []                                  # count, then the inner tag (name, type, size, index, struct, guid)
    for _ in range(struct.unpack_from('<i', raw, 0)[0]):
        out = []
        o = tags(raw, o, len(raw), names, 0, out)
        spans.append({l.split()[0]: float(l.split(': ')[1]) for l in out})
    assert spans == [{'Min': 1.0, 'Max': 5.0}, {'Min': -2.0, 'Max': 2.0}], spans
    raw = bytes.fromhex(re.search(r'MoodsByName \[0\] MapProperty .*?: (\w+)', cdo).group(1))
    o, pairs = 8, []
    for _ in range(struct.unpack_from('<i', raw, 4)[0]):
        n = struct.unpack_from('<i', raw, o)[0]
        key = raw[o + 4:o + 3 + n].decode(); o += 4 + n
        pairs.append((key, names[struct.unpack_from('<i', raw, o)[0]])); o += 8
    assert pairs == [('Calm', 'EMood::Calm'), ('Angry', 'EMood::Angry'), ('Sleepy', 'EMood::Sleepy')], pairs
    spots = bytes.fromhex(re.search(r'Spots \[0\] MapProperty .*?: (\w+)', cdo).group(1))
    assert names[struct.unpack_from('<i', spots, 8)[0]].lower() == 'home' and struct.unpack_from('<3f', spots, 16) == (7, 8, 9)
    print('ok  TypesTest: Spans, MoodsByName and Spots defaults hold every element')


def native_struct_values():
    """A struct the engine reads in its own binary form, which WriteValue does not write (it writes tags), gets no
    value: not as a default, and not as a UE_STRUCT member, whose default instance holds every member's."""
    refused('NativeDefault', '  FGameplayTagContainer Labels = {};\n', 'GameplayTagContainer value in its own binary form')
    refused('NativeMember', '  int32 X = 0;\n', "a UE_STRUCT's defaults hold every member's",
            top='struct FTagHolder {\n  UE_STRUCT;\n  FGameplayTagContainer Tags;\n};\n')
    print('ok  a GameplayTagContainer value is refused, as a default and as a UE_STRUCT member')


def string_behaviour():
    import runscript
    check('StringTest', 'MakeKey', lambda Prefix, Index: Prefix + '_' + str(Index),
          [dict(Prefix=p, Index=i) for p in ('', 'Abc') for i in (-5, 0, 42, 2**31 - 1)])
    base = asset('StringTest')
    cdo = dump('dumptags.py', base, [e['name'] for e in dumpexp.load(base)[5]].index('Default__StringTest_C'))
    assert 'Umlaut [0] NameProperty size=8: Größe' in cdo, cdo
    check('StringTest', 'IsUmlaut', lambda S: S.lower() == 'größe', [dict(S=s) for s in ('Größe', 'GRößE', 'Grösse', '')])
    print('ok  StringTest: a non-ASCII FName reads back as written, in the CDO and the bytecode')

    class Trace(dict):
        """The object's fields, recording every store in order."""
        def __init__(s): super().__init__(); s.log = []
        def __setitem__(s, k, v): s.log.append((k, v)); super().__setitem__(k, v)

    f = Trace()
    runscript.MESSAGES.clear()
    run(asset('StringTest'), 'ReceiveBeginPlay', self_vars=f)
    k3, tail = 'Kills: 3, ratio 0.5', 'plain nameplain text1234567890123' + '1234567890123_7' + 'false'
    assert f.log == [('Count', 3), ('Ratio', 0.5), ('Label', k3), ('Key', k3 + '_key'), ('Caption', k3 + '_key'),
                     ('Caption', k3 + (k3 + '_key') * 2), ('Label', 'plain nameplain text'), ('Big', 1234567890123),
                     ('Caption', '1234567890123'), ('Label', 'plain nameplain text1234567890123'),
                     ('Count', 0), ('Ratio', 0.0), ('Big', 0), ('Count', 7), ('Key', '1234567890123_7'), ('Label', tail),
                     ('Label', 'Kills: 7'), ('Label', '7 left')], f.log
    assert runscript.MESSAGES == [tail + ' (Self)'], runscript.MESSAGES
    print('ok  StringTest.ReceiveBeginPlay: every conversion and concat; "lit" + N and N + "lit" concatenate')


def base_names(v):
    """A UserDefinedStruct value with the cooked GUID suffix off each member: Kills_624902_<guid> -> Kills."""
    return {re.sub(r'_\d+_[0-9A-F]{32}$', '', k): base_names(x) for k, x in v.items()} if isinstance(v, dict) else v


def struct_behaviour():
    import runscript
    kills = next(n for n in dumpexp.load(os.path.join(os.path.dirname(asset('StructTest')), 'FStats'))[3] if n.startswith('Kills_'))
    check('StructTest', 'KillsOf', lambda S: S.get(kills, 0), [dict(S=s) for s in ({}, {kills: 5}, {kills: -1})])
    check('StructTest', 'MakeLocal', lambda K: wrap(K + wrap(K * 2) * 10 + 1000), [dict(K=k) for k in (0, 3, -2, 2**30)])
    check('StructTest', 'MakeArgument', lambda K: wrap(K + wrap(K + 1)), [dict(K=k) for k in (0, 5, -1, 2**31 - 1)])
    check('StructTest', 'MakeInLoop', lambda Rounds: max(Rounds, 0), [dict(Rounds=r) for r in (-3, 0, 1, 2, 4)])
    # A copy passed where a reference is written (T& parameter, Array_Add, Array_Get's / Map_Find's out item, a by-value
    # range-for variable, a same-typed input Append / Union read in place): the write lands in the copy, never in the
    # variable it was copied from.
    n = 0
    for k in (-7, 0, 5, 2**31 - 1):
        for fn, fields, want, after in (
                ('CopyToRef', {'Stats': {kills: 3}}, 3, {'Stats': {kills: 3}}),
                ('MemberCopyToRef', {'Stats': {kills: 3}}, 3, {'Stats': {kills: 3}}),
                ('ArgCopyToRef', {}, k, {}),
                ('ArrayCopyAdd', {'Counts': [1, 2]}, 2, {'Counts': [1, 2]}),
                ('ArrayGetIntoCopy', {'Stats': {kills: 3}, 'Counts': [9]}, 3, {'Stats': {kills: 3}, 'Counts': [9]}),
                ('RangeCopyToRef', {'Many': [{kills: 1}, {kills: 2}]}, 1, {'Many': [{kills: 1}, {kills: 2}]}),
                ('MapFindIntoCopy', {'Scores': {1: 40}}, k, {'Scores': {1: 40}}),
                ('ArrayAppendCopy', {'Counts': [1, 2]}, i32(k + 4), {'Counts': [1, 2, 1, 2]}),
                ('SetUnionCopy', {'Seen': [1, 2], 'Fresh': [2, 3]}, i32(k + 3), {'Seen': [2, 3, 1], 'Fresh': [2, 3]}),
                ('AppendComputed', {'Counts': [1, 2]}, i32(k + 6), {'Counts': [1, 2, 1, 2, 1, 2]})):
            f = copy.deepcopy(fields)
            got = run(asset('StructTest'), fn, self_vars=f, K=k)[0]
            assert (got, f) == (want, after), (fn, k, got, f)
            n += 1
    print('ok  StructTest: a local copy bound to a written reference leaves its source alone  (%d cases)' % n)
    for k in (-7, 0, 2**31 - 1):
        f = {'Table': {1: {}}, 'Keys': 0}
        got = run(asset('StructTest'), 'MapMemberStore', self_vars=f, K=k)[0]
        assert got == i32(k + 10) and f['Keys'] == 1 and list(f['Table']) == [1] \
            and base_names(f['Table'][1]) == {'Inner': {'Kills': i32(k + 10)}, 'Stamp': 4}, (k, got, f)
    assert "StructTest::MapMemberStore: AddTo's reference parameter V is bound to a map element's member" in LOGS['StructTest']
    print("ok  StructTest: a store through a map element's members, or a reference to one, reaches the map, its key found once")
    f = {}
    runscript.MESSAGES.clear()
    run(asset('StructTest'), 'ReceiveBeginPlay', self_vars=f)
    # `Stats = Local` replaces the whole struct: Alive and Owner are false / null again, so no message is posted.
    assert base_names(f) == {'Stats': {'Kills': 4, 'Time': 1.5}, 'Nested': {'Inner': {'Kills': 8, 'Time': 1.5}},
                             'Moody': {'Mood': 0, 'Level': 2}}, base_names(f)
    assert runscript.MESSAGES == [], runscript.MESSAGES
    print('ok  StructTest.ReceiveBeginPlay: member stores, whole-struct copies, nested members')
    base = asset('StructTest')
    lines = dump('dumptags.py', base, [e['name'] for e in dumpexp.load(base)[5]].index('Default__StructTest_C')).split('\n')
    at = next(i for i, l in enumerate(lines) if l.startswith('  Deep [0] StructProperty'))
    deep = [l.strip() for l in itertools.takewhile(lambda l: l.startswith('    '), lines[at + 1:]) if 'StructProperty' not in l]
    assert deep == ['Kills [0] IntProperty size=4: 0', 'Time [0] FloatProperty size=4: 1.5', 'Alive [0] BoolProperty size=0 value=0:',
                    'Owner [0] ObjectProperty size=4: index 0', 'Stamp [0] Int64Property size=8: 7'], deep
    print('ok  StructTest: a designated member default gives the members it leaves out zero')


def pointer_behaviour():
    import runscript
    check('PointerTest', 'Advance', lambda Base, Count: Base + 4 * Count,
          [dict(Base=b, Count=c) for b in (0, 0x7FF600001000) for c in (-2, 0, 1, 2**31 - 1)])
    for v in (-1, 0, 41, 2**31 - 1):
        assert run(asset('PointerTest'), 'Bump', Value=v)[1]['Value'] == wrap(v + 1), v
    for ok in (True, False):
        f = {'Failures': 2}
        runscript.MESSAGES.clear()
        run(asset('PointerTest'), 'Check', self_vars=f, bOk=ok, What='x')
        assert f == {'Failures': 2 if ok else 3} and runscript.MESSAGES == ([] if ok else ['PointerTest FAILED: x']), (ok, f)
    # A byte read through int8* / signed char* sign-extends; through uint8* it does not.
    addr, sb = 0x7FF600001000, lambda b: b - 256 if b > 127 else b
    for pair in ((0xFF, 0xFF), (0x80, 0x7F), (0x05, 0xFE), (0x7F, 0x80), (0, 0)):
        runscript.MEM.update({addr: pair[0], addr + 1: pair[1]})
        got = [run(asset('PointerTest'), 'SignedByteMix', P=addr)[0], run(asset('PointerTest'), 'SignedByteNegative', P=addr)[0]]
        got += [run(asset('PointerTest'), fn, P=addr, I=i)[0] for fn in ('SignedCharAt', 'UnsignedByteAt') for i in (0, 1)]
        got += [run(asset('PointerTest'), fn, P=addr)[0] for fn in ('SignedByteAsUnsigned', 'SignedByteIsMax')]
        want = [sb(pair[0]) * 3 + sb(pair[1]), sb(pair[0]) < 0, sb(pair[0]), sb(pair[1]), pair[0], pair[1]]
        want += [pair[0] * 1000 + pair[1], pair[0] == 255]
        assert got == want, (pair, got, want)
    runscript.MEM.clear()
    # An intrinsic whose value goes nowhere is no call (it was an EX_CallMath on null, which crashes the VM).
    assert run(asset('PointerTest'), 'DiscardedIntrinsics', P=addr, N='x')[0] == 3
    # Kismet has no unsigned int32: a uint32 read would widen and compare as signed, so it is refused.
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        with open(os.path.join(tmp, 'ReadU32.cpp'), 'w') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/ReadU32");\n'
                    'class ReadU32 : public AActor {\npublic:\n  int64 Get(uint32 *P) { return *P; }\n};\n')
        proc = assetgen_compile([os.path.join(tmp, 'ReadU32.cpp'), UEAPI, tmp])
        assert proc.returncode != 0 and 'reading a uint32 through a pointer' in proc.stdout, proc.stdout
    # The synthesized read scratch is cooked beside the class, and the class imports it.
    d = dumpexp.load(os.path.join(os.path.dirname(asset('PointerTest')), 'FDeref'))
    assert [e['name'] for e in d[5]] == ['FDeref'] and "Class'UserDefinedStruct'" in d[4], d[4]
    assert "UserDefinedStruct'FDeref'" in dumpexp.load(asset('PointerTest'))[4]
    print('ok  PointerTest: Advance, Bump (out-parm), Check, signed byte reads, discarded intrinsics, uint32 reads refused; FDeref synthesized')


mod_enum()
constants()
types_behaviour()
types_defaults()
native_struct_values()
string_behaviour()
struct_behaviour()
check('StructTest', 'MakeNative', lambda D: D + 0.5, [dict(D=d) for d in (0.0, 4.0)])
pointer_behaviour()


# ---- IfaceTest, OverrideTest, SuperTest, NameTest, AssetTest

def interfaces():
    """IfaceTest: what the engine reads of a mod interface and of the classes that implement one."""
    folder = os.path.dirname(asset('IfaceTest'))
    base = lambda a: os.path.join(folder, a)
    IFACE = '/Game/_ElytrasMods/IfaceTest/%s.%s_C'

    def cls(a):
        out, paths = dump('dumpstruct.py', base(a), 0), import_paths(base(a))
        sup = re.search(r'SuperStruct imp\[(\d+)\]', out)
        return dict(flags=int(re.search(r'ClassFlags (\S+)', out).group(1), 16),
                    funcs=set(re.findall(r"\('([^']+)', 'exp\[\d+\]", re.search(r'FuncMap (.*)', out).group(1))),
                    props={n: t for t, n in re.findall(r'^  (\w+Property) (\w+) ', out, re.M)},
                    ifaces=[(paths[int(i)], int(off), int(k2)) for i, off, k2 in
                            re.findall(r"imp\[(\d+)\][^,]*, (-?\d+), (\d+)\)", re.search(r'Interfaces (.*)', out).group(1))],
                    super=paths[int(sup.group(1))] if sup else None, text=out)

    # CLASS_Interface (0x4000) over UInterface, or over the interface it extends; one function per declared method,
    # no state - IMarkable's variables are properties of its implementers.
    t, m = cls('ITargetable'), cls('IMarkable')
    assert t['flags'] & 0x4000 and t['super'] == '/Script/CoreUObject.Interface', t['text']
    assert m['flags'] & 0x4000 and m['super'] == IFACE % ('ITargetable', 'ITargetable'), m['text']
    assert t['funcs'] == {'GetPriority', 'OnTargeted'} and m['funcs'] == {'Mark'}, (t['funcs'], m['funcs'])
    assert not t['props'] and not m['props'], (t['props'], m['props'])
    print('ok  IfaceTest: ITargetable / IMarkable cook as interface classes, IMarkable extending ITargetable')
    # An implementer lists its interface with bImplementedByK2 and has a function of every name in the interface's
    # chain: an interface call finds its target by name (FindFunctionChecked), so a missing one is fatal.
    chain = {'ITargetable': {'GetPriority', 'OnTargeted'}, 'IMarkable': {'GetPriority', 'OnTargeted', 'Mark'},
             'CurveSourceInterface': {'GetBindingName', 'GetCurveValue', 'GetCurves'}}
    for a, iface in (('Turret', 'ITargetable'), ('Describe', 'ITargetable'), ('Beacon', 'IMarkable'),
                     ('Singer', 'CurveSourceInterface'), ('Hummer', 'CurveSourceInterface')):
        c = cls(a)
        path = '/Script/Engine.CurveSourceInterface' if iface == 'CurveSourceInterface' else IFACE % (iface, iface)
        assert c['ifaces'] == [(path, 0, 1)] and chain[iface] <= c['funcs'], (a, c['ifaces'], c['funcs'])
    lb, b = cls('LoudBeacon'), cls('Beacon')      # LoudBeacon inherits Beacon's: no entry, no Marks shadowing Beacon's
    assert lb['ifaces'] == [] and lb['super'] == '/Game/_ElytrasMods/IfaceTest/Beacon.Beacon_C' and 'Marks' not in lb['props'], lb['text']
    assert b['props'].get('Marks') == 'IntProperty' and b['props'].get('MarkedBy') == 'ObjectProperty', b['props']
    for a, want in (('Beacon', 12), ('LoudBeacon', 40)):
        cdo = dump('dumptags.py', base(a), exports_of(base(a)).index('Default__%s_C' % a))
        assert 'Marks [0] IntProperty size=4: %d' % want in cdo, (a, cdo)
    print('ok  IfaceTest: implementers list their interface, define its whole chain, and hold its variables')
    # A native interface function is implemented by a script function (no FUNC_Native 0x400) with the native's
    # inherited BlueprintEvent 0x8000000 | Const 0x40000000.
    for a in ('Singer', 'Hummer'):
        for fn in ('GetBindingName', 'GetCurveValue', 'GetCurves'):
            flags = int(re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base(a), exports_of(base(a)).index(fn))).group(1), 16)
            assert flags & 0x48000000 == 0x48000000 and not flags & 0x400, (a, fn, hex(flags))
    print('ok  IfaceTest: a native interface function is implemented by a script function with its flags')


def interface_bodies():
    """IfaceTest: implementations and stubs, run offline - return values and the fields they write."""
    b = lambda a: os.path.join(os.path.dirname(asset('IfaceTest')), a)
    zero = (0,)                          # a zeroed local; None, a bare return, leaves a script caller's destination as it was
    assert run(b('Turret'), 'GetPriority')[0] == 7
    for a, fn, parms, before, after in (('Turret', 'OnTargeted', {'By': 'x'}, {'Hits': 5}, {'Hits': 6}),
                                        ('Beacon', 'Mark', {'Count': 4}, {'Marks': 12}, {'Marks': 16}),
                                        ('Beacon', 'OnTargeted', {'By': 'x'}, {'Marks': 12}, {'Marks': 12, 'MarkedBy': 'x'}),
                                        ('Describe', 'OnTargeted', {'By': 'x'}, {}, {})):
        run(b(a), fn, self_vars=before, **parms)
        assert before == after, (a, fn, before)
    for a in ('Beacon', 'Describe'):     # GetPriority left out: an empty stub
        f = {}
        assert run(b(a), 'GetPriority', self_vars=f)[0] in zero and f == {}, a
    assert run(b('Singer'), 'GetBindingName')[0] == 'Singer' and run(b('Hummer'), 'GetBindingName')[0] == 'Hummer'
    assert run(b('Singer'), 'GetCurveValue', CurveName='x')[0] == 0.5 and run(b('Hummer'), 'GetCurveValue', CurveName='x')[0] in zero
    for a in ('Singer', 'Hummer'):
        ret, env = run(b(a), 'GetCurves', OutValues=['kept'])
        assert ret is None and env['OutValues'] == ['kept'], (a, env)
    print('ok  IfaceTest: implementations and stubs return and write what the C++ says')


def interface_calls():
    """IfaceTest: a call through an interface value goes by name to the object behind it; the cast names the
    interface class; LoudBeacon reads the Marks Beacon holds, on itself and on Other."""
    folder = os.path.dirname(asset('IfaceTest'))
    for a, fn in (('Spotter', 'Rate'), ('Beacon', 'PriorityOf')):
        base = os.path.join(folder, a)
        paths, w = import_paths(base), dump('walkscript.py', base, exports_of(base).index(fn))
        casts = re.findall(r'ObjToInterfaceCast\s+imp\[(\d+)\]', w)
        assert casts and {paths[int(i)] for i in casts} == {'/Game/_ElytrasMods/IfaceTest/ITargetable.ITargetable_C'}, (a, w)
        assert 'InterfaceContext' in w and re.search(r'VirtualFunction\s+GetPriority\b', w), (a, w)
        assert not re.search(r"FinalFunction\s+imp\[\d+\]:Function'GetPriority'", w), (a, w)
    base = os.path.join(folder, 'LoudBeacon')
    w = dump('walkscript.py', base, exports_of(base).index('Total'))
    reads = re.findall(r"InstanceVariable\s+Marks@imp\[(\d+)\]", w)
    assert len(reads) == 2 and {import_paths(base)[int(i)] for i in reads} == {'/Game/_ElytrasMods/IfaceTest/Beacon.Beacon_C'}, w
    assert re.search(r'Context\s+skip \d+ Marks@[^\n]*\n[^\n]*LocalVariable\s+Other@', w) and 'Add_IntInt' in w, w
    print('ok  IfaceTest: interface calls go by name; the cast names ITargetable; Marks is read where Beacon holds it')


def inherited_defaults():
    """OverrideTest: UE_DEFAULTS on an inherited SCS component (a handler record on the parent's node), on a native
    parent's component (its default subobject) and on an inherited plain property (a tag on this CDO)."""
    import dumptags
    folder = os.path.dirname(asset('OverrideTest'))
    base = lambda a: os.path.join(folder, a)

    def objects(a):
        """export name -> (class path, template path, outer name, tags)."""
        exports = dumpexp.load(base(a))[5]
        name = lambda v: ref(base(a), v) if v else None
        return {e['name']: (name(e['cls']), name(e['tmpl']), name(e['outer']), dump('dumptags.py', base(a), i))
                for i, e in enumerate(exports)}

    bp = objects('BaseProp')
    assert 'Intensity [0] FloatProperty size=4: 1000.0' in bp['Lamp_GEN_VARIABLE'][3], bp['Lamp_GEN_VARIABLE']
    nodes = {re.search(r'InternalVariableName \[0\] NameProperty size=8: (\w+)', t).group(1): t
             for c, _, _, t in bp.values() if c == '/Script/Engine.SCS_Node'}
    children, roots, _ = scs_tree(base('BaseProp'))
    assert roots == ['Root'] and children == {'DefaultSceneRoot': [], 'Root': ['Lamp'], 'Lamp': []}, (roots, children)
    lamp_guid = re.search(r'VariableGuid \[0\] StructProperty size=16 struct=Guid: (\w+)', nodes['Lamp']).group(1)
    # DimProp: one record keyed on (BaseProp_C, Lamp's node guid) - FComponentKey::Match compares exactly those -
    # whose template, archetyped on BaseProp's Lamp_GEN_VARIABLE, holds Intensity 250.
    dp = objects('DimProp')
    ua, ue, total, names, imports, exports = dumpexp.load(base('DimProp'))
    h = next(e for e in exports if e['name'].startswith('InheritableComponentHandler'))
    blob = ue[h['off'] - total: h['off'] - total + h['size']]
    records = re.search(r'Records \[0\] ArrayProperty size=\d+ inner=StructProperty: (\w+)', dp[h['name']][3]).group(1)
    assert records.startswith('01000000'), records
    out = []
    dumptags.tags(blob, blob.find(bytes.fromhex(records)) + 4, len(blob), names, 1, out)
    rec = '\n'.join(out)
    idx = lambda f: int(re.search(r'%s \[0\] ObjectProperty size=4: index (-?\d+)' % f, rec).group(1))
    assert ref(base('DimProp'), idx('OwnerClass')) == '/Game/_ElytrasMods/OverrideTest/BaseProp.BaseProp_C', rec
    assert 'SCSVariableName [0] NameProperty size=8: Lamp' in rec and 'struct=Guid: ' + lamp_guid in rec, rec
    c, tmpl, _, tags = dp[ref(base('DimProp'), idx('ComponentTemplate'))]
    assert c == '/Script/Engine.PointLightComponent' and tmpl == '/Game/_ElytrasMods/OverrideTest/BaseProp.BaseProp_C:Lamp_GEN_VARIABLE', (c, tmpl)
    assert 'Intensity [0] FloatProperty size=4: 250.0' in tags, tags
    # InitialLifeSpan: a tag on the CDO, and no property of DimProp_C's own shadowing AActor's.
    assert 'InitialLifeSpan [0] FloatProperty size=4: 3.0' in dp['Default__DimProp_C'][3], dp['Default__DimProp_C']
    assert not re.search(r'^  \w+Property ', dump('dumpstruct.py', base('DimProp'), 0), re.M), 'DimProp_C re-declares a property'
    print('ok  OverrideTest: an inherited SCS component is a handler record on the parent node; a plain property a CDO tag')
    # Walker: ACharacter's components are Default__Character's subobjects CollisionCylinder and CharacterMesh0
    # ([V] GObjects dump; [S] Character.cpp:25-27). The override must be an export OF THAT NAME under this CDO -
    # FLinkerLoad::CreateExport (LinkerLoad.cpp:4690) looks the name up, else constructs a new, unused component.
    wk = objects('Walker')
    for sub, cls_path, want in (('CollisionCylinder', '/Script/Engine.CapsuleComponent', 'CapsuleRadius [0] FloatProperty size=4: 55.0'),
                                ('CharacterMesh0', '/Script/Engine.SkeletalMeshComponent', 'bVisible [0] BoolProperty size=0 value=0')):
        assert sub in wk, (sub, sorted(wk))
        c, _, outer, tags = wk[sub]
        assert c == cls_path and outer == 'Default__Walker_C' and want in tags, wk[sub]
    print('ok  OverrideTest: a native parent\'s component is overridden under its subobject name')


def run_as(chain, fn, fields, **parms):
    """Runs fn on an object of class chain[0] whose mod ancestors are chain[1:] (package bases) as the VM
    dispatches: a call by name runs the most derived definition, EX_FinalFunction exactly the function its import
    names. (runscript alone looks both up in the calling package.) A final call goes by its import, (package, index):
    one package can call two classes' functions of one name, a forwarding override its parent's and a qualified call
    an ancestor's."""
    import runscript
    names = {b: exports_of(b) for b in chain}
    owner = lambda f: next(b for b in chain if f in names[b])
    same = lambda a, b: os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))
    finals = {}
    for b in chain:
        paths = import_paths(b)
        for i in range(len(names[b])):
            for imp in re.findall(r'FinalFunction\s+imp\[(\d+)\]', dump('walkscript.py', b, i)):
                pkg, _, fname = paths[int(imp)].rpartition(':')
                if pkg.startswith('/Game/'):                        # a mod function, in a package beside this one
                    t = os.path.join(os.path.dirname(chain[0]), pkg.split('.')[0].rsplit('/', 1)[1])
                    finals[fname] = finals[b, int(imp)] = (next((c for c in chain if same(c, t)), t), fname)
    saved = runscript.run, runscript.params_of, dict(runscript.MATH)
    runscript.run = lambda base, f, self_vars=None, **p: saved[0](owner(f), f, self_vars, **p)
    runscript.params_of = lambda base, f, *flag: saved[1](owner(f), f, *flag)
    for k, (target, f) in finals.items():
        runscript.MATH[k] = (lambda t, f: lambda *a: saved[0](t, f, fields, **dict(zip(saved[1](t, f), a)))[0])(target, f)
    try:
        return saved[0](owner(fn), fn, fields, **parms)[0]
    finally:
        runscript.run, runscript.params_of = saved[0], saved[1]
        runscript.MATH.clear(); runscript.MATH.update(saved[2])


def parent_call():
    """SuperTest: `SuperBase::X()` in an override runs the parent's X once (its body expanded in place, or final on
    SuperBase_C's function - by name it would re-enter the override, forever); an unqualified inherited call still
    dispatches to the most derived."""
    folder = os.path.dirname(asset('SuperTest'))
    test, base = os.path.join(folder, 'SuperTest'), os.path.join(folder, 'SuperBase')

    class Base:                                   # SuperTest.cpp, as Python
        def __init__(s, Count): s.Count = Count
        def ReceiveBeginPlay(s): s.Count = 1
        def Bump(s, By): s.Count += By; return s.Count
        def Twice(s, By): return s.Bump(By) + s.Bump(By)
        TwiceInline = Twice

    class Test(Base):
        def ReceiveBeginPlay(s): Base.ReceiveBeginPlay(s); s.Count += 10
        def Bump(s, By): return Base.Bump(s, By * 2)
        def Thrice(s, By): return s.Twice(By) + s.Bump(By)
        def ViaInline(s, By): return s.TwiceInline(By)

    n = 0
    for chain, model in (([base], Base), ([test, base], Test)):
        for fn, args in (('ReceiveBeginPlay', {}), ('Bump', {'By': 3}), ('Twice', {'By': 2}), ('Thrice', {'By': 1}), ('ViaInline', {'By': 2})):
            if not hasattr(model, fn): continue
            for count in (0, 5):
                fields, obj = {'Count': count}, model(count)
                got, want = run_as(chain, fn, fields, **args), getattr(obj, fn)(**args)
                assert (got in (None, 0) if want is None else got == want) and fields['Count'] == obj.Count, (chain[0], fn, count, got, want, fields)
                n += 1
    print('ok  SuperTest: parent calls, overrides and inherited calls run as in C++  (%d cases)' % n)
    paths = import_paths(test)
    # A parent's body known here is expanded in place; one that overrides an engine event stays a final call.
    for fn, expanded in (('Bump', True), ('ReceiveBeginPlay', False)):
        w = dump('walkscript.py', test, exports_of(test).index(fn))
        finals = [paths[int(i)] for i in re.findall(r'FinalFunction\s+imp\[(\d+)\]', w)]
        assert ('/Game/_ElytrasMods/SuperTest/SuperBase.SuperBase_C:' + fn not in finals) == expanded, w
        assert not re.search(r'VirtualFunction\s+%s\b' % fn, w), w
    w = dump('walkscript.py', test, exports_of(test).index('Thrice'))
    assert re.search(r'VirtualFunction\s+Twice\b', w) and re.search(r'VirtualFunction\s+Bump\b', w), w
    # An override names the parent's UFunction as its super and keeps its flags; a new function has no super.
    for b, fn, parent in ((test, 'Bump', '/Game/_ElytrasMods/SuperTest/SuperBase.SuperBase_C:Bump'),
                          (test, 'ReceiveBeginPlay', '/Game/_ElytrasMods/SuperTest/SuperBase.SuperBase_C:ReceiveBeginPlay'),
                          (base, 'ReceiveBeginPlay', '/Script/Engine.Actor:ReceiveBeginPlay'),
                          (test, 'Thrice', None), (base, 'Bump', None), (base, 'Twice', None)):
        e = dumpexp.load(b)[5][exports_of(b).index(fn)]
        assert (ref(b, e['super']) if e['super'] else None) == parent, (b, fn, e['super'])
    flags = lambda b, fn: re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', b, exports_of(b).index(fn))).group(1)
    assert flags(test, 'Bump') == flags(base, 'Bump') and flags(test, 'ReceiveBeginPlay') == flags(base, 'ReceiveBeginPlay') == '0x8080800'
    print('ok  SuperTest: Base::Method() is the parent\'s body, expanded or a final call; overrides bind to it')


def pure_virtual():
    """SuperTest's PureBase: `= 0` cooks an empty function returning the default, which a subclass's version names as
    its super and a call by name on an object without one finds; a class clang calls abstract is cooked Abstract."""
    folder = os.path.dirname(asset('SuperTest'))
    pb, pm, pk = (os.path.join(folder, c) for c in ('PureBase', 'PureMid', 'PureKid'))
    assert run(pb, 'Pure', V=5)[0] == 0 and {'Pure', 'Touch'} <= set(exports_of(pb)), exports_of(pb)
    assert run_as([pk, pm, pb], 'UsePure', {}, V=4) == 70 and run_as([pm, pb], 'UsePure', {}, V=4) == 0
    for b, fn in ((pk, 'Pure'), (pm, 'Touch')):
        e = dumpexp.load(b)[5][exports_of(b).index(fn)]
        assert ref(b, e['super']) == '/Game/_ElytrasMods/SuperTest/PureBase.PureBase_C:' + fn, (b, fn, e['super'])
    abstract = lambda b: int(re.search(r'ClassFlags (\S+)', dump('dumpstruct.py', b, 0)).group(1), 16) & 0x1
    assert abstract(pb) and abstract(pm) and not abstract(pk) and not abstract(os.path.join(folder, 'SuperBase'))
    pl = os.path.join(folder, 'PokeLess')       # an interface's `= 0` it leaves out: a stub, not Abstract
    assert not abstract(pl) and run(pl, 'Poke')[0] == 0
    print('ok  SuperTest: `= 0` is an empty function a subclass overrides; a class left abstract is cooked Abstract')


def engine_names():
    """NameTest: members Dumper-7 respelled, run and cooked under the engine's names; access, purity, const."""
    base = asset('NameTest')
    loaded = dumpexp.load(base)
    exports = [e['name'] for e in loaded[5]]
    cdo = dump('dumptags.py', base, exports.index('Default__NameTest_C'))
    assert 'Index [0] IntProperty size=4: 7' in cdo and re.search(r"Name \[0\] StrProperty size=\d+: 'Karl'", cdo), cdo
    # SplitName would have made FName(Index, 1) of the C++ spelling: neither it nor the spelling may be in the package.
    assert 'Index_0' not in loaded[3] and 'Name_0' not in loaded[3], [n for n in loaded[3] if n.endswith('_0')]
    f = {'Index': 7}                 # runscript keys a field by the name it is cooked under
    assert run(base, 'Next', self_vars=f)[0] == 8 and f == {'Index': 8}, f
    assert run(base, 'Peek', self_vars={'Index': 41})[0] == 41
    owners = {import_paths(base)[int(i)] for i in re.findall(r'InstanceVariable\s+Index@imp\[(\d+)\]', dump('walkscript.py', base, exports.index('Next')))}
    assert owners == {'/Script/FSD.FSDSaveGame'}, owners
    print('ok  NameTest: a member Dumper-7 respelled is cooked and run by the engine\'s name (tag and bytecode)')
    assert run(base, 'Step')[0] == 1 and run(base, 'Twice')[0] == 2
    # FUNC_Public 0x20000 / Private 0x40000 / Protected 0x80000 as the C++ says; UE_PURE = BlueprintPure 0x10000000.
    fflags = lambda fn: int(re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base, exports.index(fn))).group(1), 16)
    for fn, want in (('Next', 0x20000), ('Peek', 0x20000), ('SameKind', 0x20000), ('Whose', 0x20000), ('Step', 0x80000), ('Twice', 0x40000)):
        assert fflags(fn) & 0xE0000 == want and bool(fflags(fn) & 0x10000000) == (fn == 'Peek'), (fn, hex(fflags(fn)))
    print('ok  NameTest: a function carries its C++ access specifier, and UE_PURE')
    props = dump('dumpstruct.py', base, exports.index('NameTest_C'))
    pflags = lambda name: int(re.search(r'Property %s .*? flags=(\S+)' % name, props).group(1), 16)
    assert pflags('Limit') & 0x10 and not any(pflags(p) & 0x10 for p in ('Seed', 'Charges', 'Plain')), props
    assert 'Limit [0] IntProperty size=4: 3' in cdo, cdo
    print('ok  NameTest: a const member is BlueprintReadOnly')


def object_forwards():
    """NameTest: UObject's C++ helpers. The engine has no GetOuter / GetClass / GetName UFunction on UObject, so none
    is imported or called by name; GetClass / GetName are the Kismet statics on the object, a class's own GetName
    (UFSDSaveGame's) wins, and GetOuter reads OuterPrivate at the object + 0x20."""
    base = asset('NameTest')
    exports, paths = exports_of(base), import_paths(base)
    assert not any(p.endswith(('Object:GetOuter', 'Object:GetClass', 'Object:GetName')) for p in paths), paths
    walk = lambda fn: dump('walkscript.py', base, exports.index(fn))
    calls = lambda fn: [paths[int(i)] for i in re.findall(r'(?:FinalFunction|CallMath)\s+imp\[(\d+)\]', walk(fn))]
    for fn in ('Whose', 'SameKind'):
        assert not re.search(r'VirtualFunction\s+(GetOuter|GetClass|GetName)\b', walk(fn)), walk(fn)
    assert calls('Whose').count('/Script/Engine.KismetSystemLibrary:GetObjectName') == 1 and '/Script/FSD.FSDSaveGame:GetName' in calls('Whose'), calls('Whose')
    assert calls('SameKind').count('/Script/Engine.GameplayStatics:GetObjectClass') == 2, calls('SameKind')
    w = walk('SameKind')
    assert re.search(r"GetObjectClass'\s*\n[^\n]*LocalVariable\s+Other@", w) and re.search(r"GetObjectClass'\s*\n[^\n]*Self", w), w
    ua, ue, total, names, imports, exps = dumpexp.load(base)
    e = exps[exports.index('Whose')]
    assert b'\x35' + (0x20).to_bytes(8, 'little') in ue[e['off'] - total: e['off'] - total + e['size']], 'Whose reads no OuterPrivate (+0x20)'
    print('ok  NameTest: GetOuter / GetClass / GetName reach engine functions or OuterPrivate, never a missing UFunction')


def api_stub():
    """The editor API stub (--api) of NameTest: what a Blueprint author sees of a mod class."""
    import struct, tempfile, dumptags
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, 'cooked'))
        os.makedirs(os.path.join(tmp, 'api'))
        proc = assetgen_compile([os.path.join(TESTS, 'NameTest.cpp'), UEAPI,
                                 os.path.join(tmp, 'cooked'), '--api', os.path.join(tmp, 'api')])
        assert proc.returncode == 0, proc.stdout + proc.stderr
        base = os.path.join(tmp, 'api', 'NameTest')
        ua, ue, total, names, imports, exports = dumpexp.load(base)
        blob = ue[exports[0]['off'] - total: exports[0]['off'] - total + exports[0]['size']]
        out = []
        dumptags.tags(blob, 0, len(blob), names, 1, out)
        # NewVariables: count, the inner StructProperty tag (49 bytes), then one tagged BPVariableDescription each.
        at = blob.find(bytes.fromhex(re.search(r'NewVariables .*: (\w+)', '\n'.join(out)).group(1)))
        p, variables = at + 4 + 49, {}
        for _ in range(struct.unpack_from('<i', blob, at)[0]):
            v = []
            p = dumptags.tags(blob, p, len(blob), names, 0, v)
            variables[re.search(r'VarName \[0\] NameProperty size=8: (\w+)', '\n'.join(v)).group(1)] = '\n'.join(v)
        entries = {exports[e['outer'] - 1]['name']: dump('dumptags.py', base, i)
                   for i, e in enumerate(exports) if e['name'].startswith('K2Node_FunctionEntry')}
    category = b'Names|Test'.hex()
    # A private field is left out; the const one is offered read-only (CPF_BlueprintReadOnly 0x10).
    assert set(variables) == {'Charges', 'Plain', 'Limit'}, set(variables)
    rflags = lambda v: struct.unpack('<Q', bytes.fromhex(re.search(r'PropertyFlags \[0\] UInt64Property size=8: (\w+)', variables[v]).group(1)))[0]
    assert rflags('Limit') & 0x10 and not rflags('Charges') & 0x10 and not rflags('Plain') & 0x10
    # UE_CATEGORY files Charges and Peek; Plain follows UE_CATEGORY("") and has none.
    assert category in variables['Charges'] and 'Category' not in variables['Plain'], variables
    assert category in entries['Peek'] and not any(category in entries[f] for f in entries if f != 'Peek'), entries
    # Every method is offered, its entry node carrying its access specifier and purity (ExtraFlags).
    assert set(entries) == {'Next', 'Peek', 'SameKind', 'Step', 'Twice', 'Whose'}, set(entries)
    extra = lambda f: int(re.search(r'ExtraFlags \[0\] IntProperty size=4: (-?\d+)', entries[f]).group(1))
    for f in entries:
        assert extra(f) & 0xE0000 == {'Step': 0x80000, 'Twice': 0x40000}.get(f, 0x20000), (f, hex(extra(f)))
        assert bool(extra(f) & 0x10000000) == (f == 'Peek'), (f, hex(extra(f)))
    print('ok  NameTest: the API stub carries UE_CATEGORY, access and purity, and leaves a private field out')


def static_assets():
    """AssetTest: namespace-scope objects cook as assets of their class holding exactly the members their braces
    name; `&Asset` anywhere is a reference to that asset's package path."""
    import struct
    folder = os.path.dirname(asset('AssetTest'))
    base = lambda a: os.path.join(folder, a)
    MOD = '/Game/_ElytrasMods/AssetTest/'

    def tags_of(a, i=0):
        return {m.group(1): m.group(2) for m in re.finditer(r'^  (\w+) \[0\] (\w+ size=\d+[^:]*: ?.*)$', dump('dumptags.py', base(a), i), re.M)}

    def objs(a, hexs):
        """An ObjectProperty array payload (count, then one FPackageIndex each), as paths."""
        raw = bytes.fromhex(hexs)
        return [ref(base(a), v) for v in struct.unpack_from('<%di' % struct.unpack_from('<i', raw)[0], raw, 4)]

    for a, cls in (('MD_Plain', MOD + 'UMoodDef.UMoodDef_C'), ('MD_Calm', MOD + 'UMoodDef.UMoodDef_C'),
                   ('MD_Big', MOD + 'UMoodDef.UMoodDef_C'), ('ED_AssetTest', '/Script/FSD.EnemyDescriptor')):
        e = dumpexp.load(base(a))[5]
        assert [x['name'] for x in e] == [a] and ref(base(a), e[0]['cls']) == cls and e[0]['flags'] & 0x3 == 0x3, (a, e)   # RF_Public | RF_Standalone
    # Only the named members are written, the rest stay the class defaults; an explicit zero is written.
    assert tags_of('MD_Plain') == {}, tags_of('MD_Plain')
    assert tags_of('MD_Calm') == {'Count': 'IntProperty size=4: 0', 'Mood': 'ByteProperty size=8 enum=EDefMood: EDefMood::Calm'}, tags_of('MD_Calm')
    big = tags_of('MD_Big')
    assert set(big) == {'Health', 'Title', 'Tag', 'bBig', 'Next', 'Waves'}, big
    assert big['Health'].endswith(': -500.5') and big['Title'].endswith(": 'Big'") and big['Tag'].endswith(': big') and 'value=1' in big['bBig'], big
    assert ref(base('MD_Big'), int(big['Next'].split()[-1])) == MOD + 'MD_Calm.MD_Calm', big['Next']
    assert big['Waves'].endswith(struct.pack('<4i', 3, 3, 5, 8).hex()), big['Waves']
    cdo = dump('dumptags.py', base('UMoodDef'), exports_of(base('UMoodDef')).index('Default__UMoodDef_C'))
    for want in ("Title [0] StrProperty size=9: 'Base'", 'Health [0] FloatProperty size=4: 100.0', 'Count [0] IntProperty size=4: 3',
                 'Mood [0] ByteProperty size=8 enum=EDefMood: EDefMood::Angry'):
        assert want in cdo, (want, cdo)
    ed = tags_of('ED_AssetTest')
    assert objs('ED_AssetTest', ed['VeteranClasses'].split()[-1]) == ['/Game/Enemies/Spider/Grunt/ED_Spider_Grunt.ED_Spider_Grunt'], ed
    assert ed['SpawnSpread'].endswith(': 250.0') and ed['IdealSpawnSize'].endswith(': 4') and 'value=1' in ed['CanBeUsedForConstantPressure'], ed
    # A soft class is tagged SoftObjectProperty, as the cook tags ED_Spider_Grunt's own EnemyClass: a tag naming
    # SoftClassProperty is dropped at load as a type mismatch. The value is the path (an FName) and an empty sub-path.
    assert ed['EnemyClass'].startswith('SoftObjectProperty size=12:'), ed['EnemyClass']
    soft = struct.unpack('<3i', bytes.fromhex(ed['EnemyClass'].split()[-1]))
    assert dumpexp.load(base('ED_AssetTest'))[3][soft[0]] == '/Game/Enemies/Spider/Grunt/ENE_Spider_Grunt_Normal.ENE_Spider_Grunt_Normal_C' \
        and soft[1:] == (0, 0), soft
    print('ok  AssetTest: each declared object is an asset of its class with the members its braces name')
    # AssetUser's defaults: a pointer, an array, a set and two maps, object elements by package path.
    user = base('AssetUser')
    names = dumpexp.load(user)[3]
    cdo = {k: bytes.fromhex(v.split()[-1]) if k != 'Picked' else int(v.split()[-1]) for k, v in tags_of('AssetUser', exports_of(user).index('Default__AssetUser_C')).items()}
    assert ref(user, cdo['Picked']) == MOD + 'MD_Big.MD_Big', cdo
    assert objs('AssetUser', cdo['Enemies'].hex()) == ['/Game/Enemies/Spider/Grunt/ED_Spider_Grunt.ED_Spider_Grunt', MOD + 'ED_AssetTest.ED_AssetTest'], cdo
    # One name in two namespaces is two assets; a Package.Object path imports that object from that package.
    assert objs('AssetUser', cdo['Picks'].hex()) == [
        '/Game/Enemies/Spider/Grunt/ED_Spider_Grunt.ED_Spider_Grunt', '/Game/Enemies/Spider/Exploder/ED_Spider_Exploder.ED_Spider_Exploder',
        '/Game/Art/Environments/Holiday_GreatEggHunt/SK_greatEggHunt_bunnyPlush.SK_GreatEggHunt_BunnyPlush'], cdo
    tags = struct.unpack_from('<6i', cdo['Labels'])                              # removed, count, (FName) x 2
    assert tags[:2] == (0, 2) and [names[tags[2]], names[tags[4]]] == ['big', 'calm'], tags
    m = struct.unpack_from('<8i', cdo['ByName'])                                     # removed, count, (FName, object) x 2
    assert m[:2] == (0, 2) and [(names[m[2]], ref(user, m[4])), (names[m[5]], ref(user, m[7]))] == [('big', MOD + 'MD_Big.MD_Big'), ('calm', MOD + 'MD_Calm.MD_Calm')], m
    assert struct.unpack_from('<3ifif', cdo['Scale']) == (0, 2, 1, 0.5, 2, -2.0), cdo
    print('ok  AssetTest: member defaults reference the assets, in containers too')
    # ReceiveBeginPlay posts "Picked <Picked->Title>, calm count <MD_Calm->Count>" through the game state; Say is
    # inline, so no UFunction of that name.
    exports, paths = exports_of(user), import_paths(user)
    assert 'Say' not in exports, exports
    w = dump('walkscript.py', user, exports.index('ReceiveBeginPlay'))
    used = {paths[int(i)] for i in re.findall(r'imp\[(\d+)\]', w)}
    for p in ('/Script/FSD.GameFunctionLibrary:GetFSDGameState', '/Script/FSD.FSDGameState:PostGameMessage',
              MOD + 'MD_Calm.MD_Calm', '/Script/Engine.KismetStringLibrary:Conv_IntToString'):
        assert p in used, (p, used)
    for want in (r"StringConst\s+'Picked '", r"StringConst\s+', calm count '", r'InstanceVariable\s+Picked@', r'InstanceVariable\s+Title@',
                 r'InstanceVariable\s+Count@'):
        assert re.search(want, w), (want, w)
    print('ok  AssetTest: a function body reaches an asset by reference')
    ar = dump('dumpar.py', registry_of('AssetTest'))
    assert set(re.findall(r'^\s+(/Game/\S+)\s+(\S+)$', ar, re.M)) == {
        (MOD + 'AssetUser.AssetUser_C', 'BlueprintGeneratedClass'), (MOD + 'UMoodDef.UMoodDef_C', 'BlueprintGeneratedClass'),
        (MOD + 'EDefMood.EDefMood', 'UserDefinedEnum'), (MOD + 'MD_Plain.MD_Plain', 'UMoodDef_C'), (MOD + 'MD_Calm.MD_Calm', 'UMoodDef_C'),
        (MOD + 'MD_Big.MD_Big', 'UMoodDef_C'), (MOD + 'ED_AssetTest.ED_AssetTest', 'EnemyDescriptor')}, ar
    print('ok  AssetTest: the asset registry lists every asset with its class')


def ue_assets():
    """genueassets names each asset a registry lists by its content path, one header per class; a mod reaching one
    through that header imports exactly that object. The registry here is AssetTest's, the game's in real use."""
    import tempfile
    with tempfile.TemporaryDirectory(dir=TESTS) as tmp:
        out = os.path.join(tmp, 'UeAssets')
        proc = subprocess.run([sys.executable, os.path.join(HERE, 'genueassets.py'), registry_of('AssetTest'), UEAPI, out],
                              capture_output=True, encoding='utf-8')
        assert proc.returncode == 0, proc.stdout + proc.stderr
        h = open(os.path.join(out, 'UEnemyDescriptor.h'), encoding='utf-8-sig').read()
        assert 'UE_ASSET_AT(::UEnemyDescriptor, ED_AssetTest, "/Game/_ElytrasMods/AssetTest/ED_AssetTest");' in h, h
        assert not os.path.exists(os.path.join(out, 'UBlueprintGeneratedClass.h')), os.listdir(out)
        with open(os.path.join(tmp, 'UeAssetsUser.cpp'), 'w') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeAssets/UEnemyDescriptor.h"\n'
                    'UE_MOD_PACKAGE("/Game/_ElytrasMods/UeAssetsUser");\n'
                    'class UeAssetsUser : public AActor {\npublic:\n'
                    '  UEnemyDescriptor *Ed = &UeAssets::UEnemyDescriptor::Game::_ElytrasMods::AssetTest::ED_AssetTest;\n'
                    '  int32 Count() { return UeAssets::UEnemyDescriptor::All.Num(); }\n};\n')
        proc = assetgen_compile([os.path.join(tmp, 'UeAssetsUser.cpp'), UEAPI, tmp])
        assert proc.returncode == 0, proc.stdout + proc.stderr
        user = os.path.join(tmp, 'UeAssetsUser')
        cdo = dump('dumptags.py', user, exports_of(user).index('Default__UeAssetsUser_C'))
        ed = int(re.search(r'Ed \[0\] ObjectProperty size=4: index (-?\d+)', cdo).group(1))
        assert ref(user, ed) == '/Game/_ElytrasMods/AssetTest/ED_AssetTest.ED_AssetTest', cdo
        assert global_default(tmp, 'UeAssets__UEnemyDescriptor__All', 'All') == ['/Game/_ElytrasMods/AssetTest/ED_AssetTest.ED_AssetTest']
        # --pak: a pak with no registry of its own (mint's) names its assets from each .uasset's export table. IfaceTest's
        # registry has no enemy descriptor, so ED_AssetTest can only come from AssetTest's cooked folder.
        for paks, has in (([], False), (['--pak', os.path.join(ROOT, 'AssetTest')], True)):
            out = os.path.join(tmp, 'UeAssetsPak%d' % len(paks))
            proc = subprocess.run([sys.executable, os.path.join(HERE, 'genueassets.py'), registry_of('IfaceTest'), UEAPI, out]
                                  + paks, capture_output=True, encoding='utf-8')
            assert proc.returncode == 0, proc.stdout + proc.stderr
            h = os.path.join(out, 'UEnemyDescriptor.h')
            assert os.path.exists(h) == has, os.listdir(out)
            assert not os.path.exists(os.path.join(out, 'UBlueprintGeneratedClass.h')), os.listdir(out)
        assert 'UE_ASSET_AT(::UEnemyDescriptor, ED_AssetTest, "/Game/_ElytrasMods/AssetTest/ED_AssetTest");' in \
            open(h, encoding='utf-8-sig').read()
    print('ok  genueassets: a header per class names each asset by its path, and a mod reaches one, or All, through it;'
          ' --pak adds a pak\'s assets from their own headers')


def asset_elsewhere():
    """A UE_ASSET_AT into another mod, of a class declared in a header both mods include. Unpinned, each mod cooks its
    own copy of the class and the asset is an instance of the other copy, so in game the reference loads as null
    (BpMods' OffsetsData, 2026-09-18): refused. Pinned with UE_CLASS, the import names the owner's class."""
    import tempfile
    top = ('class UOtherDef : public UPrimaryDataAsset {\npublic:\n%s  int32 N = 1;\n};\n'
           'UE_ASSET_AT(UOtherDef, OtherData, "/Game/_ElytrasMods/Other/OtherData");\n')
    refused('AssetElsewhere', '  UOtherDef *Picked = &OtherData;\n',
            'OtherData at /Game/_ElytrasMods/Other/OtherData is a UOtherDef, which this mod cooks its own copy of, so it '
            'would load as null: name the class\'s owner where it is declared, e.g. '
            'UE_CLASS("/Game/_ElytrasMods/Other/UOtherDef", "UOtherDef_C")', top=top % '')
    with tempfile.TemporaryDirectory(dir=TESTS) as tmp:
        with open(os.path.join(tmp, 'AssetPinned.cpp'), 'w') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/AssetPinned");\n'
                    + top % '  UE_CLASS("/Game/_ElytrasMods/Other/UOtherDef", "UOtherDef_C");\n'
                    + 'class AssetPinned : public AActor {\npublic:\n  UOtherDef *Picked = &OtherData;\n};\n')
        proc = assetgen_compile([os.path.join(tmp, 'AssetPinned.cpp'), UEAPI, tmp])
        assert proc.returncode == 0, proc.stdout + proc.stderr
        assert not os.path.exists(os.path.join(tmp, 'UOtherDef.uasset')), os.listdir(tmp)
        imports = dict(import_paths(os.path.join(tmp, 'AssetPinned'), classes=True))
        assert imports['/Game/_ElytrasMods/Other/OtherData.OtherData'] == '/Game/_ElytrasMods/Other/UOtherDef.UOtherDef_C', imports
    print("ok  AssetTest: another mod's asset of a class this mod would cook is refused; with the class pinned to its "
          "owner, the import names the owner's class")


def global_default(folder, cls, member):
    """The default a generated global class holds: a soft path (list) decoded from its FName indices, else the tag."""
    import struct
    base = os.path.join(folder, cls)
    tags = dump('dumptags.py', base, exports_of(base).index('Default__%s_C' % cls))
    m = re.search(r'^  %s \[0\] (\w+) size=\d+( inner=\w+)?: (.*)$' % member, tags, re.M)
    if not m: return None
    if 'SoftObjectProperty' not in m.group(0): return m.group(3)
    names, raw = dumpexp.load(base)[3], bytes.fromhex(m.group(3))
    one = m.group(1) == 'SoftObjectProperty'                                # a lone FSoftObjectPath: FName, sub-path
    at = [0] if one else range(4, 4 + 12 * struct.unpack_from('<i', raw)[0], 12)
    paths = [names[struct.unpack_from('<i', raw, o)[0]] for o in at]
    return paths[0] if one else paths


def globals_():
    """GlobalTest: each namespace-scope variable a function uses is the one member of a class generated for it,
    <Ns>__<Name>, whose default object holds the initializer. Every class of the mod reads and writes that one object.
    UE_ASSET_ALL's All lists, as soft paths, the UE_ASSET_ATs under its namespace whose class is its element class."""
    folder = os.path.dirname(asset('GlobalTest'))
    assert global_default(folder, 'Counter', 'Counter') == '5'
    assert global_default(folder, 'Greeting', 'Greeting') == "'hi'"
    assert global_default(folder, 'Tally__Hits', 'Hits') is None           # zero, as a C++ global with no initializer
    picks = global_default(folder, 'Picks__All', 'All')
    assert sorted(picks) == ['/Game/Enemies/Spider/Exploder/ED_Spider_Exploder.ED_Spider_Exploder',
                             '/Game/Enemies/Spider/Grunt/ED_Spider_Grunt.ED_Spider_Grunt'], picks   # not the texture
    web = '/Game/LevelElements/RoomObjects/Hazards/StickySpiderWeb/T_StickySpiderWeb_Corner'
    assert global_default(folder, 'GlobalTest', 'WebIcon') == web + '.T_StickySpiderWeb_Corner'
    print('ok  GlobalTest: each global is its own class\'s default, its initializer; All is its namespace\'s assets of its class')
    # Run: the default objects as their cooked defaults say, shared by both classes.
    objs = {'Default__Counter_C': Obj('Counter_C', Counter=5), 'Default__Greeting_C': Obj('Greeting_C', Greeting='hi'),
            'Default__Tally__Hits_C': Obj('Tally__Hits_C'), 'Default__Picks__All_C': Obj('Picks__All_C', All=list(picks))}
    vm, peer = VM(asset('GlobalTest'), objects=objs), VM(os.path.join(folder, 'GlobalPeer'), objects=objs)
    assert [vm.call('Bump', 2), vm.call('Bump', 3), peer.call('Read')] == [7, 10, 1002]
    assert [vm.call('Take'), vm.call('Take'), peer.call('Read')] == [10, 11, 1202]     # Counter++ is the value before
    assert vm.call('Greet') == 'hi!' and vm.call('PickCount') == 2 and vm.call('FirstPick') == picks[0]
    print('ok  GlobalTest: both classes read and write the one object: =, op=, ++ and a postfix value')


def edits():
    """EditTest (S38): UE_ASSET_EDIT and UE_PATCH rewrite AssetTest's cooked packages in place. Only the named tags
    change (replaced where they were, or appended); every name and import the package had keeps its index, a new one
    is appended; an object a new tag points at is created before the edited object is serialized. The build's
    roundtrip gate has already read every edited package back."""
    import struct
    game = lambda p: os.path.join(ROOT, 'AssetTest', 'FSD', 'Content', '_ElytrasMods', 'AssetTest', p)
    edited = lambda p: os.path.join(ROOT, 'EditTest', 'FSD', 'Content', '_ElytrasMods', 'AssetTest', p)
    tag_line = re.compile(r'^  (\w+) \[0\] (\w+ size=\d+[^:]*: ?.*)$', re.M)
    tags = lambda base, i: {m.group(1): m.group(2) for m in tag_line.finditer(dump('dumptags.py', base, i))}

    a, b = game('ED_AssetTest'), edited('ED_AssetTest')
    before, after = tags(a, 0), tags(b, 0)
    assert list(after) == list(before) + ['EnemySignificance'], (list(before), list(after))     # replaced in place, one appended
    assert after['SpawnSpread'].endswith(': 800.0') and 'value=0' in after['CanBeUsedForConstantPressure'], after
    # EEnemySignificance is an enum class: UEnemyDescriptor's member is an EnumProperty, and so is the tag (PropEnumClass)
    assert after['EnemySignificance'] == 'EnumProperty size=8 enum=EEnemySignificance: EEnemySignificance::Critical', after
    for same in ('EnemyClass', 'IdealSpawnSize'):
        assert after[same] == before[same], (same, before[same], after[same])
    la, lb = dumpexp.load(a), dumpexp.load(b)
    assert lb[3][:len(la[3])] == la[3] and lb[4][:len(la[4])] == la[4], 'a name or an import moved'
    raw = bytes.fromhex(after['VeteranClasses'].split()[-1])
    vets = struct.unpack_from('<%di' % struct.unpack_from('<i', raw)[0], raw, 4)
    paths = [ref(b, v) for v in vets]
    assert paths == ['/Game/Enemies/Spider/Grunt/ED_Spider_Grunt.ED_Spider_Grunt',
                     '/Game/Enemies/Spider/Exploder/ED_Spider_Exploder.ED_Spider_Exploder'], paths
    assert -vets[1] - 1 >= len(la[4]), 'the Exploder was not a new import'
    assert vets[1] in dumpexp.preload(b)[0][1], dumpexp.preload(b)[0]                # create before serialize
    print('ok  EditTest: UE_ASSET_EDIT replaces and adds tags of a cooked asset, appending what the package lacks')

    a, b = game('UMoodDef'), edited('UMoodDef')
    ea, eb = dumpexp.load(a)[5], dumpexp.load(b)[5]
    cdo = [e['name'] for e in ea].index('Default__UMoodDef_C')
    before, after = tags(a, cdo), tags(b, cdo)
    assert list(after) == list(before) + ['Tag'], (list(before), list(after))
    assert after['Health'].endswith(': 42.0') and after['Count'].endswith(': 0') and after['Tag'].endswith(': tweaked'), after
    assert after['Title'] == before['Title'] and after['Mood'] == before['Mood'], after
    ua, ub = dumpexp.load(a), dumpexp.load(b)
    blob = lambda l, e: l[1][e['off'] - l[2]:e['off'] - l[2] + e['size']]
    for i, (x, y) in enumerate(zip(ea, eb)):
        assert i == cdo or blob(ua, x) == blob(ub, y), 'export %s changed' % x['name']     # the class, its functions: the game's
    print('ok  EditTest: UE_PATCH edits a Blueprint class\'s default object, and nothing else of its package')

    import tempfile
    game_dir = os.path.join(ROOT, 'AssetTest', 'FSD', 'Content')
    for name, src, why, flags in (
            ('EditOwn', 'UMoodDef MD = {.Count = 1};\nUE_ASSET_EDIT(MD) {.Count = 2};\n', 'the target is a UE_ASSET_AT',
             ['--game', game_dir]),
            ('EditNative', 'class Tweaks : public AActor {\n  UE_PATCH;\n  UE_DEFAULTS { bHidden = true; }\n};\n',
             'a patch derives from the game Blueprint it edits', ['--game', game_dir]),
            ('EditMember', 'class Tweaks : public UMoodDef {\n  UE_PATCH;\n  int32 Extra;\n  UE_DEFAULTS { Count = 1; }\n};\n',
             'a member or an interface of its own is not built yet', ['--game', game_dir]),
            ('EditNoGame', 'class Tweaks : public UMoodDef {\n  UE_PATCH;\n  UE_DEFAULTS { Count = 1; }\n};\n',
             'pass the folder /Game is in', [])):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, name + '.cpp')
            with open(path, 'w', encoding='utf-8') as f:
                f.write('#include "UeApi/Types.h"\n#include "UeApi/Engine.h"\n#include "UeApi/FSD.h"\n'
                        'UE_MOD_PACKAGE("/Game/_ElytrasMods/%s");\n'
                        'class UMoodDef : public UPrimaryDataAsset {\npublic:\n'
                        '  UE_CLASS("/Game/_ElytrasMods/AssetTest/UMoodDef", "UMoodDef_C");\n  int32 Count;\n};\n%s' % (name, src))
            proc = assetgen_compile([path, UEAPI, tmp] + flags)
            assert proc.returncode != 0 and why in proc.stdout, (name, proc.stdout)
    print('ok  EditTest: an edit of a mod\'s own asset, a patch of a native class, a patch with a member of its own, and '
          'no --game are refused')

    # A component's defaults. CompTest stands in for a game Blueprint, declared as UeApi declares one, and Lamp for one of
    # its own SCS components: the patch lands in the Lamp's template. A parent Blueprint's component the class does not
    # override has no template in its package, and a member UeApi gives no SCS node is not a component: both refused.
    comp_game = os.path.join(ROOT, 'CompTest', 'FSD', 'Content')
    decl = ('#include "UeApi/Types.h"\n#include "UeApi/Engine.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/LampEdit");\n'
            'class Parent : public AActor {\npublic:\n  UE_CLASS("/Game/Fake/Parent", "Parent_C");\n  class USceneComponent* Ghost;\n'
            '  static constexpr const char* Ghost__UeScsNode = "00000000000000000000000000000000";\n};\n'
            'class CompTest : public Parent {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/CompTest/CompTest", "CompTest_C");\n'
            '  class UPointLightComponent* Lamp;\n'
            '  static constexpr const char* Lamp__UeScsNode = "00000000000000000000000000000000";\n'
            '  class USceneComponent* Loose;\n};\n'
            'class LampTweaks : public CompTest {\n  UE_PATCH;\n  UE_DEFAULTS { %s }\n};\n')
    for body, why in (('Lamp->Intensity = 5000.0f; Lamp->AttenuationRadius = 900.0f;', None),
                      ('Ghost->bVisible = false;', 'does not override that inherited component'),
                      ('Loose->bVisible = false;', 'is not one of CompTest_C\'s components')):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'LampEdit.cpp')
            with open(path, 'w', encoding='utf-8') as f:
                f.write(decl % body)
            out = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'LampEdit')
            os.makedirs(out)
            proc = assetgen_compile([path, UEAPI, out, '--game', comp_game])
            if why:
                assert proc.returncode != 0 and why in proc.stdout, (body, proc.stdout)
                continue
            assert proc.returncode == 0, proc.stdout + proc.stderr
            a = os.path.join(comp_game, '_ElytrasMods', 'CompTest', 'CompTest')
            b = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'CompTest', 'CompTest')
            la, lb = dumpexp.load(a), dumpexp.load(b)
            lamp = [e['name'] for e in la[5]].index('Lamp_GEN_VARIABLE')
            before, after = tags(a, lamp), tags(b, lamp)
            assert list(after) == list(before) + ['AttenuationRadius'], (list(before), list(after))    # Intensity replaced in place
            assert after['Intensity'].endswith(': 5000.0') and after['AttenuationRadius'].endswith(': 900.0'), after
            assert all(i == lamp or blob(la, x) == blob(lb, y) for i, (x, y) in enumerate(zip(la[5], lb[5]))), 'another export changed'
    print('ok  EditTest: UE_PATCH edits a Blueprint\'s own component in its SCS template; an inherited one without an '
          'override record, and a member that is no component, are refused')

    # A method replaces the Blueprint's function of that name: CompTest's ReceiveBeginPlay (Ticks + 1) becomes Ticks + 5,
    # run offline. `CompTest::ReceiveBeginPlay()` in it runs the game's body, kept beside it. A method the Blueprint
    # lacks is added to it: a helper, or an override of what it inherits (AActor's ReceiveTick). A function the class's
    # declaration has but its package lacks, and other parameters than the game's, are refused.
    decl = ('#include "UeApi/Types.h"\n#include "UeApi/Engine.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/FnEdit");\n'
            'class CompTest : public AActor {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/CompTest/CompTest", "CompTest_C");\n'
            '  int32 Ticks;\n  void ReceiveBeginPlay();\n  void Nope();\n};\n'
            'class Tweaks : public CompTest {\n  UE_PATCH;\n  %s\n};\n')
    for body, why, ticks, new in (
            ('void ReceiveBeginPlay() { int32 Step = 5; Ticks = Ticks + Step; }', None, 6, []),
            ('void ReceiveBeginPlay() { CompTest::ReceiveBeginPlay(); Ticks = Ticks + 5; }', None, 7, ['ReceiveBeginPlay__Vanilla']),
            ('void ReceiveBeginPlay() { Ticks = Twice(Ticks) + 5; }\n  int32 Twice(int32 X) { return X * 2; }\n'
             '  void ReceiveTick(float DeltaSeconds) { Ticks = Ticks + 1; }', None, 7, ['Twice', 'ReceiveTick']),
            ('void Nope() { Ticks = 1; }', 'has no function of that name of its own', 0, []),
            ('void ReceiveBeginPlay(int32 X) { Ticks = X; }', 'are not the game function\'s', 0, [])):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'FnEdit.cpp')
            with open(path, 'w', encoding='utf-8') as f:
                f.write(decl % body)
            out = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'FnEdit')
            os.makedirs(out)
            proc = assetgen_compile([path, UEAPI, out, '--game', comp_game])
            if why:
                assert proc.returncode != 0 and why in proc.stdout, (body, proc.stdout)
                continue
            assert proc.returncode == 0, proc.stdout + proc.stderr
            a = os.path.join(comp_game, '_ElytrasMods', 'CompTest', 'CompTest')
            b = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'CompTest', 'CompTest')
            for base, want in ((a, 2), (b, ticks)):
                me = {'Ticks': 1}
                run(base, 'ReceiveBeginPlay', self_vars=me)
                assert me == {'Ticks': want}, (base, me)
            la, lb = dumpexp.load(a), dumpexp.load(b)
            assert sorted(e['name'] for e in lb[5][len(la[5]):]) == sorted(new), [e['name'] for e in lb[5]]
            if 'ReceiveBeginPlay__Vanilla' in new:                  # the game's body, kept for the Parent:: call
                me = {'Ticks': 1}
                run(b, 'ReceiveBeginPlay__Vanilla', self_vars=me)
                assert me == {'Ticks': 2}, me
            if 'ReceiveTick' in new:                                # an override of AActor's event, which it names
                me = {'Ticks': 1}
                run(b, 'ReceiveTick', self_vars=me, DeltaSeconds=0.5)
                assert me == {'Ticks': 2}, me
                tick = dump('dumpstruct.py', b, [e['name'] for e in lb[5]].index('ReceiveTick'))
                assert re.search(r"^SuperStruct imp\[\d+\]:Function'ReceiveTick'$", tick, re.M), tick
            import invariants
            pa, pb = invariants.Package(a), invariants.Package(b)

            def tick_only(i):
                """An added ReceiveTick turns the default object's tick on (KismetCompiler.cpp 4738-4839, which
                added_tick_can_tick checks): its tags are the game's plus PrimaryActorTick, and the rest is the game's."""
                ta, tb = pa.tags(i), pb.tags(i)
                key = lambda t: (t['name'], t['type'], t['index'], bytes(t['value']))
                return (not pa.tag(i, 'PrimaryActorTick') and pa.blob(i)[ta.end:] == pb.blob(i)[tb.end:]
                        and [key(t) for t in tb if t['name'] != 'PrimaryActorTick'] == [key(t) for t in ta])
            assert all(x['name'] == 'ReceiveBeginPlay' or new and x['name'] == 'CompTest_C' or blob(la, x) == blob(lb, y)
                       or 'ReceiveTick' in new and x['name'] == 'Default__CompTest_C' and tick_only(i)
                       for i, (x, y) in enumerate(zip(la[5], lb[5]))), 'another export changed'
            proc = subprocess.run([ASSETGEN, 'roundtrip', tmp], capture_output=True, encoding='utf-8')
            assert proc.returncode == 0, proc.stdout
            n = 2 + len(new)
            assert 'functions whose payload reads exactly (ReadFunctionLayout): %d of %d' % (n, n) in proc.stdout, proc.stdout
            assert 'classes whose payload reads exactly (ReadClassLayout): 1 of 1' in proc.stdout, proc.stdout
    # An RPC's game body is kept as a plain function: with its net flags it would be a net field of its own, shifting
    # the class's RPCs against the game's, and a call to it would be routed again. The replacement keeps them.
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'RpcEdit.cpp')
        with open(path, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/Engine.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/RpcEdit");\n'
                    'class ReplTest : public AActor {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/ReplTest/ReplTest", "ReplTest_C");\n'
                    '  int32 Local;\n  UE_CLIENT void ClientPing(int32 Seq);\n};\n'
                    'class Tweaks : public ReplTest {\n  UE_PATCH;\n'
                    '  void ClientPing(int32 Seq) { ReplTest::ClientPing(Seq); Local = Local + 1; }\n};\n')
        out = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'RpcEdit')
        os.makedirs(out)
        proc = assetgen_compile([path, UEAPI, out, '--game', os.path.join(ROOT, 'ReplTest', 'FSD', 'Content')])
        assert proc.returncode == 0, proc.stdout + proc.stderr
        b = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'ReplTest', 'ReplTest')
        me = {}
        run(b, 'ClientPing', self_vars=me, Seq=5)
        assert me == {'Local': 6}, me
        names = [e['name'] for e in dumpexp.load(b)[5]]
        flags = lambda fn: int(re.search(r'^FunctionFlags (0x[0-9a-f]+)$', dump('dumpstruct.py', b, names.index(fn)), re.M).group(1), 16)
        assert flags('ClientPing') & 0x01000040 == 0x01000040 and flags('ClientPing__Vanilla') & 0x01000040 == 0, \
            (hex(flags('ClientPing')), hex(flags('ClientPing__Vanilla')))       # FUNC_Net | FUNC_NetClient
        proc = subprocess.run([ASSETGEN, 'roundtrip', tmp], capture_output=True, encoding='utf-8')
        assert proc.returncode == 0 and 'classes whose payload reads exactly (ReadClassLayout): 1 of 1' in proc.stdout, proc.stdout
    print('ok  EditTest: a UE_PATCH method replaces the Blueprint\'s function of that name, Parent:: reaching the game\'s body '
          '(an RPC\'s as a plain function), or is added to it (a helper, an override); a stale declaration and other '
          'parameters are refused')

    # A namespace-scope variable a patch's method uses is the one member of a class of its own (LowerGlobal), cooked into
    # the mod's package as a class's global is: the patched function reads and writes that class's default object.
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'GlobalEdit.cpp')
        with open(path, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/Engine.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/GlobalEdit");\n'
                    'class CompTest : public AActor {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/CompTest/CompTest", "CompTest_C");\n'
                    '  int32 Ticks;\n  void ReceiveBeginPlay();\n};\n'
                    'int32 Step = 5;\n'
                    'class Tweaks : public CompTest {\n  UE_PATCH;\n'
                    '  void ReceiveBeginPlay() { Ticks = Ticks + Step; Step = Step + 1; }\n};\n')
        out = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'GlobalEdit')
        os.makedirs(out)
        proc = assetgen_compile([path, UEAPI, out, '--game', comp_game])
        assert proc.returncode == 0, proc.stdout + proc.stderr
        assert os.path.exists(os.path.join(out, 'Step.uasset')), 'the global\'s class was not cooked: %s' % os.listdir(out)
        assert global_default(out, 'Step', 'Step') == '5'
        step = Obj('Step_C', Step=5)
        vm = VM(os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'CompTest', 'CompTest'),
                objects={'Default__Step_C': step}, Ticks=1)
        vm.call('ReceiveBeginPlay')
        assert vm.self.vars == {'Ticks': 6} and step.vars == {'Step': 6}, (vm.self.vars, step.vars)
    print('ok  EditTest: a namespace-scope variable a UE_PATCH method uses has its class cooked, whose default object the '
          'patched function reads and writes')


def path_edits():
    """S38: a patch's UE_DEFAULTS path assigns part of a member's value on TypesTest's default object - a native
    struct's member inside an element (Points[1].Y), a member of a struct written as tags inside one (Spans[0].Max), a
    whole element (Spans[1]) - and the rest of each value stays the cook's bytes. A member the default object has no
    value of takes only a path of members of structs written as tags (Home is native); an index past the end is
    refused."""
    import struct, tempfile
    from dumptags import tags as read_tags
    a = os.path.join(ROOT, 'TypesTest', 'FSD', 'Content', '_ElytrasMods', 'TypesTest', 'TypesTest')
    decl = ('#include "UeApi/Types.h"\n#include "UeApi/Engine.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/PathEdit");\n'
            'class TypesTest : public AActor {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/TypesTest/TypesTest", "TypesTest_C");\n'
            '  FVector Home;\n  TArray<FVector> Points;\n  TArray<FFloatInterval> Spans;\n};\n'
            'class Tweaks : public TypesTest {\n  UE_PATCH;\n  UE_DEFAULTS { %s }\n};\n')
    blob = lambda l, e: l[1][e['off'] - l[2]:e['off'] - l[2] + e['size']]

    def value(base, name):
        """The default object's tag `name`: its value's bytes, and the package's names."""
        l = dumpexp.load(base)
        text = dump('dumptags.py', base, [e['name'] for e in l[5]].index('Default__TypesTest_C'))
        return bytes.fromhex(re.search(r'^  %s \[0\] \w+ size=\d+[^:]*: ([0-9a-f]*)$' % name, text, re.M).group(1)), l[3]

    def spans(base):
        """Spans' elements, each its members' values: after the count, the inner tag (49 bytes), then tag lists."""
        b, names = value(base, 'Spans')
        at, out = 4 + 49, []
        for _ in range(struct.unpack_from('<i', b)[0]):
            lines = []
            at = read_tags(b, at, len(b), names, 0, lines)
            out.append([l.split(': ')[-1] for l in lines])
        assert at == len(b) and struct.unpack_from('<i', b, 4 + 16)[0] == len(b) - 4 - 49, (at, len(b))    # the inner tag's Size
        return out

    points = lambda base: struct.unpack_from('<6f', value(base, 'Points')[0], 4 + 49)
    assert points(a) == (1, 2, 3, 4, 5, 6) and spans(a) == [['1.0', '5.0'], ['-2.0', '2.0']], (points(a), spans(a))
    for body, why in (('Points[1].Y = 50.0f; Spans[0].Max = 9.0f; Spans[1] = FFloatInterval{3.0f, 4.0f};', None),
                      ('Home.Z = 1.0f;', 'Home is not set on Default__TypesTest_C'),
                      ('Points[2].X = 1.0f;', 'Points has 2 elements')):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'PathEdit.cpp')
            with open(path, 'w', encoding='utf-8') as f:
                f.write(decl % body)
            out = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'PathEdit')
            os.makedirs(out)
            proc = assetgen_compile([path, UEAPI, out, '--game', os.path.join(ROOT, 'TypesTest', 'FSD', 'Content')])
            if why:
                assert proc.returncode != 0 and why in proc.stdout, (body, proc.stdout)
                continue
            assert proc.returncode == 0, proc.stdout + proc.stderr
            b = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'TypesTest', 'TypesTest')
            assert points(b) == (1, 2, 3, 4, 50, 6) and spans(b) == [['1.0', '9.0'], ['3.0', '4.0']], (points(b), spans(b))
            la, lb = dumpexp.load(a), dumpexp.load(b)
            cdo = [e['name'] for e in la[5]].index('Default__TypesTest_C')
            other = lambda base: [l for l in dump('dumptags.py', base, cdo).splitlines() if not l.startswith(('  Points ', '  Spans '))]
            assert other(a) == other(b), 'another tag changed'
            assert all(i == cdo or blob(la, x) == blob(lb, y) for i, (x, y) in enumerate(zip(la[5], lb[5]))), 'another export changed'
            proc = subprocess.run([ASSETGEN, 'roundtrip', tmp], capture_output=True, encoding='utf-8')
            assert proc.returncode == 0, proc.stdout
    print('ok  EditTest: a UE_DEFAULTS path assigns part of a value - a native struct\'s member, a tagged one\'s, an '
          'element - keeping the rest of it; a native member with no value, and an index past the end, are refused')

    # UE_ASSET_EDITS: the same paths into a data asset, AssetTest's ED_AssetTest. The element's new object is one the
    # package does not import yet; a whole member beside it.
    a = os.path.join(ROOT, 'AssetTest', 'FSD', 'Content', '_ElytrasMods', 'AssetTest', 'ED_AssetTest')
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'AssetEdits.cpp')
        with open(path, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/AssetEdits");\n'
                    'UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Exploder, "/Game/Enemies/Spider/Exploder/ED_Spider_Exploder");\n'
                    'UE_ASSET_AT(UEnemyDescriptor, ED_AssetTest, "/Game/_ElytrasMods/AssetTest/ED_AssetTest");\n'
                    'UE_ASSET_EDITS {\n  ED_AssetTest.VeteranClasses[0] = &ED_Spider_Exploder;\n  ED_AssetTest.IdealSpawnSize = 9;\n}\n')
        out = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'AssetEdits')
        os.makedirs(out)
        proc = assetgen_compile([path, UEAPI, out, '--game', os.path.join(ROOT, 'AssetTest', 'FSD', 'Content')])
        assert proc.returncode == 0, proc.stdout + proc.stderr
        b = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'AssetTest', 'ED_AssetTest')
        la, lb = dumpexp.load(a), dumpexp.load(b)
        assert lb[4][:len(la[4])] == la[4] and [i for i in lb[4][len(la[4]):] if 'Exploder' in i], lb[4]     # appended
        exploder = -1 - lb[4].index(next(i for i in lb[4][len(la[4]):] if i.endswith("'ED_Spider_Exploder'")))
        before, after = dump('dumptags.py', a, 0).splitlines(), dump('dumptags.py', b, 0).splitlines()
        changed = [(x.split(':')[0].strip(), y.split(': ')[-1]) for x, y in zip(before, after) if x != y]
        assert len(before) == len(after) and changed == [
            ('VeteranClasses [0] ArrayProperty size=8 inner=ObjectProperty', (struct.pack('<ii', 1, exploder)).hex()),
            ('IdealSpawnSize [0] IntProperty size=4', '9')], changed
        proc = subprocess.run([ASSETGEN, 'roundtrip', tmp], capture_output=True, encoding='utf-8')
        assert proc.returncode == 0, proc.stdout
    print('ok  EditTest: UE_ASSET_EDITS assigns a data asset\'s members and parts of them by path, an element\'s new '
          'object imported')


def game_edits():
    """S38 on the game's own packages (--game): ED_Spider_Grunt and the grunt Blueprint's class defaults, edited as
    BpMods' GruntTweaks does. Only the named tags differ from the cook's; every other export is its bytes."""
    if not GAME or not os.path.exists(os.path.join(UEAPI, 'Game', 'ENE_Spider_Grunt_Normal_C.h')):
        print('--  S38 on the game\'s own packages: skipped (needs --game <extracted pak>/FSD/Content and UeApi/Game)')
        return
    import tempfile
    tag_line = re.compile(r'^  (\w+) \[0\] (\w+ size=\d+[^:]*: ?.*)$', re.M)
    tags = lambda base, i: {m.group(1): m.group(2) for m in tag_line.finditer(dump('dumptags.py', base, i))}
    blob = lambda l, e: l[1][e['off'] - l[2]:e['off'] - l[2] + e['size']]
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'GameEdit.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/Engine.h"\n#include "UeApi/FSD.h"\n'
                    '#include "UeApi/Game/ENE_Spider_Grunt_Normal_C.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/GameEdit");\n'
                    'UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");\n'
                    'UE_ASSET_EDIT(ED_Spider_Grunt) {.SpawnSpread = 800.0f, .IdealSpawnSize = 12};\n'
                    'class GruntTweaks : public ENE_Spider_Grunt_Normal_C {\n  UE_PATCH;\n'
                    '  UE_DEFAULTS {\n    CustomTimeDilation = 0.5f;\n'
                    '    HealthComponent->MaxHealth = 180.0f;\n'       # a native class's component: its default subobject
                    '    MeleeAttack->CenterOnTarget = true;\n'       # the Blueprint's own SCS component: its template
                    '    enemy->mixerName = "Grunty";\n  }\n'         # a parent's component it overrides: the record's template
                    '  void GetEnemySpawnedCount(int& SpawnCount) {\n'
                    '    ENE_Spider_Grunt_Normal_C::GetEnemySpawnedCount(SpawnCount);\n'   # the game's body, kept
                    '    SpawnCount = SpawnCount + 41;\n  }\n};\n')
        out = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'GameEdit')
        os.makedirs(out)
        proc = assetgen_compile([src, UEAPI, out, '--game', GAME])
        assert proc.returncode == 0, proc.stdout + proc.stderr
        proc = subprocess.run([ASSETGEN, 'roundtrip', tmp], capture_output=True, encoding='utf-8')
        assert proc.returncode == 0, proc.stdout
        game = lambda p: os.path.join(GAME, *p.split('/'))
        edited = lambda p: os.path.join(tmp, 'FSD', 'Content', *p.split('/'))

        a, b = game('Enemies/Spider/Grunt/ED_Spider_Grunt'), edited('Enemies/Spider/Grunt/ED_Spider_Grunt')
        before, after = tags(a, 0), tags(b, 0)
        assert list(after) == list(before) and {k for k in before if before[k] != after[k]} == {'SpawnSpread', 'IdealSpawnSize'}, after
        assert after['SpawnSpread'].endswith(': 800.0') and after['IdealSpawnSize'].endswith(': 12'), after

        a, b = game('Enemies/Spider/Grunt/ENE_Spider_Grunt_Normal'), edited('Enemies/Spider/Grunt/ENE_Spider_Grunt_Normal')
        la, lb = dumpexp.load(a), dumpexp.load(b)
        names = [e['name'] for e in la[5]]
        added = {'Default__ENE_Spider_Grunt_Normal_C': ('CustomTimeDilation', ': 0.5'), 'HealthComponent': ('MaxHealth', ': 180.0'),
                 'MeleeAttack_GEN_VARIABLE': ('CenterOnTarget', 'value=1:'), 'Enemy_GEN_VARIABLE': ('mixerName', ": 'Grunty'")}
        for name, (tag, value) in added.items():
            before, after = tags(a, names.index(name)), tags(b, names.index(name))
            assert list(after) == list(before) + [tag] and after[tag].rstrip().endswith(value), (name, list(before), after)
        assert all(x['name'] in added or x['name'] in ('GetEnemySpawnedCount', 'ENE_Spider_Grunt_Normal_C') or blob(la, x) == blob(lb, y)
                   for x, y in zip(la[5], lb[5])), 'another export changed'
        assert [e['name'] for e in lb[5][len(la[5]):]] == ['GetEnemySpawnedCount__Vanilla'], [e['name'] for e in lb[5]]
        assert lb[3][:len(la[3])] == la[3] and lb[4][:len(la[4])] == la[4], 'a name or an import moved'   # + Add_IntInt's
        # The replaced function: the game's (sets 1), then the patch's, which calls the game's body, kept, and adds 41.
        # Its parameter stays the game's output pin.
        assert run(a, 'GetEnemySpawnedCount')[1] == {'SpawnCount': 1} and run(b, 'GetEnemySpawnedCount')[1] == {'SpawnCount': 42}
        assert run(b, 'GetEnemySpawnedCount__Vanilla')[1] == {'SpawnCount': 1}
        fn = names.index('GetEnemySpawnedCount')
        parm = lambda base: re.search(r'^  IntProperty SpawnCount .*$', dump('dumpstruct.py', base, fn), re.M).group(0)
        assert parm(a) == parm(b) and 'flags=0x180 ' in parm(b), (parm(a), parm(b))

        # The grunt's Sphere is its parent's component, which it does not override: no template to edit.
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/Engine.h"\n#include "UeApi/FSD.h"\n'
                    '#include "UeApi/Game/ENE_Spider_Grunt_Normal_C.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/GameEdit");\n'
                    'class GruntTweaks : public ENE_Spider_Grunt_Normal_C {\n  UE_PATCH;\n'
                    '  UE_DEFAULTS { Sphere->SphereRadius = 10.0f; }\n};\n')
        proc = assetgen_compile([src, UEAPI, out, '--game', GAME])
        assert proc.returncode != 0 and 'does not override that inherited component' in proc.stdout, proc.stdout

    # Paths into the grunt's values: PrimaryActorTick, which its default object has no value of, becomes a tag holding
    # only bCanEverTick (the engine fills the rest in from the parent's); one element of MeleeAttack's Montages; one
    # element of an array inside SimpleArmorDamage's ArmorBreakEffects struct. Both objects are imports already.
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'GamePaths.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/Engine.h"\n#include "UeApi/FSD.h"\n'
                    '#include "UeApi/Game/ENE_Spider_Grunt_Normal_C.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/GamePaths");\n'
                    'UE_ASSET_AT(UAnimMontage, ANIM_Spider_Grunt_Attack_I, "/Game/Enemies/Spider/Animation/ANIM_Spider_Grunt_Attack_I");\n'
                    'UE_ASSET_AT(UParticleSystem, P_SpiderGrunt_Armor_Debris, '
                    '"/Game/Enemies/Spider/Particles/P_SpiderGrunt_Armor_Debris");\n'
                    'class GruntPaths : public ENE_Spider_Grunt_Normal_C {\n  UE_PATCH;\n  UE_DEFAULTS {\n'
                    '    PrimaryActorTick.bCanEverTick = true;\n'
                    '    MeleeAttack->Montages[0] = &ANIM_Spider_Grunt_Attack_I;\n'
                    '    SimpleArmorDamage->ArmorBreakEffects.DissolveParticles[0] = &P_SpiderGrunt_Armor_Debris;\n  }\n};\n'
                    'UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");\n'
                    'UE_ASSET_EDITS { ED_Spider_Grunt.SpawnRarityModifiers[1].Rarity = 2.0f; }\n')
        out = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'GamePaths')
        os.makedirs(out)
        proc = assetgen_compile([src, UEAPI, out, '--game', GAME])
        assert proc.returncode == 0, proc.stdout + proc.stderr
        a, b = game('Enemies/Spider/Grunt/ENE_Spider_Grunt_Normal'), os.path.join(tmp, 'FSD', 'Content', 'Enemies', 'Spider', 'Grunt', 'ENE_Spider_Grunt_Normal')
        la, lb = dumpexp.load(a), dumpexp.load(b)
        assert lb[4] == la[4], 'an import was added'
        cdo, melee, armor = (names.index(n) for n in ('Default__ENE_Spider_Grunt_Normal_C', 'MeleeAttack_GEN_VARIABLE',
                                                        'SimpleArmorDamage_GEN_VARIABLE'))
        before, after = dump('dumptags.py', a, cdo).splitlines(), dump('dumptags.py', b, cdo).splitlines()
        assert after[:len(before)] == before and [l.split(' size=')[0] for l in after[len(before):]] == \
            ['  PrimaryActorTick [0] StructProperty', '    bCanEverTick [0] BoolProperty'] and after[-1].endswith('value=1: '), after
        assert tags(a, melee)['Montages'].endswith(': 02000000fffffffffeffffff'), tags(a, melee)
        assert tags(b, melee)['Montages'].endswith(': 02000000fefffffffeffffff'), tags(b, melee)     # -1 -> -2, Attack_I
        before, after = dump('dumptags.py', a, armor).splitlines(), dump('dumptags.py', b, armor).splitlines()
        changed = [(x.strip(), y.strip()) for x, y in zip(before, after) if x != y]
        assert len(before) == len(after) and [(x.split(':')[0], y.split(':')[0]) for x, y in changed[:1]] == \
            [('ArmorBreakEffects [0] StructProperty size=90 struct=ArmorDamageEffects',) * 2] and changed[1:] == \
            [('DissolveParticles [0] ArrayProperty size=8 inner=ObjectProperty: 01000000b4ffffff',       # its nested line:
              'DissolveParticles [0] ArrayProperty size=8 inner=ObjectProperty: 01000000b3ffffff')], changed   # -76 -> -77
        assert all(i in (cdo, melee, armor) or blob(la, x) == blob(lb, y) for i, (x, y) in enumerate(zip(la[5], lb[5]))), 'another export changed'

        # UE_ASSET_EDITS on ED_Spider_Grunt: Rarity in one element of SpawnRarityModifiers, each element a tag list
        # after the array's inner tag. (The game's own values there are uninitialized memory from its cook: kept.)
        import struct
        from dumptags import tags as read_tags
        def rarities(base):
            b, at, out = bytes.fromhex(tags(base, 0)['SpawnRarityModifiers'].split(': ')[-1]), 4 + 49, []
            for _ in range(struct.unpack_from('<i', b)[0]):
                out.append([])
                at = read_tags(b, at, len(b), dumpexp.load(base)[3], 0, out[-1])
            return out
        ra = rarities(game('Enemies/Spider/Grunt/ED_Spider_Grunt'))
        rb = rarities(os.path.join(tmp, 'FSD', 'Content', 'Enemies', 'Spider', 'Grunt', 'ED_Spider_Grunt'))
        assert len(ra) == 4 and rb[:1] + rb[2:] == ra[:1] + ra[2:] and rb[1][0] == ra[1][0] \
            and rb[1][1] == 'Rarity [0] FloatProperty size=4: 2.0' and ra[1][1] != rb[1][1], (ra, rb)
        proc = subprocess.run([ASSETGEN, 'roundtrip', tmp], capture_output=True, encoding='utf-8')
        assert proc.returncode == 0, proc.stdout
    print('ok  S38 on the game: ED_Spider_Grunt, the grunt Blueprint\'s defaults, three kinds of its components, and one '
          'function replaced around the game\'s body, kept; paths into four of its values; the rest the cook\'s bytes')


def edit_staleness():
    """bpbuild counts a game package a mod's compile edited among its outputs: found in the staged Content tree outside
    the mod's own folder, with a copy in game_content (an embedded dep's folder has none). A newer game copy, from a
    re-extract after a game update, restales the mod, so an edit of the old package does not ship."""
    import tempfile
    sys.path.insert(0, HERE)
    import bpbuild
    with tempfile.TemporaryDirectory() as tmp:
        stage_fsd = os.path.join(tmp, 'build', 'Mod', 'FSD')
        own = os.path.join(stage_fsd, 'Content', '_ElytrasMods', 'Mod')
        game = os.path.join(tmp, 'game')
        def touch(p, t=None):
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').close()
            if t: os.utime(p, (t, t))
        touch(os.path.join(own, 'Mod.uasset'))
        touch(os.path.join(own + 'Two', 'Other.uasset'))                       # shares the prefix, not the folder
        touch(os.path.join(stage_fsd, 'Content', '_ElytrasMods', 'Dep', 'Dep.uasset'))
        touch(os.path.join(stage_fsd, 'Content', 'Enemies', 'Grunt', 'G.uasset'), 1000)
        touch(os.path.join(game, 'Enemies', 'Grunt', 'G.uasset'), 500)
        touch(os.path.join(game, '_ElytrasMods', 'ModTwo', 'Other.uasset'), 500)
        edits = lambda: bpbuild.staged_edits(stage_fsd, own, game)
        assert sorted(os.path.basename(s) for s, _g in edits()) == ['G.uasset', 'Other.uasset'], edits()
        assert bpbuild.staged_edits(stage_fsd, own, None) == []
        newer = lambda: any(os.path.getmtime(g) > os.path.getmtime(s) for s, g in edits())
        assert not newer()
        os.utime(os.path.join(game, 'Enemies', 'Grunt', 'G.uasset'), (2000, 2000))
        assert newer()
    print('ok  bpbuild: an edited game package is a mod\'s output, restaled by a newer game copy; an embedded dep is not one')


interfaces()
interface_bodies()
interface_calls()
inherited_defaults()
parent_call()
pure_virtual()
func_stub_super()
engine_names()
object_forwards()
api_stub()
static_assets()
ue_assets()
asset_elsewhere()
globals_()
edits()
path_edits()
game_edits()
edit_staleness()


# ---- SoftTest

def soft_conversions():
    """SoftTest: Kismet's soft reference conversions as C++ ones, run offline. A soft pointer is its path here, a class
    its path too; runvm refuses a call where a Conv_ reads its argument by address (native_refs), so Contains seeing
    the class also says it was put in a variable first."""
    A, B, C = '/Game/A/BP_A.BP_A_C', '/Game/B/BP_B.BP_B_C', '/Game/C/BP_C.BP_C_C'
    assert 'Conv_ClassToSoftClassReference' in runscript.NATIVE_REFS     # the control: the refusal is armed
    loaded = {}
    natives = {'GetObjectClass': lambda vm, ctx, o: o.cls if isinstance(o, Obj) else None,
               'Conv_ClassToSoftClassReference': lambda vm, ctx, c: c or '',
               'Conv_SoftClassReferenceToClass': lambda vm, ctx, s: s or None,
               'Conv_SoftClassReferenceToString': lambda vm, ctx, s: s,
               'Conv_ObjectToSoftObjectReference': lambda vm, ctx, o: o.vars['Path'] if isinstance(o, Obj) else '',
               'Conv_SoftObjectReferenceToObject': lambda vm, ctx, s: loaded.get(s),
               'Conv_SoftObjectReferenceToString': lambda vm, ctx, s: s,
               'EqualEqual_SoftClassReference': lambda vm, ctx, a, b: a.lower() == b.lower(),
               'NotEqual_SoftClassReference': lambda vm, ctx, a, b: a.lower() != b.lower()}
    vm = VM(asset('SoftTest'), natives, Kinds=[A, B], Kind=A, Draws=0)
    assert [vm.call('KnowsClassOf', Obj(c)) for c in (A, B, C)] == [True, True, False]
    assert [vm.call('KnowsInline', Obj(c)) for c in (A, B, C)] == [True, True, False]
    assert [vm.call('DrawIsPrime') for _ in range(7)] == [False, True, True, False, True, False, True]
    assert vm.self.vars['Draws'] == 7, vm.self.vars       # the item ran once per Contains, not once per element
    thing = Obj('Actor', Path='/Game/Maps/Cave.Cave:PersistentLevel.Thing_1')
    vm.call('Remember', thing)
    assert vm.self.vars['Seen'] == thing.vars['Path'] and vm.call('SeenPath') == thing.vars['Path'], vm.self.vars
    assert vm.call('Recall') is None                      # not loaded: null, as SoftObject.Get() answers
    loaded[thing.vars['Path']] = thing
    assert vm.call('Recall') is thing and vm.call('KindClass') == A
    assert vm.call('ClassPathOf', Obj(B)) == B            # the path, not Conv_ObjectToString's name
    assert (vm.call('SameKind', A), vm.call('SameKind', B), vm.call('OtherKind', B)) == (True, False, True)
    print('ok  SoftTest: an object or class becomes a soft pointer, a soft pointer its path or (cast) its object')
    print('ok  SoftTest: Contains on an inline list of constants runs its item once and compares it with each')


soft_conversions()


# ---- SubsystemTest

def subsystem_gets():
    """SubsystemTest: X::Get() is one call of the library getter for X's kind with X's class, as the editor's Get node
    makes it, and answers what that call does: no cast after it. A world context left out is self, or a static's own
    world context. The fake getters answer with what they were asked."""
    natives = dict((g, lambda vm, ctx, *a: a) for g in ('GetEngineSubsystem', 'GetGameInstanceSubsystem', 'GetWorldSubsystem'))
    vm = VM(asset('SubsystemTest'), natives)
    other, ctx = Obj('Actor'), Obj('Actor')
    for fn, args, getter, want in (('Engine', (), 'GetEngineSubsystem', ('UGCSubsystem',)),
                                   ('GameInstance', (), 'GetGameInstanceSubsystem', (vm.self, 'DamageSubsystem')),
                                   ('World', (), 'GetWorldSubsystem', (vm.self, 'TracerManager')),
                                   ('OtherWorld', (other,), 'GetWorldSubsystem', (other, 'TracerManager')),
                                   ('FromStatic', (ctx,), 'GetWorldSubsystem', (ctx, 'TracerManager')),
                                   ('Blueprint', (), 'GetWorldSubsystem', (vm.self, 'BP_TracerManager_C'))):
        del vm.log[:]
        got = vm.call(fn, *args)
        assert [(n, list(a)) for n, _, a in vm.log] == [(getter, list(want))] and tuple(got) == want, (fn, vm.log, got)
    print('ok  SubsystemTest: Get and GetSubsystem<T> reach the getter for their kind, the world context defaulting to self')


subsystem_gets()


# ---- ReplTest, LatentTest, AsyncTest, SpawnTest

def replication():
    import re, subprocess
    here, base = os.path.dirname(os.path.abspath(__file__)), asset('ReplTest')
    exports = [e['name'] for e in dumpexp.load(base)[5]]
    tool = lambda t, i: dump(t, base, str(i))
    cls = tool('dumpstruct.py', 0)
    assert 'NumReplicatedProperties [0] IntProperty size=4: 4' in cls, cls
    for prop, flags, notify, cond in (('Score', '0x10025', 'None', 0), ('bOpen', '0x100010025', 'OnRep_Open', 0),
                                      ('Aim', '0x10025', 'None', 3), ('Slots', '0x100010025', 'OnRep_Slots', 2),
                                      ('Local', '0x10005', 'None', 0)):
        assert re.search(r'Property %s .*flags=%s rep=\d+ notify=%s cond=%d' % (prop, flags, notify, cond), cls), prop
    for fn, flags in (('ServerOpen', 0xc2208c0), ('ClientPing', 0xd020840), ('MultiBoom', 0xc024840), ('OnRep_Open', 0xc020800),
                      ('ServerBump', 0xc620840), ('AuthOnly', 0xc020804), ('Pretty', 0xc020808)):
        assert 'FunctionFlags %#x' % flags in tool('dumpstruct.py', exports.index(fn)), fn
    print('ok  ReplTest: replicated properties, their conditions and notifies, RPC flags')

    # A mod child's override of a mod parent's RPC: the parent's flags, and the parent's function as its super.
    kid = os.path.join(os.path.dirname(base), 'ReplKid')
    imports, kid_exports = dumpexp.load(kid)[4], dumpexp.load(kid)[5]
    for fn, flags in (('ServerOpen', 0xc2208c0), ('MultiBoom', 0xc024840), ('OnRep_Open', 0xc020800)):
        e = next(x for x in kid_exports if x['name'] == fn)
        out = dump('dumpstruct.py', kid, str(kid_exports.index(e)))
        assert 'FunctionFlags %#x' % flags in out, (fn, out)
        assert e['super'] < 0 and imports[-e['super'] - 1] == "Function'%s'" % fn, (fn, e['super'])
    print("ok  ReplTest: an override of a mod parent's RPC keeps its net flags and names it as super")


def latent_flags():
    """The generated completion events: flags the engine checks before binding a delegate to them."""
    base = asset('LatentTest')
    for ev in ('Load_OnLoaded_0', 'Load_OnLoaded_1'):
        assert 'FunctionFlags 0xc000000' in dump('dumpstruct.py', base, export_index(base, ev)), ev
    names = set(exports_of(os.path.join(os.path.dirname(base), 'LatentJob')))
    assert names == {'LatentJob_C', 'Default__LatentJob_C', 'ExecuteUbergraph_LatentJob', 'Run'}, names
    base = asset('AsyncTest')
    for ev in ('Download_OnSuccess_0', 'Download_OnFail_0', 'PlayThen_OnCompleted_0'):
        assert 'FunctionFlags 0xc000000' in dump('dumpstruct.py', base, export_index(base, ev)), ev
    print('ok  LatentTest / AsyncTest: the generated events are what a delegate binds to')


def latent_runs():
    """LatentTest through its latent actions: each Delay / LoadAsset parks the function; firing the action
    resumes it after the call with its locals, on the actor, in the class's ubergraph."""
    lat = {n: latent_call for n in ('Delay', 'LoadAsset', 'LoadAssetClass')}
    vm = VM(asset('LatentTest'), lat, Stage=4, Log=[])
    vm.call('ReceiveBeginPlay')
    assert vm.self.vars == dict(Stage=1, Log=[]), vm.self.vars                      # parked in Delay(this, 0.5)
    [(fn, ctx, info, _)] = vm.latent
    assert (fn, ctx, info[2], info[3], vm.log[-1][2][1]) == ('Delay', vm.self, 'ExecuteUbergraph_LatentTest', vm.self, 0.5)
    vm.call('ViaInline')                                  # waits alongside, with an action of its own: Tag = 1 + 3
    assert len(vm.latent) == 2 and vm.latent[0][2][1] != vm.latent[1][2][1], vm.latent
    vm.fire(0)
    assert vm.self.vars == dict(Stage=12, Log=[]), vm.self.vars                     # Local (4 + 7) survived: + 1
    for i in range(3):                                    # one Delay(0.25) per round, I kept across each
        vm.fire(len(vm.latent) - 1)
        assert vm.self.vars['Log'][:i + 1] == list(range(i + 1)), vm.self.vars
    assert vm.self.vars['Log'] == [0, 1, 2, 100] and vm.log[-1][2][1] == 1.0, vm.self.vars   # Wait(1.0, 100) parked
    vm.fire(len(vm.latent) - 1)
    assert vm.self.vars['Log'] == [0, 1, 2, 100, 101], vm.self.vars                 # its Tag survived the Delay
    vm.fire(0)
    assert vm.self.vars['Stage'] == 4 and not vm.latent, (vm.self.vars, vm.latent)   # ViaInline's Tag, not Wait's
    print('ok  LatentTest: every latent call parks its function and resumes after it with the locals it had')
    for loaded in (Obj('Texture2D'), None):
        vm = VM(asset('LatentTest'), lat, Stage=9, Wanted='soft:Cls', Icon='soft:Icon')
        vm.call('Load')
        [(fn, ctx, info, dlg)] = vm.latent
        assert (fn, vm.log[-1][2][1], info[3]) == ('LoadAssetClass', 'soft:Cls', vm.self) and dlg, vm.latent
        vm.fire(0, result='Cls')
        assert vm.self.vars['Got'] == 'Cls' and [(a[0], vm.log[-1][2][1]) for a in vm.latent] == [('LoadAsset', 'soft:Icon')]
        vm.fire(0, result=loaded)
        assert vm.self.vars['Stage'] == (1 if loaded else 0) and not vm.latent, vm.self.vars
    print('ok  LatentTest.Load: LoadAssetClass / LoadAsset resume with the loaded value as the call\'s value')
    vm = VM(os.path.join(os.path.dirname(asset('LatentTest')), 'LatentJob'), {'Delay': latent_call})
    vm.call('Run', 0.75)
    [(fn, ctx, info, _)] = vm.latent
    assert ctx is vm.self and info[3] is vm.self and vm.log[-1][2][1] == 0.75 and 'Done' not in vm.self.vars
    vm.fire()
    assert vm.self.vars['Done'] == 1
    assert run(asset('LatentTest'), 'Plain', self_vars=vm.self.vars)[0] is None and vm.self.vars['Stage'] == 5
    print('ok  LatentTest: a UObject (LatentJob) waits and resumes too; a function with no latent call just runs')


def latent_links():
    """Every FLatentActionInfo names its class's ubergraph, targets Self, has a UUID no other latent call of the class
    shares, and a Linkage that starts a statement of that ubergraph."""
    for base in (asset('LatentTest'), os.path.join(os.path.dirname(asset('LatentTest')), 'LatentJob')):
        vm = VM(base)
        uber = next(e for e in vm.exports if e.startswith('ExecuteUbergraph_'))
        infos = []

        def walk(n):
            if n.op == 0x2F and n.val == 'LatentActionInfo': infos.append(n)
            for k in n.kids: walk(k)
        for fn in vm.exports:
            try: [walk(n) for n in vm.script(fn)[0]]
            except SystemExit: pass                      # the class and CDO exports have no script
        assert infos, base
        for link, uuid, execfn, target in (n.kids for n in infos):
            assert execfn.val == uber and target.op == 0x17 and link.val in vm.script(uber)[1], (base, link.val)
        assert len({n.kids[1].val for n in infos}) == len(infos), [n.kids[1].val for n in infos]
    print('ok  LatentTest: each latent call resumes a statement of its own ubergraph, under a UUID of its own')


def static_locals():
    """A static local lives in the ubergraph's frame, one per object: its initializer runs on the object's first call
    only, and one with none starts at the frame's zero, which a loop round it does not reset. Anywhere else it is
    refused, an inline function's too; a static constant needs no frame (LatentTest.Plain, in latent_runs)."""
    vm = VM(asset('LatentTest'), {'Delay': latent_call})
    for stage, want in ((4, 1502), (50, 1604)):          # Count = 4 + 10 once, then kept; Seen counts every round
        vm.self.vars['Stage'] = stage
        vm.call('Counted')
        vm.fire()
        assert vm.self.vars['Calls'] == want, (stage, vm.self.vars)
    other = vm.new(Stage=7)                               # another object: a frame, and statics, of its own
    vm.call('Counted', on=other)
    vm.fire()
    assert other.vars['Calls'] == 1802 and vm.self.vars['Calls'] == 1604, (other.vars, vm.self.vars)
    refused('StaticPlain', '  int32 N;\n  void F() { static int32 Count = N; ++Count; N = Count; }\n', 'lives in the ubergraph')
    refused('StaticInline', '  inline void Bump() { static int32 Count = 0; ++Count; }\n'
            '  void F() { Bump(); UKismetSystemLibrary::Delay(1.0f); }\n', 'each expansion would keep its own')
    print('ok  LatentTest.Counted: a static is initialized once per object and kept in its frame; refused outside one')


def await_runs():
    """AsyncTest with its proxies faked: what is bound to which dispatcher, when Activate runs, what a broadcast does."""
    made, activated = [], []
    def factory(cls):
        return lambda vm, ctx, *a: made.append(Obj(cls, args=a)) or made[-1]
    natives = {'CreateProxyObjectForPlayMontage': factory('PlayMontageCallbackProxy'),
               'DownloadImage': factory('AsyncTaskDownloadImage'),
               'Activate': lambda vm, ctx: activated.append((ctx, [p for o, p, f, _ in vm.binds if o is ctx]))}
    vm = VM(asset('AsyncTest'), natives, Mesh='mesh', Montage='montage', Last='None')
    bound = lambda obj: sorted((p, f) for o, p, f, b in vm.binds if o is obj and b is vm.self)
    vm.call('Play')                                       # callback style: both dispatchers call Done
    proxy = made[-1]
    assert proxy.vars['args'] == ('mesh', 'montage', 1.0, 0.0, 'None'), proxy.vars
    assert bound(proxy) == [('OnCompleted', 'Done'), ('OnInterrupted', 'Done')], vm.binds
    vm.broadcast(proxy, 'OnInterrupted', 'Cut')
    assert vm.self.vars['Last'] == 'Cut'
    vm.call('PlayThen')                                   # await style: nothing after the await runs yet
    proxy = made[-1]
    assert vm.self.vars['Last'] == 'Cut' and [p for p, f in bound(proxy)] == ['OnCompleted'], vm.binds
    vm.broadcast(proxy, 'OnCompleted', 'End')
    assert vm.self.vars['Last'] == 'End' and not activated     # a montage proxy is not an async action
    vm.call('Download', 'http://x')                       # an async action: activated once, OnSuccess already bound
    task = made[-1]
    assert task.vars['args'] == ('http://x',) and activated == [(task, ['OnSuccess'])] and 'Image' not in vm.self.vars
    vm.broadcast(task, 'OnSuccess', 'tex')
    assert vm.self.vars['Image'] == 'tex' and [p for p, f in bound(task)] == ['OnFail', 'OnSuccess'], vm.binds
    vm.broadcast(task, 'OnFail', None)
    assert vm.self.vars['Image'] is None and len(activated) == 1
    print('ok  AsyncTest: binds, one Activate after the first bind, each await resuming with its value')


def delegate_targets():
    """Every function a delegate is bound to by name exists in the class and takes the one parameter its dispatcher passes."""
    for mod in ('AsyncTest', 'LatentTest'):
        vm, names = VM(asset(mod)), set()

        def walk(n):
            if n.op == 0x4B: names.add(n.val)
            for k in n.kids: walk(k)
        for fn in vm.exports:
            try: [walk(n) for n in vm.script(fn)[0]]
            except SystemExit: pass
        assert names and all(f in vm.exports and len(vm.script(f)[2]) == 1 for f in names), (mod, names)
    # An inline method is no UFunction: binding one would name a function the class does not have.
    refused('DispInline', '  UE_DISPATCHER(OnHit, int32 Points);\n  int32 Got = 0;\n'
            '  inline void Handle(int32 Points) { Got += Points; }\n  void F() { OnHit.Add(this, &DispInline::Handle); }\n',
            'a delegate cannot bind Handle')
    print('ok  AsyncTest / LatentTest: every delegate bound by name is a one-parameter function of the class; not an inline one')


def repl_runs():
    """ReplTest as the server runs it: a replicated write wakes the object first, a RepNotify variable's OnRep runs right
    after the write (on the object written), an RPC called on the authority runs its body."""
    vm = VM(asset('ReplTest'), Score=5, Slots=[])
    vm.call('ReceiveBeginPlay')
    assert vm.self.vars == dict(Score=6, Slots=[4], bOpen=False, Local=1, Notified=12), vm.self.vars
    trace = [(l[0], l[2]) if l[0] == 'set' else (l[0],) for l in vm.log if l[1] is vm.self]
    assert trace == [('FlushNetDormancy',), ('set', 'bOpen'), ('set', 'Notified'),        # bOpen = true; OnRep_Open
                     ('FlushNetDormancy',), ('set', 'Slots'), ('set', 'Notified'),        # Slots[0] = 4; OnRep_Slots
                     ('set', 'Local'),
                     ('FlushNetDormancy',), ('set', 'bOpen'), ('set', 'Notified'),        # ServerOpen(false)
                     ('FlushNetDormancy',), ('set', 'Score')], trace                      # MultiBoom
    vm = VM(asset('ReplTest'))
    other = vm.self.vars['Other'] = vm.new(bOpen=True, Notified=0)
    vm.call('SetOther')
    assert other.vars == dict(bOpen=False, Notified=1, Local=2) and vm.self.vars['bReplicateMovement'] is True
    trace = [(l[0], l[1] is other, l[2] if l[0] == 'set' else None) for l in vm.log]
    assert trace == [('FlushNetDormancy', True, None), ('set', True, 'bOpen'), ('set', True, 'Notified'),
                     ('set', True, 'Local'), ('FlushNetDormancy', False, None),
                     ('set', False, 'bReplicateMovement')], trace       # a native RepNotify is for clients: no call
    for fn, parms, want in (('ClientPing', dict(Seq=42), dict(Local=42)), ('AuthOnly', {}, dict(Local=3)),
                            ('Pretty', {}, dict(Local=4)), ('OnRep_Open', {}, dict(Notified=1)),
                            ('OnRep_Slots', {}, dict(Notified=10))):
        vm = VM(asset('ReplTest'))
        vm.call(fn, **parms)
        assert vm.self.vars == want, (fn, vm.self.vars)
    vm = VM(asset('ReplTest'), Calls=0, Notified=0)
    vm.call('SetViaCall')
    assert vm.self.vars == dict(Calls=2, bOpen=True, Notified=1, Score=9), vm.self.vars
    assert run(asset('ReplTest'), 'ServerBump', Count=41)[1]['Count'] == 42
    kid = VM(os.path.join(os.path.dirname(asset('ReplTest')), 'ReplKid'))
    for fn, parms, seen in (('ServerOpen', dict(bValue=True), 7), ('ServerOpen', dict(bValue=False), 8),
                            ('MultiBoom', {}, 9), ('OnRep_Open', {}, 10)):
        kid.call(fn, **parms)
        assert kid.self.vars['Seen'] == seen, (fn, kid.self.vars)
    print('ok  ReplTest: wake before a replicated write, OnRep after it, RPC bodies; ReplKid overrides')


def rpc_routing():
    """A call to a net / authority-only / cosmetic function goes through CallFunction's callspace routing
    (EX_VirtualFunction / EX_FinalFunction): a Local* call (ProcessLocalFunction) would just run it here."""
    base = asset('ReplTest')
    vm, flags = VM(base), {}
    for i, e in enumerate(dumpexp.load(base)[5]):
        m = re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base, i))
        if m: flags[e['name']] = int(m.group(1), 16)
    routed = {f for f, v in flags.items() if v & (0x40 | 0x4 | 0x8)}      # FUNC_Net | AuthorityOnly | Cosmetic
    calls = []

    def walk(n):
        if n.op in (0x1B, 0x1C, 0x45, 0x46) and n.val in routed: calls.append((n.val, n.op))
        for k in n.kids: walk(k)
    for fn in flags:
        [walk(n) for n in vm.script(fn)[0]]
    assert sorted(f for f, _ in calls) == ['MultiBoom', 'ServerOpen'] and all(op in (0x1B, 0x1C) for _, op in calls), calls
    print('ok  ReplTest: RPC calls go through the net routing')


def spawn_runs():
    """SpawnTest with the engine faked: which classes are spawned / constructed / added, with what, in which order."""
    made = lambda k: (lambda vm, ctx, *a: Obj(a[k], args=a))
    natives = {'Conv_VectorToTransform': lambda vm, ctx, v: ('xf', tuple(v)),
               'BeginDeferredActorSpawnFromClass': made(1), 'SpawnObject': made(0), 'Create': made(1),
               'AddComponentByClass': made(0), 'FinishSpawningActor': lambda vm, ctx, a, xf: a,
               'K2_GetRootComponent': lambda vm, ctx: 'root'}
    vm = VM(asset('SpawnTest'), natives)
    vm.call('ReceiveBeginPlay')
    me, spawned = vm.self, vm.self.vars['Spawned']
    where, zero = ('xf', (0.0, 0.0, 100.0)), ('xf', (0.0, 0.0, 0.0))
    twin = next(l[2][0] for l in vm.log if l[0] == 'FinishSpawningActor' and l[2][0] is not spawned)
    late = next(l[2][0] for l in vm.log if l[0] == 'FinishAddComponent')
    assert [l for l in vm.log if l[0] not in ('Conv_VectorToTransform', 'K2_GetRootComponent')] == [
        ('BeginDeferredActorSpawnFromClass', me, [me, 'Actor', where, 0, me]), ('FinishSpawningActor', me, [spawned, where]),
        ('set', me, 'Spawned'),
        ('BeginDeferredActorSpawnFromClass', me, [me, 'SpawnTest_C', where, 0, None]),   # the mod class, deferred,
        ('set', twin, 'Tag'), ('FinishSpawningActor', me, [twin, where]),              # Tag set before it finishes
        ('SpawnObject', me, ['USpawnProbe_C', me]), ('set', me, 'Made'),
        ('Create', 'Default__WidgetBlueprintLibrary', [me, 'UserWidget', None]),       # cosmetic: on the library's CDO
        ('set', me, 'Widget'),
        ('AddComponentByClass', me, ['SceneComponent', False, zero, False]), ('set', me, 'Part'),
        ('K2_AttachToComponent', vm.self.vars['Part'], ['root', 'None', 2, 2, 2, True]),  # SnapToTarget, weld
        ('AddComponentByClass', me, ['SceneComponent', False, zero, True]),               # held back ...
        ('set', late, 'bHiddenInGame'), ('FinishAddComponent', me, [late, False, zero]),  # ... until set, then finished
        ('K2_AttachToActor', spawned, [me, 'None', 1, 1, 1, True])], vm.log              # KeepWorld
    assert twin.vars['Tag'] == 7 and late.vars['bHiddenInGame'] is True
    # The twin's own BeginPlay (FinishSpawning runs it) sees Tag = 7 and spawns nothing.
    twin.cls, log = me.cls, len(vm.log)
    vm.call('ReceiveBeginPlay', on=twin)
    assert len(vm.log) == log, vm.log[log:]
    assert vm.call('OwnClass') == 'SpawnTest_C'
    assert vm.call('OwnClassByMacro') == 'SpawnTest_C'
    probe = vm.call('MakeProbe')
    assert (probe.cls, probe.vars['args']) == ('USpawnProbe_C', ('USpawnProbe_C', me)), probe.vars
    print('ok  SpawnTest: spawn / construct / add-component calls, their classes and the deferred-set order')


def spawn_relative():
    """Compiled from its own folder as a bare "SpawnTest.cpp": `SpawnTest::StaticClass()` still names the mod class
    (the qualifier is read back from the mod's sources, which a bare path's empty parent once hid)."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        proc = assetgen_compile(['SpawnTest.cpp', UEAPI, tmp], cwd=TESTS)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        assert VM(os.path.join(tmp, 'SpawnTest'), {}).call('OwnClass') == 'SpawnTest_C'
    print('ok  SpawnTest: a bare relative source path finds X::StaticClass() qualifiers')


def outer_runs():
    """GetTypedOuter / GetOutermostTypedOuter over a faked outer chain: nearest / farthest outer of the kind, the
    object itself never a candidate, null when there is none; and no function of their own."""
    kinds = {'Pawn': {'Pawn', 'Actor'}}
    isa = lambda o, c: isinstance(o, Obj) and c in kinds.get(o.cls, {o.cls})
    pkg = Obj('Package'); outer_lvl = Obj('Level', Obj('World', pkg)); lvl = Obj('Level', outer_lvl)
    far = Obj('Actor', lvl); near = Obj('Pawn', far); comp = Obj('SceneComponent', near)
    vm = VM(asset('SpawnTest'), {'GetOuterObject': lambda vm, ctx, o: o.outer}, isa=isa)
    for fn, obj, want in (('OwningActor', comp, near), ('OwningActor', near, far), ('OwningActor', far, None),
                          ('OwningActor', None, None), ('LevelOf', comp, outer_lvl), ('LevelOf', outer_lvl, None)):
        assert vm.call(fn, obj) is want, (fn, obj, want)
    assert not {'GetTypedOuter', 'GetOutermostTypedOuter'} & vm.exports, vm.exports
    print('ok  SpawnTest: GetTypedOuter / GetOutermostTypedOuter walk the outers, inlined')


def uber_frames():
    """Every class with an ubergraph: the class names it (UberGraphFunction), carries the transient UberGraphFrame
    pointer the engine allocates the persistent frame through, and the function is FUNC_UbergraphFunction (0x8000) -
    the flag ProcessEvent / ProcessScriptFunction use to run it on that frame instead of a fresh one."""
    folder = os.path.dirname
    for base in (asset('LatentTest'), os.path.join(folder(asset('LatentTest')), 'LatentJob'), asset('AsyncTest')):
        names = [e['name'] for e in dumpexp.load(base)[5]]
        uber = next(i for i, n in enumerate(names) if n.startswith('ExecuteUbergraph_'))
        cls = dump('dumpstruct.py', base, 0)
        assert 'UberGraphFunction [0] ObjectProperty size=4: index %d' % (uber + 1) in cls, (base, cls)
        assert re.search(r"StructProperty UberGraphFrame .*flags=0x202000 .*PointerToUberGraphFrame", cls), cls
        assert int(re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base, uber)).group(1), 16) & 0x8000, base
    print('ok  LatentTest / AsyncTest: each ubergraph is the class\'s UberGraphFunction and runs on the persistent frame')


def repl_defaults():
    """The actor replicates (CDO bReplicates, or no replicated variable ever leaves the server) and keeps Score's
    initializer; ReplKid inherits both rather than overriding them."""
    folder = os.path.dirname(asset('ReplTest'))
    for name, own in (('ReplTest', True), ('ReplKid', False)):
        base = os.path.join(folder, name)
        names = [e['name'] for e in dumpexp.load(base)[5]]
        cdo = dump('dumptags.py', base, names.index('Default__%s_C' % name))
        if own: assert re.search(r'bReplicates \[0\] BoolProperty size=0 value=1', cdo) and 'Score [0] IntProperty size=4: 5' in cdo, cdo
        else: assert 'bReplicates' not in cdo and 'Score' not in cdo, cdo
    print('ok  ReplTest: the CDO replicates and holds Score = 5; ReplKid inherits them')


replication()
repl_defaults()
repl_runs()
rpc_routing()
uber_frames()
latent_flags()
latent_runs()
latent_links()
static_locals()
await_runs()
delegate_targets()
spawn_runs()
spawn_relative()
outer_runs()


# ---- PRELOAD: the event-driven loader's preload dependencies (invariant_rules/preload.py)
#
# A cooked package tells the event-driven loader, per export, what must be created or serialized before the export is
# created or serialized (AsyncLoading.cpp 2447-2499). The edl_* rules read that graph and ask what the loader fetches
# with bCheckSerialized, and what a payload names, is ordered before it: they hold on every package of the game's own
# content. Each mod below carries a kind of dependency, and its test asserts the order the engine needs; a pending one
# is a kind AssetGen does not order yet.

import invariants
from invariant_rules import preload as EDL

EDL_RULES = ('edl_preload_arcs', 'edl_create_prereqs', 'edl_super_serialized', 'edl_class_cdo', 'edl_class_closure',
             'edl_parent_subobjects_serialized', 'edl_property_types', 'edl_payload_created', 'edl_cdo_components')


def mod_packages(base):
    """Every package a mod's compile wrote: the Content folder its class sits under, walked."""
    return invariants.packages([base.replace(os.sep, '/').split('/Content/')[0] + '/Content'])


def keeps_edl(base, names=EDL_RULES):
    """Every package of base's mod keeps the named edl_* rules."""
    found = [(os.path.basename(b),) + f for b in mod_packages(base) for f in invariants.check(invariants.Package(b), set(names))]
    assert not found, '; '.join('%s %s %s: %s' % f for f in found[:3])


def preload_chain():
    """PreloadChain_C <- PreloadMid_C <- PreloadRoot_C <- AActor, one package each. Each class has exactly one CDO,
    whose template is its parent's CDO; the parent class and the parent CDO are serialized before the class, and the
    class before its CDO is created; every export's class, template and outer are ready before it is created; the
    graph has no cycle. And the CDO holds the class's own defaults, on top of what it inherits."""
    keeps_edl(asset('PreloadChain'))
    folder = os.path.dirname(asset('PreloadChain'))
    for name, parent in (('PreloadChain', '/Game/_ElytrasMods/PreloadChain/PreloadMid.Default__PreloadMid_C'),
                         ('PreloadMid', '/Game/_ElytrasMods/PreloadChain/PreloadRoot.Default__PreloadRoot_C'),
                         ('PreloadRoot', '/Script/Engine.Default__Actor')):
        pkg = invariants.Package(os.path.join(folder, name))
        st = pkg.struct(pkg.find(name + '_C'))
        cdo = pkg.exports[st.cdo - 1]
        assert pkg.path(cdo['tmpl']) == parent, (name, pkg.path(cdo['tmpl']))
    cdo = dump('dumptags.py', asset('PreloadChain'), exports_of(asset('PreloadChain')).index('Default__PreloadChain_C'))
    assert 'Level [0] IntProperty size=4: 3' in cdo and 'Weight [0] FloatProperty size=4: 4.0' in cdo, cdo
    print('ok  PreloadChain: one CDO per class, templated on the parent CDO; parent class and CDO serialized before '
          'the class, the class before its CDO is created; no cycle')


preload_chain()


def preload_refs():
    """PreloadRefs' payloads name UPreloadProbe_C as NewObject's class constant (Make) and as Probe's property class,
    OnPing's signature as Broadcast's target (Ping), and the engine cylinder as Mesh's template default. Each is created
    before the export naming it is serialized, or it loads as null: the writer lists every object a payload names, as
    the cook's DependsMap does. First, that each payload does name what the source says it does."""
    base = asset('PreloadRefs')
    pkg = invariants.Package(base)

    def named(export):
        return {pkg.path(r) for _, r in EDL.edl_payload_refs(pkg, pkg.find(export)) if EDL.edl_in_range(pkg, r)}
    probe = '/Game/_ElytrasMods/PreloadRefs/UPreloadProbe.UPreloadProbe_C'
    for export, want in (('Make', probe), ('PreloadRefs_C', probe),
                         ('Ping', '/Game/_ElytrasMods/PreloadRefs/PreloadRefs.PreloadRefs_C:OnPing__DelegateSignature'),
                         ('Mesh_GEN_VARIABLE', '/Engine/BasicShapes/Cylinder.Cylinder')):
        assert want in named(export), '%s does not name %s: %s' % (export, want, sorted(named(export)))
    keeps_edl(base, ['edl_payload_created'])


preload_refs()
print("ok  PreloadRefs: a class constant, a property's class, a Broadcast target and a template default are created first")


def preload_override():
    """PreloadOverride's Step and Score override PreloadBase_C's: their SuperStruct is that function, and the loader
    fetches it with bCheckSerialized while it serializes the override - a Fatal 'Missing Dependency' when the parent
    package loads in the same batch and has not serialized it yet."""
    base = asset('PreloadOverride')
    pkg = invariants.Package(base)
    supers = {e['name']: pkg.path(e['super']) for k, e in enumerate(pkg.exports)
              if pkg.class_of(k + 1) == 'Function' and e['super']}
    assert supers == {'Step': '/Game/_ElytrasMods/PreloadOverride/PreloadBase.PreloadBase_C:Step',
                      'Score': '/Game/_ElytrasMods/PreloadOverride/PreloadBase.PreloadBase_C:Score'}, supers
    keeps_edl(base, ['edl_super_serialized'])


preload_override()
print('ok  PreloadOverride: an override is serialized after its Blueprint parent function, its SuperStruct')


def preload_types():
    """PreloadTypes' structs and enum type a class variable, an array element, a parameter, a local and struct
    members: each owner is serialized after the type it links against."""
    base = asset('PreloadTypes')
    keeps_edl(base, ['edl_property_types'])


preload_types()
print('ok  PreloadTypes: a user-defined struct or enum is serialized before the class, function or struct it types')


def preload_uber_class():
    """PreloadUber_C names ExecuteUbergraph_PreloadUber as its UberGraphFunction: Link preloads it, and the CDO's
    persistent frame is only made from a loaded one, so it is serialized before the class."""
    base = asset('PreloadUber')
    pkg = invariants.Package(base)
    assert EDL.edl_obj_tag(pkg, pkg.find('PreloadUber_C'), 'UberGraphFunction') > 0
    keeps_edl(base, ['edl_class_closure'])


preload_uber_class()
print('ok  PreloadUber: the class is serialized after its ubergraph function')


def preload_uber_calls():
    """Wait, the stub the latent call leaves, calls ExecuteUbergraph_PreloadUber and writes its frame: resolved while
    Wait is serialized, so the ubergraph is created before; otherwise the call target reads back null."""
    base = asset('PreloadUber')
    pkg = invariants.Package(base)
    wait = pkg.find('Wait')
    assert any(op[0] == 'obj' and op[1] and pkg.path(op[1]).endswith(':ExecuteUbergraph_PreloadUber')
               for t in pkg.script(wait) for n in t.walk() for op in n.ops), 'Wait does not call ExecuteUbergraph_PreloadUber'
    keeps_edl(base, ['edl_payload_created'])


preload_uber_calls()
print('ok  PreloadUber: a function that enters the ubergraph has it created before it is serialized')


def preload_ich():
    """PreloadIch_C names its InheritableComponentHandler, through which the Lamp archetype is found: the handler is
    serialized (so created) before the class."""
    base = asset('PreloadIch')
    pkg = invariants.Package(base)
    assert EDL.edl_obj_tag(pkg, pkg.find('PreloadIch_C'), 'InheritableComponentHandler') > 0
    keeps_edl(base, ['edl_class_closure', 'edl_payload_created'])


preload_ich()
print('ok  PreloadIch: the class is serialized after its InheritableComponentHandler')


# PreloadDso: a native default subobject restated along a Blueprint chain, and a child that restates nothing.

def preload_dso_template(name, want):
    """The export restating CollisionCylinder under Default__<name>_C, and the package: its TemplateIndex must be
    `want`, the subobject of that name under the parent CDO (GetArchetypeFromRequiredInfo, UObjectArchetype.cpp
    64-87)."""
    pkg = invariants.Package(os.path.join(os.path.dirname(asset('PreloadDso')), name))
    k = next((k for k, e in enumerate(pkg.exports) if e['name'] == 'CollisionCylinder'
              and e['outer'] == pkg.find('Default__%s_C' % name) + 1), None)
    assert k is not None, '%s: no CollisionCylinder export under its CDO' % name
    tmpl = pkg.exports[k]['tmpl']
    assert tmpl and pkg.path(tmpl) == want, '%s: TemplateIndex %s, not %s' % (name, tmpl and pkg.path(tmpl), want)
    return pkg, k


def preload_dso():
    """PreloadDsoBase restates CollisionCylinder, a default subobject ACharacter's constructor makes: the export's
    TemplateIndex is its archetype, Default__Character's subobject of that name; the loader checks it is set and
    fetches it with bCheckSerialized."""
    pkg, k = preload_dso_template('PreloadDsoBase', '/Script/Engine.Default__Character:CollisionCylinder')
    found = invariants.check(pkg, {'edl_create_prereqs'})
    assert not found, '; '.join('%s %s: %s' % f for f in found[:3])
    print('ok  PreloadDso: a restated native subobject names Default__Character:CollisionCylinder as its TemplateIndex')


def preload_dso_chain():
    """PreloadDso restates it again: its override's archetype is PreloadDsoBase's, an import from the parent's
    package. That export is serialized before the override is created, and before PreloadDso_C is serialized: the CDO
    the class makes then builds its capsule from it, and a capsule copied from an unloaded archetype keeps
    ACharacter's half height instead of the parent's 120."""
    preload_dso_template('PreloadDso', '/Game/_ElytrasMods/PreloadDso/PreloadDsoBase.Default__PreloadDsoBase_C:CollisionCylinder')
    keeps_edl(asset('PreloadDso'), ['edl_create_prereqs', 'edl_class_closure', 'edl_parent_subobjects_serialized'])


def preload_dso_kid():
    """PreloadDsoKid restates nothing, so whether it exports a capsule of its own is the writer's choice; either way the
    CDO its class makes while it is serialized copies its capsule from PreloadDsoBase's CollisionCylinder export
    (UObjectGlobals.cpp 3844-3859). That export is in PreloadDsoKid's linker table and serialized before the class."""
    kid = os.path.join(os.path.dirname(asset('PreloadDso')), 'PreloadDsoKid')
    parent = invariants.Package(os.path.join(os.path.dirname(kid), 'PreloadDsoBase'))
    assert any(e['name'] == 'CollisionCylinder' and e['outer'] == parent.find('Default__PreloadDsoBase_C') + 1
               for e in parent.exports), 'PreloadDsoBase exports no CollisionCylinder under its CDO'
    found = invariants.check(invariants.Package(kid), {'edl_parent_subobjects_serialized'})
    assert not found, '; '.join('%s %s: %s' % f for f in found[:3])


preload_dso()
preload_dso_chain()
print('ok  PreloadDso: a child\'s subobject override is archetyped on the parent\'s, serialized before it and the child class')
preload_dso_kid()
print('ok  PreloadDso: a child class is serialized after every default subobject its Blueprint parent\'s CDO exports')


def preload_game_parent():
    """PreloadGameParent's parent is the game's ENE_Spider_Grunt_Normal_C, whose package exports every default
    subobject of its CDO: the class is serialized after each of them, as after a parent cooked in the same compile
    (preload_dso_kid). edl_parent_subobjects_serialized reads them off the game's package, so this needs --game."""
    if not GAME:
        print('--  PreloadGameParent: skipped (needs --game: the parent CDO\'s subobjects are read off the game\'s package)')
        return False
    base = asset('PreloadGameParent')
    saved = list(invariants.GAME_CONTENT)
    invariants.GAME_CONTENT[:] = [GAME]
    try:
        found = invariants.check(invariants.Package(base), {'edl_parent_subobjects_serialized'})
    finally:
        invariants.GAME_CONTENT[:] = saved
    assert not found, '%d findings, e.g. %s' % (len(found), '; '.join('%s %s: %s' % f for f in found[:2]))
    return True


if preload_game_parent():
    print('ok  PreloadGameParent: a child of a game Blueprint is serialized after every default subobject its parent\'s '
          'CDO exports')


def preload_case_kid():
    """PreloadCaseKid restates the grunt's Temperature by the name UeApi gives it, temperature (the object dump's
    spelling). FName compares without case, so that is one object: one import row, which is both the override's
    archetype and a parent subobject the class is serialized after (import_unique, edl_parent_subobjects_serialized)."""
    if not GAME:
        print('--  PreloadCaseKid: skipped (needs --game: the parent CDO\'s subobjects are read off the game\'s package)')
        return False
    base = asset('PreloadCaseKid')
    saved = list(invariants.GAME_CONTENT)
    invariants.GAME_CONTENT[:] = [GAME]
    try:
        found = invariants.check(invariants.Package(base), {'import_unique', 'edl_parent_subobjects_serialized'})
    finally:
        invariants.GAME_CONTENT[:] = saved
    assert not found, '; '.join('%s %s: %s' % f for f in found[:3])
    return True


if preload_case_kid():
    print('ok  PreloadCaseKid: a restated subobject spelled in another case than the parent\'s export is one import')


def preload_nested_kid():
    """PreloadNestedKid's parent, the game's WPN_Pickaxe_C, exports an instanced bonus under each of two default
    subobjects (Damage:BreakIceBonus_0): the class is serialized after those too, as after every default subobject,
    since the CDO it makes then copies each from them (edl_parent_subobjects_serialized walks every depth)."""
    if not GAME:
        print('--  PreloadNestedKid: skipped (needs --game: the parent CDO\'s subobjects are read off the game\'s package)')
        return False
    base = asset('PreloadNestedKid')
    saved = list(invariants.GAME_CONTENT)
    invariants.GAME_CONTENT[:] = [GAME]
    try:
        found = invariants.check(invariants.Package(base), {'edl_parent_subobjects_serialized', 'import_unique'})
    finally:
        invariants.GAME_CONTENT[:] = saved
    assert not found, '%d findings, e.g. %s' % (len(found), '; '.join('%s %s: %s' % f for f in found[:2]))
    keeps_invariants(base)
    return True


if preload_nested_kid():
    print('ok  PreloadNestedKid: a child of a game Blueprint is serialized after its parent CDO\'s nested subobjects too')


def ueapi_too_old():
    """A UeApi that a genueapi older than the compiler wrote is refused, saying to regenerate it, before the compile
    reads any of it: one made before UeDefaultSubobjects compiles a game Blueprint's child with none of the ordering
    edges above, and says nothing. Here a UeApi with no Version.json (any made before genueapi stamped one) and one
    stamped 0."""
    import tempfile
    src = os.path.join(TESTS, 'PreloadCaseKid.cpp')
    for stamp in (None, '{"genueapi": 0}\n'):
        with tempfile.TemporaryDirectory() as tmp:
            stale = os.path.join(tmp, 'UeApi')
            os.makedirs(stale)
            if stamp:
                with open(os.path.join(stale, 'Version.json'), 'w', encoding='utf-8') as f: f.write(stamp)
            proc = assetgen_compile([src, stale, os.path.join(tmp, 'out')])
            assert proc.returncode != 0 and 'older genueapi' in proc.stdout and 'regenerate' in proc.stdout, \
                (stamp, proc.stdout[-500:])


ueapi_too_old()
print('ok  a UeApi older than the compiler, or with no Version.json, is refused, saying to regenerate it')


def ueapi_without_game():
    """genueapi without --game writes no game Blueprint's UeDefaultSubobjects or UeClassTail, which reads as a Blueprint
    with none: a class deriving from one would compile without a word and load before its parent's subobjects. Its
    Version.json says "game": false, and such a class is refused, saying to regenerate with --game; one with a native
    parent still compiles. Here this UeApi's own headers, included by their full path, under tables whose Version.json
    says false."""
    import json, shutil, tempfile
    real = os.path.abspath(UEAPI).replace('\\', '/')
    with tempfile.TemporaryDirectory() as tmp:
        api = os.path.join(tmp, 'UeApi')
        os.makedirs(api)
        for f in glob.glob(os.path.join(UEAPI, '*.json')):
            shutil.copy(f, api)
        stamp = json.load(open(os.path.join(UEAPI, 'Version.json'), encoding='utf-8'))
        stamp['game'] = False
        json.dump(stamp, open(os.path.join(api, 'Version.json'), 'w', encoding='utf-8'))
        for mod, header, parent, ok in (('NoGameKid', 'Game/ENE_Spider_Grunt_Normal_C.h', 'ENE_Spider_Grunt_Normal_C', False),
                                        ('NoGameActor', 'Engine.h', 'AActor', True)):
            src = os.path.join(tmp, mod + '.cpp')
            with open(src, 'w', encoding='utf-8') as f:
                f.write('#include "%s/UeMeta.h"\n#include "%s/%s"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/%s");\n'
                        'class %s : public %s {\npublic:\n    int32 Count;\n};\n' % (real, real, header, mod, mod, parent))
            proc = assetgen_compile([src, api, os.path.join(tmp, 'out', mod)])
            if ok:
                assert proc.returncode == 0, (mod, proc.stdout[-500:])
            else:
                assert proc.returncode != 0 and 'without --game' in proc.stdout and 'regenerate' in proc.stdout, \
                    (mod, proc.stdout[-500:])


ueapi_without_game()
print('ok  a UeApi made without --game refuses a class deriving from a game Blueprint, saying to regenerate it with '
      '--game; a native parent still compiles')


def subobject_bomber():
    """SubobjectBomber restates two of ABomber's own members that two default subobjects each fit, neither named for
    the member: GooSoundComponent is GooAudioComponent, AcidEmitterLeft is GooEmitterLeft, as the game's ENE_Bomber_C
    default object says. Each override is an export of the subobject's name under the class's CDO, archetyped on
    Default__Bomber's subobject, and the CDO's tag of the member names it."""
    import struct
    base = asset('SubobjectBomber')
    pkg = invariants.Package(base)
    cdo = pkg.find('Default__SubobjectBomber_C')
    for member, sub, value in (('GooSoundComponent', 'GooAudioComponent', ('VolumeMultiplier', 0.5)),
                               ('AcidEmitterLeft', 'GooEmitterLeft', ('SecondsBeforeInactive', 2.0))):
        k = next((k for k, e in enumerate(pkg.exports) if e['name'] == sub and e['outer'] == cdo + 1), None)
        assert k is not None, 'no %s export under the CDO for %s' % (sub, member)
        assert pkg.path(pkg.exports[k]['tmpl']) == '/Script/FSD.Default__Bomber:' + sub, pkg.path(pkg.exports[k]['tmpl'])
        t = pkg.tag(cdo, member)
        assert t and struct.unpack_from('<i', t['value'])[0] == k + 1, (member, t)
        t = pkg.tag(k, value[0])
        assert t and struct.unpack_from('<f', t['value'])[0] == value[1], (sub, value[0], t)
    keeps_invariants(base)


subobject_bomber()
print('ok  SubobjectBomber: a member two subobjects fit, neither named for it, overrides the one the game\'s Blueprint '
      'names')


# ---- TABLES: the package's own tables - names and their numbers, imports, exports, archetypes
# (invariant_rules/tables.py)

import struct, tempfile, glob as _glob
import invariants
from invariant_rules import tables          # raw_tables, name_refs, parse_tables


def name_pairs(base):
    """{(export name or None, where): {(name-map entry, Number)}} of every FName reference in the package at base, as
    the loader resolves them (tables.name_refs): 'struct' is a struct body, 'tags' an export's tags, 'script' bytecode."""
    pkg = invariants.Package(base)
    names, out = tables.raw_tables(pkg).names, {}
    for k, where, i, n in tables.name_refs(pkg):
        out.setdefault((pkg.exports[k]['name'] if k is not None else None, where), set()).add((names[i], n))
    return out, names


def name_numbers():
    """NameNumberTest: a name ending in _digits is stored as FName makes it - the entry without the suffix, the number
    one above it - wherever ParseNumber splits it (a lone zero, nine digits), and whole where it does not (a leading
    zero, MAX_int32): members, their CDO tags, the body that reads them, and FName literals alike."""
    base = asset('NameNumberTest')
    refs, names = name_pairs(base)
    members = {('Count', 8), ('Level', 1), ('Rocket_04', 0), ('Width', 123456790)}
    for where in (('NameNumberTest_C', 'struct'), ('Default__NameNumberTest_C', 'tags'), ('Sum', 'script')):
        assert members <= refs[where], (where, sorted(refs[where]))
    for fn, want in (('Tag', ('Tag', 8)), ('Socket', ('Socket_01', 0)), ('Nine', ('Nine', 123456790)),
                     ('Cap', ('Cap_2147483647', 0))):
        assert want in refs[fn, 'script'], (fn, sorted(refs[fn, 'script']))
    whole = [n for n in ('Count_7', 'Level_0', 'Width_123456789', 'Tag_7', 'Nine_123456789') if n in names]
    assert not whole, 'stored whole: %s' % whole
    fields = {'Count_7': 1, 'Level_0': 2, 'Rocket_04': 3, 'Width_123456789': 4}
    assert run(base, 'Sum', self_vars=fields)[0] == 10
    keeps_invariants(base)
    print('ok  NameNumberTest: a _N suffix is stored as FName splits it (entry, N+1), and whole where FName keeps it whole')


def name_suffix():
    """The ten-digit case of the same rule: below MAX_int32, FName splits it too."""
    base = asset('NameSuffix')
    refs, names = name_pairs(base)
    whole = [n for n in ('Tag_1234567890', 'Count_1234567890') if n in names]
    assert not whole, 'stored whole, Number 0: %s' % whole
    assert ('Count', 1234567891) in refs['NameSuffix_C', 'struct'] & refs['Default__NameSuffix_C', 'tags']
    assert ('Tag', 1234567891) in refs['Tag', 'script'] and ('Tag', 1234567891) in refs['Same', 'script']
    keeps_invariants(base)
    print('ok  NameSuffix: a 10-digit _N suffix below MAX_int32 is stored split, as FName splits it')


def subobject_template():
    """OverrideTest's Walker overrides ACharacter's default subobjects: each override's archetype is the parent CDO's
    subobject of that name (GetArchetypeFromRequiredInfo rule 1), which the loader constructs it on."""
    b = os.path.join(os.path.dirname(asset('OverrideTest')), 'Walker')
    ex, names = dumpexp.load(b)[5], exports_of(b)
    imps = dict(import_paths(b, classes=True))
    for sub, cls in (('CollisionCylinder', '/Script/Engine.CapsuleComponent'),
                     ('CharacterMesh0', '/Script/Engine.SkeletalMeshComponent')):
        e = ex[names.index(sub)]
        got = ref(b, e['tmpl']) if e['tmpl'] else 'a null template'
        assert got == '/Script/Engine.Default__Character:' + sub, (sub, got)
        assert imps['/Script/Engine.Default__Character:' + sub] == cls, imps
    assert imps['/Script/Engine.Default__Character'] == '/Script/Engine.Character', imps
    keeps_invariants(b)
    print("ok  OverrideTest Walker: each default-subobject override names Default__Character's subobject as its archetype")


def subobject_chain():
    """SubobjectChain: the base's override stands on Default__Character's CollisionCylinder, the kid's on the base's
    own override - a /Game object, so the kid's export also waits for it to be serialized before it is created. The
    middle class restates nothing, yet exports the capsule the tip's override stands on."""
    kid = asset('SubobjectChain')
    base, mid, tip = (os.path.join(os.path.dirname(kid), 'SubobjectChain' + c) for c in ('Base', 'Mid', 'Tip'))
    for b, want in ((base, '/Script/Engine.Default__Character:CollisionCylinder'),
                    (kid, '/Game/_ElytrasMods/SubobjectChain/SubobjectChainBase.Default__SubobjectChainBase_C:CollisionCylinder'),
                    (mid, '/Script/Engine.Default__Character:CollisionCylinder'),
                    (tip, '/Game/_ElytrasMods/SubobjectChain/SubobjectChainMid.Default__SubobjectChainMid_C:CollisionCylinder')):
        e = dumpexp.load(b)[5][exports_of(b).index('CollisionCylinder')]
        got = ref(b, e['tmpl']) if e['tmpl'] else 'a null template'
        assert got == want, (os.path.basename(b), got)
        assert dict(import_paths(b, classes=True))[want] == '/Script/Engine.CapsuleComponent'
    for b in (kid, tip):
        k = exports_of(b).index('CollisionCylinder')
        assert dumpexp.load(b)[5][k]['tmpl'] in dumpexp.preload(b)[k][2], 'the template is not serialized before create'
    for b in (base, kid, mid, tip):
        keeps_invariants(b)
    print("ok  SubobjectChain: an override's archetype is the parent CDO's subobject of its name, a /Game one serialized "
          "before create; a parent that leaves it alone exports it for its subclass")


def self_ref_import():
    """SelfRefImport: a member of the mod's own class and a call to its own function on another instance reference
    this package's exports, never an import of the package itself."""
    b = asset('SelfRefImport')
    own = '/game/_elytrasmods/selfrefimport/selfrefimport'
    selfs = [p for p in import_paths(b) if p.lower() == own or p.lower().startswith(own + '.')]
    assert not selfs, 'imports of itself: %s' % selfs
    pkg = invariants.Package(b)
    peer = next(p for p in pkg.struct(pkg.find('SelfRefImport_C')).props if p.name == 'Peer')
    assert peer.ref > 0 and pkg.exports[peer.ref - 1]['name'] == 'SelfRefImport_C', pkg.path(peer.ref)
    keeps_invariants(b)
    print('ok  SelfRefImport: its own class and function are named as its exports, never through an import of itself')


ABSTRACT_CLASS = 'class UPureDef : public UPrimaryDataAsset {\npublic:\n  virtual int32 Pure() = 0;\n};\n'
ABSTRACT_COMP = 'class UPureComp : public UActorComponent {\npublic:\n  virtual int32 Pure() = 0;\n};\n'


def refused_naming(mod, body, pattern, top=''):
    """refused(), where the reason may come from clang (its diagnostics go to stderr, the compiler's FAILED line to
    stdout) or from the compiler itself: the compile fails and stdout or stderr matches the regex `pattern`."""
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, mod + '.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/%s");\n%s'
                    'class %s : public AActor {\npublic:\n%s};\n' % (mod, top, mod, body))
        proc = assetgen_compile([src, UEAPI, tmp])
        assert proc.returncode != 0 and re.search(pattern, proc.stdout + proc.stderr), (mod, proc.stdout, proc.stderr)


def abstract_instances():
    """A class left abstract is never instanced: the loader constructs every non-CDO export, and StaticAllocateObject
    check()s !CLASS_Abstract (UObjectGlobals.cpp 2362). An asset of an abstract mod class - one this mod would cook,
    or one it names at a path - is refused, naming the class abstract (today clang does: a variable of an abstract
    type)."""
    abstract = r"'UPureDef' is an abstract class|UPureDef\W.*\babstract\b"
    refused_naming('AbstractAsset', '  UPureDef *Picked = &PureData;\n', abstract,
                   top=ABSTRACT_CLASS + 'UPureDef PureData = {};\n')
    refused_naming('AbstractAsset', '  UPureDef *Picked = &PureData;\n', abstract,
                   top=ABSTRACT_CLASS + 'UE_ASSET_AT(UPureDef, PureData, "/Game/_ElytrasMods/AbstractAsset/PureData");\n')
    print('ok  AbstractAsset: an asset of an abstract mod class, cooked here or named at a path, is refused as abstract')


def name_too_long():
    """An FName literal of 1100 characters has no cooked form: the loader reads a name-map entry into a NAME_SIZE
    buffer, NUL included (NameTypes.h 36), and a longer one misreads every later name (UnrealNames.cpp 2657-2672). It
    is refused, saying it is too long; 1023 characters, the most an FName holds, still cooks."""
    body = '  bool F() { return FName("%s") == FName("A"); }\n'
    refused('NameTooLong', body % ('x' * 1100), 'is too long: 1100 characters')
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'NameAtLimit.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/NameAtLimit");\n'
                    'class NameAtLimit : public AActor {\npublic:\n%s};\n' % (body % ('x' * 1023)))
        proc = assetgen_compile([src, UEAPI, tmp])
        assert proc.returncode == 0, proc.stdout + proc.stderr
    print('ok  NameTooLong: an FName over 1023 characters is refused as too long; one of 1023 cooks')


def mutated(tmp, base, patch):
    """A copy of base's package folder in a fresh Content tree under tmp, with base's own files patched in memory:
    patch(ua, ue, t, pkg) edits the bytearrays of its .uasset / .uexp (t its raw tables, pkg the Package). The copy keeps
    its path below FSD/Content, so its package name and its /Game imports of the folder's other packages still hold."""
    tmp = tempfile.mkdtemp(dir=tmp)
    rel = base.replace(os.sep, '/').split('/FSD/Content/')[-1]
    out = os.path.join(tmp, 'Mut', 'FSD', 'Content', *rel.split('/'))
    os.makedirs(os.path.dirname(out))
    for f in _glob.glob(os.path.join(os.path.dirname(base), '*.u*')):
        with open(f, 'rb') as src, open(os.path.join(os.path.dirname(out), os.path.basename(f)), 'wb') as dst:
            dst.write(src.read())
    ua, ue = bytearray(open(base + '.uasset', 'rb').read()), bytearray(open(base + '.uexp', 'rb').read())
    pkg = invariants.Package(base)
    patch(ua, ue, tables.raw_tables(pkg), pkg)
    open(out + '.uasset', 'wb').write(ua)
    open(out + '.uexp', 'wb').write(ue)
    return out


def tables_rules_fire():
    """Each TABLES rule sees the thing it checks: one package, clean as built, patched to break exactly that invariant,
    and the rule reports it."""
    nn = asset('NameNumberTest')
    dp = os.path.join(os.path.dirname(asset('OverrideTest')), 'DimProp')
    mood = os.path.join(os.path.dirname(asset('AssetTest')), 'UMoodDef')       # MD_Big beside it is an instance of it
    ct = asset('CompTest')                                                      # its Rocks / Grass are (H)ISMC templates
    exp_at = lambda t, k, field: t.export_offset + 104 * k + {'cls': 0, 'super': 4, 'tmpl': 8, 'outer': 12, 'name': 16,
                                                              'flags': 24, 'size': 28, 'forced': 44}[field]
    imp_at = lambda t, j, field: t.import_offset + 28 * j + {'cn': 8, 'outer': 16, 'obj': 20, 'num': 24}[field]
    put = lambda buf, at, v: struct.pack_into('<i', buf, at, v)
    get = lambda buf, at: struct.unpack_from('<i', buf, at)[0]
    idx = lambda pkg, name: pkg.find(name)

    def rename(t, old, new):
        """Rewrites name entry `old` in place as `new`, of the same length."""
        j = t.names.index(old)
        return lambda ua: ua.__setitem__(slice(t.names_at[j] + 4, t.names_at[j] + 4 + len(new)), new.encode())

    def super_payload(ua, ue, t, pkg, k, v):
        """Sets struct export k's serialized SuperStruct (after its tags and lazy-object GUID bool) to v."""
        at = pkg.tags(k).end
        at += 4 + (16 if get(pkg.blob(k), at) else 0)
        put(ue, pkg.exports[k]['off'] - pkg.total + at, v)

    def first_nonroot(t):
        return next(j for j, m in enumerate(t.imports) if m['outer'])

    def import_of(t, name):
        return next(j for j, m in enumerate(t.imports) if t.name(m['obj']) == name)

    def within_of_itself(ua, ue, t, p):
        """Sets class UMoodDef_C's ClassWithin (in its tail: ClassWithin, config name, ClassGeneratedBy, interfaces,
        bDeprecatedForceScriptOrder, the dummy name, bCooked, the CDO) to the class itself."""
        k = idx(p, 'UMoodDef_C')
        st = p.struct(k)
        put(ue, p.exports[k]['off'] - p.total + st.end - 40 - 12 * len(st.interfaces), k + 1)

    def archetype_twin(ua, ue, t, p):
        """Makes the CDO the archetype of the class, and puts a DefaultSceneRoot_GEN_VARIABLE of the same class under
        the CDO (the Tag function, renamed and reclassed): rule 1 now finds that one for the class's own
        DefaultSceneRoot_GEN_VARIABLE, whose template is still the SceneComponent CDO."""
        dsr, tag = idx(p, 'DefaultSceneRoot_GEN_VARIABLE'), idx(p, 'Tag')
        put(ua, exp_at(t, 0, 'tmpl'), 2)
        put(ua, exp_at(t, tag, 'outer'), 2)
        put(ua, exp_at(t, tag, 'cls'), t.exports[dsr]['cls'])
        struct.pack_into('<ii', ua, exp_at(t, tag, 'name'), *t.exports[dsr]['obj'])

    cases = [
        ('table_bounds', nn, lambda ua, ue, t, p: put(ua, exp_at(t, 2, 'outer'), 999), 'outer 999 is outside'),
        ('table_bounds', nn, lambda ua, ue, t, p: ua.__setitem__(t.names_at[t.names.index('Rocket_04')] + 4 + len('Rocket_04'), ord('!')),
         'does not end in its only NUL'),
        ('table_bounds', nn, lambda ua, ue, t, p: put(ue, p.exports[1]['off'] - p.total, 99999), 'FName (99999,'),
        ('names_split', nn, lambda ua, ue, t, p: rename(t, 'Rocket_04', 'Rocket_40')(ua), "'Rocket_40' #0"),
        ('import_chains', nn, lambda ua, ue, t, p: put(ua, imp_at(t, first_nonroot(t), 'outer'), 1), 'reaches export 0'),
        ('imports_resolve', dp, lambda ua, ue, t, p: put(ua, imp_at(t, import_of(t, 'BaseProp_C'), 'num'), 5),
         'has no export BaseProp_C_4'),
        ('imports_resolve', dp, lambda ua, ue, t, p: put(ua, imp_at(t, import_of(t, 'BaseProp_C'), 'cn'),
                                                        t.names.index('Package')), 'is a /Script/Engine.Package'),
        ('imports_resolve', dp, lambda ua, ue, t, p: rename(t, '/Game/_ElytrasMods/OverrideTest/BaseProp',
                                                           '/Game/_ElytrasMods/OverrideTest/BaseProq')(ua), 'no such package'),
        ('export_rows', nn, lambda ua, ue, t, p: put(ua, exp_at(t, 2, 'forced'), 1), 'bForcedExport'),
        ('export_rows', nn, lambda ua, ue, t, p: put(ua, exp_at(t, 2, 'flags'), p.exports[2]['flags'] | 0x10),
         'RF_ClassDefaultObject set'),
        ('export_name_unique', nn, lambda ua, ue, t, p: struct.pack_into('<ii', ua, exp_at(t, 3, 'name'), *t.exports[2]['obj']),
         'repeats export 2'),
        ('payload_exact', nn, lambda ua, ue, t, p: put(ua, exp_at(t, idx(p, 'SCS_Node_0'), 'size'), p.exports[idx(p, 'SCS_Node_0')]['size'] + 4),
         'reads to'),
        ('cdo_body_exact', nn, lambda ua, ue, t, p: put(ua, exp_at(t, 1, 'size'), p.exports[1]['size'] + 4), 'CDO tags end at'),
        ('super_index_matches_payload', nn, lambda ua, ue, t, p: put(ua, exp_at(t, 0, 'super'), 0), 'SuperIndex None'),
        ('struct_super_kinds', nn, lambda ua, ue, t, p: (put(ua, exp_at(t, 0, 'super'), 0), super_payload(ua, ue, t, p, 0, 0)),
         'a class without a super'),
        ('function_placement', nn, lambda ua, ue, t, p: put(ua, exp_at(t, idx(p, 'Tag'), 'outer'), 0), 'a function inside the package'),
        ('public_exports', nn, lambda ua, ue, t, p: put(ua, exp_at(t, idx(p, 'Tag'), 'flags'), p.exports[idx(p, 'Tag')]['flags'] & ~1),
         'function Tag is not RF_Public'),
        ('export_archetype', nn, lambda ua, ue, t, p: put(ua, exp_at(t, 1, 'tmpl'), 0), 'null TemplateIndex'),
        ('export_archetype', nn, archetype_twin, 'which rule 1 finds first'),
        ('export_outer_within', mood, within_of_itself, 'directly in the package, but its class', 'MD_Big'),
        ('payload_exact', ct, lambda ua, ue, t, p: put(ua, exp_at(t, idx(p, 'Rocks_GEN_VARIABLE'), 'size'),
                                                      p.exports[idx(p, 'Rocks_GEN_VARIABLE')]['size'] + 8),
         'a InstancedStaticMeshComponent (native InstancedStaticMeshComponent) reads to'),
        ('payload_exact', ct, lambda ua, ue, t, p: put(ua, exp_at(t, idx(p, 'Grass_GEN_VARIABLE'), 'size'),
                                                      p.exports[idx(p, 'Grass_GEN_VARIABLE')]['size'] - 8),
         'a HierarchicalInstancedStaticMeshComponent (native HierarchicalInstancedStaticMeshComponent) reads to'),
        ('payload_exact', os.path.join(os.path.dirname(mood), 'MD_Big'),
         lambda ua, ue, t, p: put(ua, exp_at(t, idx(p, 'MD_Big'), 'size'), p.exports[idx(p, 'MD_Big')]['size'] + 4),
         '(native PrimaryDataAsset) reads to'),
        ('package_flags', nn, lambda ua, ue, t, p: struct.pack_into('<I', ua, t.flags_at, t.flags | 0x2000),
         'PackageFlags 0x80002000'),
    ]
    for base in (nn, dp, ct):
        found = invariants.check(invariants.Package(base), set(tables.TABLES_RULES))
        assert not found, (os.path.basename(base), found)
    game = list(invariants.GAME_CONTENT)
    with tempfile.TemporaryDirectory() as tmp:
        invariants.GAME_CONTENT[:] = [tmp]          # a game folder is set, so a /Game package found nowhere is missing
        try:
            for rule, base, patch, want, *other in cases:
                at = lambda b: os.path.join(os.path.dirname(b), other[0]) if other else b   # the package the rule reads
                found = invariants.check(invariants.Package(at(base)), {rule})
                assert not found, ('clean as built', rule, os.path.basename(at(base)), found)
                found = [m for r, _, m in invariants.check(invariants.Package(at(mutated(tmp, base, patch))), {rule})]
                assert any(want in m for m in found), (rule, want, found)
        finally:
            invariants.GAME_CONTENT[:] = game
    print('ok  every TABLES rule reports its invariant broken in a patched package  (%d cases)' % len(cases))


name_numbers()
name_suffix()
subobject_template()
subobject_chain()
self_ref_import()
abstract_instances()
refused('AbstractComp', '  UE_COMPONENT(UPureComp, Comp);\n', 'abstract', top=ABSTRACT_COMP)
print('ok  AbstractComp: a component of an abstract mod class is refused, naming it abstract')
name_too_long()
tables_rules_fire()


# ---- CLASS: the class tail, what a class inherits from its parent (ScriptInherit flags, ClassWithin, ClassConfigName),
# instanced references, member names and the parameter block's width (invariant_rules/class_tail.py)

import struct, tempfile
import invariants

OBJ = '/Script/CoreUObject.Object'
PLAYER_CONTROLLER = '/Script/Engine.PlayerController'


def class_tail(base, name=None):
    """(Package, export index, Struct) of the class `name`_C (default: the package's namesake) at base."""
    pkg = invariants.Package(base)
    want = (name or os.path.basename(base)) + '_C'
    i = next(i for i, st in invariants.classes(pkg) if pkg.exports[i]['name'] == want)
    return pkg, i, pkg.struct(i)


def tail_facts(base, parent, bits, within, config):
    """The class at base names `parent` as its super and carries what the parent passes on: its ScriptInherit `bits`,
    its ClassWithin and its ClassConfigName (KismetCompiler.cpp:320-321, 2450-2453)."""
    pkg, i, st = class_tail(base)
    name = os.path.basename(base)
    assert pkg.path(st.super) == parent, (name, pkg.path(st.super))
    assert st.class_flags & bits == bits, '%s ClassFlags %#x lack %#x of %s' % (name, st.class_flags, bits & ~st.class_flags, parent)
    assert pkg.path(st.within) == within, '%s ClassWithin %s, %s is within %s' % (name, pkg.path(st.within), parent, within)
    assert st.config == config, '%s ClassConfigName %s, %s has %s' % (name, st.config, parent, config)
    return pkg, i, st


def class_tail_keep():
    """ClassTailKeep: children of the native parents AssetGen already follows carry each parent's flags, Within and
    ConfigName (UCLASS specifiers, and what every game Blueprint child of the parent carries), a mod child copies its
    mod parent's exactly, and every tail is a cooked Blueprint's: Parsed|CompiledFromBlueprint, bCooked, no
    ClassGeneratedBy."""
    folder = os.path.dirname(asset('ClassTailKeep'))
    for name, parent, bits, config in (('ClassTailKeep', '/Script/Engine.Actor', 0x800004, 'Engine'),        # Actor.h:131
                                       ('TailPart', '/Script/Engine.ActorComponent', 0xa00004, 'Engine'),    # ActorComponent.h:115
                                       ('TailScene', '/Script/Engine.SceneComponent', 0xa00004, 'Engine'),
                                       ('TailObject', OBJ, 0, 'Engine'),                                     # Object.h:57-60
                                       ('TailLib', '/Script/Engine.BlueprintFunctionLibrary', 0, 'Engine')):
        pkg, i, st = tail_facts(os.path.join(folder, name), parent, bits, OBJ, config)
        assert st.class_flags & 0x40010 == 0x40010 and st.cooked == 1 and st.generated_by == 0, (name, hex(st.class_flags), st.cooked)
        keeps_invariants(os.path.join(folder, name))
    _, _, mine = class_tail(os.path.join(folder, 'ClassTailKeep'))
    kid = tail_facts(os.path.join(folder, 'TailKid'), '/Game/_ElytrasMods/ClassTailKeep/ClassTailKeep.ClassTailKeep_C',
                     mine.class_flags & 0x4AA1364E, OBJ, mine.config)
    keeps_invariants(os.path.join(folder, 'TailKid'))
    print('ok  ClassTailKeep: each class carries its parent\'s ScriptInherit flags, ClassWithin and ClassConfigName')
    # NumReplicatedProperties covers the replicated member: GetLifetimeBlueprintReplicationList registers that many
    # CPF_Net properties and stops (BlueprintGeneratedClass.cpp:1786-1799).
    pkg, i, st = class_tail(asset('ClassTailKeep'))
    tag = pkg.tag(i, 'NumReplicatedProperties')
    count = struct.unpack_from('<i', tag['value'])[0] if tag else 0
    assert count >= sum(1 for p in st.props if p.flags & 0x20) == 1, (count, [(p.name, hex(p.flags)) for p in st.props])
    fields = {'Count': 5}
    assert run(asset('ClassTailKeep'), 'Next', self_vars=fields)[0] == 6 and fields['Count'] == 6, fields
    fields = {'Count': 2, 'Extra': 10}
    assert run_as([os.path.join(folder, 'TailKid'), asset('ClassTailKeep')], 'Both', fields) == 13 and fields['Count'] == 3, fields
    assert run(os.path.join(folder, 'TailLib'), 'Twice', V=21)[0] == 42
    print('ok  ClassTailKeep: NumReplicatedProperties covers Shared; Next, TailKid.Both and TailLib.Twice run as in C++')


def parm_width():
    """ParmWidth: 254 arguments and a return value - 255 CPF_Parm properties, the most UFunction::NumParms (a uint8,
    Class.h:1800-1805) counts - cook, keep every invariant (function_parms_fit among them), and run."""
    base = asset('ParmWidth')
    pkg = invariants.Package(base)
    edge = next(pkg.struct(i) for i, e in enumerate(pkg.exports) if e['name'] == 'Edge')
    parms = [p for p in edge.props if p.flags & 0x80]
    assert len(parms) == 255 and sum(1 for p in parms if p.flags & 0x400) == 1, len(parms)
    keeps_invariants(base)
    for scale in (1, -7):
        args = {'A%d' % k: (k * 31 - 1000) * scale for k in range(254)}
        assert run(base, 'Edge', **args)[0] == args['A0'] - args['A126'] + args['A253']
    print('ok  ParmWidth: 254 arguments and a result cook as 255 parameters and run')


def parm_over():
    """255 arguments and a result would be 256 CPF_Parm properties: NumParms wraps to 0 (Class.cpp:5638-5651)."""
    refused('ParmOver', '  int32 Sum(%s) { return A0; }\n' % ', '.join('int32 A%d' % k for k in range(255)),
            'ParmOver::Sum: 256 parameters, the return value included; a function takes at most 255')
    print('ok  ParmWidth: 255 arguments and a result (256 parameters, NumParms a uint8) are refused')


def parm_huge():
    """A 65600-byte struct argument: ParmsSize, a uint16, wraps and ProcessEvent copies the wrong range
    (ScriptCore.cpp:1952-1958)."""
    refused('ParmHuge', '  int32 Take(FParmHuge B) { return 0; }\n', "a function's parameter block holds at most 65535",
            top='struct FParmHuge {\n  UE_STRUCT;\n%s};\n' % ''.join('  int32 M%d;\n' % k for k in range(16400)))
    print('ok  ParmWidth: a 65600-byte parameter block (ParmsSize a uint16) is refused')


# Children of native parents with a non-default tail (ClassTailMeta): parent, the ScriptInherit bits it passes on,
# its ClassWithin and ClassConfigName, from UE 4.27's UCLASS specifiers and the game's own Blueprint children.
TAIL_META = {
    'ClassTailMeta': ('/Script/FSD.StatusEffect', 0x801000, OBJ, 'Engine'),                 # all 242 game STE_ BPs: 0x801000
    'MetaHud': ('/Script/Engine.HUD', 0x80020c, OBJ, 'Game'),                               # HUD.h:35
    'MetaController': ('/Script/Engine.PlayerController', 0x800204, OBJ, 'Game'),           # PlayerController.h:222, Controller.h:39
    'MetaMode': ('/Script/Engine.GameModeBase', 0x80020c, OBJ, 'Game'),                     # GameModeBase.h:45
    'MetaCheats': ('/Script/Engine.CheatManager', 0, PLAYER_CONTROLLER, 'Engine'),          # CheatManager.h:87
    'MetaSettings': ('/Script/Engine.GameUserSettings', 0x40000004, OBJ, 'GameUserSettings'),  # GameUserSettings.h:37
    'MetaWalker': ('/Script/Engine.Character', 0x800004, OBJ, 'Game'),                      # Character.h:213
    'MetaTask': ('/Script/AIModule.BTTask_BlueprintBase', 0, OBJ, 'Game'),                  # BTNode.h:36
    'MetaDamage': ('/Script/Engine.DamageType', 0x10000, OBJ, 'Engine'),                     # DamageType.h:19
    'MetaAnim': ('/Script/Engine.AnimInstance', 0x800008, '/Script/Engine.SkeletalMeshComponent', 'Engine'),  # AnimInstance.h:361
    'MetaInput': ('/Script/Engine.PlayerInput', 0xc, PLAYER_CONTROLLER, 'Input'),           # PlayerInput.h:333
}


def tail_meta(name):
    def test():
        base = os.path.join(os.path.dirname(asset('ClassTailMeta')), name)
        tail_facts(base, *TAIL_META[name])
        keeps_invariants(base)
        if name == 'ClassTailMeta':
            assert run(base, 'More', self_vars={'Stacks': 4})[0] == 5
        if name == 'MetaCheats':
            fields = {'N': 2}
            run(base, 'Bump', self_vars=fields)
            assert fields['N'] == 3, fields
    return test


def walker_config():
    """OverrideTest's Walker derives from ACharacter, config=Game (Character.h:213): its config members read Game.ini."""
    pkg, i, st = class_tail(os.path.join(os.path.dirname(asset('OverrideTest')), 'Walker'))
    assert st.config == 'Game', 'Walker_C ClassConfigName %s, ACharacter has Game' % st.config


def instanced_refs():
    """InstancedRefs: a member typed by a component class - native, or the mod's own RefPart - is
    CPF_InstancedReference; an array, map or struct member holding one is CPF_ContainsInstancedReference; the class is
    CLASS_HasInstancedReference (KismetCompilerMisc.cpp:948-952, 974-977, 1215-1218, 1254-1257;
    KismetCompiler.cpp:2521-2529). Instancing walks only flagged members (Class.cpp:2152-2163)."""
    base = asset('InstancedRefs')
    pkg, i, st = class_tail(base)
    part = class_tail(os.path.join(os.path.dirname(base), 'RefPart'))[2]
    assert part.class_flags & 0x200000, 'RefPart ClassFlags %#x lack DefaultToInstanced' % part.class_flags
    props = {p.name: p for p in st.props}
    need = [('Root', props['Root'].flags, 0x80000), ('Spare', props['Spare'].flags, 0x80000),
            ('Pieces', props['Pieces'].flags, 0x8000000000), ('Pieces inner', props['Pieces'].subs[0].flags, 0x80000),
            ('ByIndex', props['ByIndex'].flags, 0x8000000000), ('ByIndex value', props['ByIndex'].subs[1].flags, 0x80000),
            ('LastHit', props['LastHit'].flags, 0x8000000000), ('Mine', props['Mine'].flags, 0x80000)]
    missing = ['%s %#x lacks %#x' % (n, f, bit) for n, f, bit in need if not f & bit]
    assert not missing, '; '.join(missing)
    assert st.class_flags & 0x800000, hex(st.class_flags)
    keeps_invariants(base)
    assert run(base, 'CountPieces', self_vars={'Pieces': ['a', 'b', 'c']})[0] == 3
    print('ok  InstancedRefs: component references are flagged instanced, their containers and HitResult contain one, '
          'the class HasInstancedReference')


def run_fname(chain, fn, fields, **parms):
    """run_as without its exact-case name match: fn is run on an object of class chain[0] (mod ancestors chain[1:]),
    and every call by name - the first included - finds the most derived function of that FName, case-insensitively,
    as the VM does (EX_VirtualFunction / ProcessEvent: FindFunctionChecked through FuncMap)."""
    import runscript
    names = {b: exports_of(b) for b in chain}

    def owner(f):
        return next((b, n) for b in chain for n in names[b] if n.lower() == f.lower())
    saved = runscript.run, runscript.params_of
    runscript.run = lambda base, f, self_vars=None, **p: saved[0](*owner(f), self_vars, **p)
    runscript.params_of = lambda base, f, *flag: saved[1](*owner(f), *flag)
    try:
        return saved[0](*owner(fn), fields, **parms)[0]
    finally:
        runscript.run, runscript.params_of = saved


def refused_or_distinct(mod, classes_src, member, oracle=None):
    """A mod whose members are one FName (FNames compare case-insensitively), or a C++ overload set a Blueprint class
    cannot hold: the compiler must refuse it, naming `member`, or cook every class with its members apart
    (member_names_distinct) and, given `oracle` = (function, value[, class chain]), the function returning what C++
    returns - run on the mod's class, or dispatched by FName on an object of the chain's first class.
    The editor refuses a duplicate function and renames a clashing variable (KismetCompiler.cpp:570-616, 1737-1747)."""
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, mod + '.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/Engine.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/%s");\n%s' % (mod, classes_src))
        out = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', mod)
        os.makedirs(out)
        proc = assetgen_compile([src, UEAPI, out])
        if proc.returncode:
            assert member in proc.stdout, (mod, proc.stdout[-400:])
            return
        found = [f for b in invariants.packages([out]) for f in invariants.check(invariants.Package(b), {'member_names_distinct'})]
        assert not found, '%s cooks %s: %s' % (mod, found[0][1], found[0][2])
        if oracle:
            chain = oracle[2] if len(oracle) > 2 else [mod]
            got = run_fname([os.path.join(out, c) for c in chain], oracle[0], {})
            assert got == oracle[1], '%s: %s() on a %s returns %r, C++ returns %r' % (mod, oracle[0], chain[0], got, oracle[1])


SHADOW_BASE = 'class ShadowBase : public AActor {\npublic:\n  int32 Score;\n};\n'
# name: (source, the member the refusal names, oracle or None, what the gap is)
SHADOWS = {
    'ShadowVarProp': (SHADOW_BASE + 'class ShadowVarProp : public ShadowBase {\npublic:\n  int32 Score;\n};\n', 'Score', None,
                      "a variable named like its Blueprint parent's (Score)"),
    'ShadowNative': ('class ShadowNative : public AActor {\npublic:\n  bool bHidden = true;\n};\n', 'bHidden', None,
                     "a variable named like AActor's bHidden"),
    'ShadowCase': ('class ShadowCase : public AActor {\npublic:\n  int32 Score;\n  int32 score;\n};\n', 'score', None,
                   'two variables of one FName (Score, score)'),
    'ShadowFnCase': ('class ShadowFnCase : public AActor {\npublic:\n  int32 Get() { return 1; }\n  int32 get() { return 2; }\n'
                     '  int32 Use() { return Get() * 10 + get(); }\n};\n', 'get', ('Use', 12),
                     'two functions of one FName (Get, get), Use() = 12'),
    'ShadowSuperCase': ('class ShadowSuperBase : public AActor {\npublic:\n  virtual int32 Get() { return 1; }\n};\n'
                        'class ShadowSuperCase : public ShadowSuperBase {\npublic:\n  int32 get() { return 2; }\n};\n', 'get',
                        ('Get', 1, ['ShadowSuperCase', 'ShadowSuperBase']),
                        "a new function get() beside the parent's virtual Get(), one FName; Get() on the child = 1"),
    'ShadowOverload': ('class ShadowOverload : public AActor {\npublic:\n  int32 Ov(int32 A) { return A * 100; }\n'
                       '  int32 Ov(int32 A, int32 B) { return A + B; }\n  int32 Use() { return Ov(1) + Ov(1, 2); }\n};\n',
                       'Ov', ('Use', 103), 'two non-inline overloads Ov(A), Ov(A, B), Use() = 103'),
    'ShadowOverloadType': ('class ShadowOverloadType : public AActor {\npublic:\n  int32 Ov(int32 A) { return A; }\n'
                           '  int32 Ov(float A) { return 2; }\n  int32 Use() { return Ov(1) * 10 + Ov(1.0f); }\n};\n',
                           'Ov', ('Use', 12), 'two non-inline overloads Ov(int32), Ov(float), Use() = 12'),
}


class_tail_keep()
parm_width()
parm_over()
parm_huge()
for _name, (_parent, _bits, _within, _config) in TAIL_META.items():
    tail_meta(_name)()
    print('ok  ClassTailMeta %s: %s\'s tail (flags %#x, within %s, config %s)' % (_name, _parent.split('.')[-1], _bits,
                                                                                 _within.split('.')[-1], _config))
walker_config()
print('ok  OverrideTest Walker: ClassConfigName Game, ACharacter\'s')
instanced_refs()
for _mod, (_src, _member, _oracle, _what) in SHADOWS.items():
    refused_or_distinct(_mod, _src, _member, _oracle)
print('ok  member names: one per FName, overloads and case twins included, none reused from an ancestor (%d cases)'
      % len(SHADOWS))


# ---- FUNC: function flags, overrides and interface implementations, call kind vs callee (invariant_rules/functions.py)

import sys
import invariants
from invariant_rules import functions as func_rules


def func_findings(base, names):
    """What the named invariants.py rules find on the package at base, as '<rule> <export>: <message>' lines."""
    return ['%s %s: %s' % f for f in invariants.check(invariants.Package(base), set(names))]


def func_calls(base):
    """(function, opcode, callee path, callee FunctionFlags) of every call in the package whose callee the rules can
    resolve: its own class chain for a call by name on self, the import or export for a call by object."""
    pkg, out = invariants.Package(base), []
    for i, st in invariants.functions(pkg):
        for n, mine in (x for t in pkg.script(i) for x in t.on_self()):
            if n.op in (0x1B, 0x1C, 0x45, 0x46, 0x68):
                f = func_rules.call_target(pkg, i, n, mine)
                if f is not None: out.append((pkg.exports[i]['name'], n.op, f.where, f.flags))
    return out


ROUTED = 0x40 | 0x4 | 0x8 | 0x100 | 0x1000 | 0x80 | 0x200000 | 0x1000000 | 0x4000


def func_rpc_routing():
    """FuncRpcRouting: every call to an RPC, authority-only, cosmetic or multicast function - inherited, overridden,
    or the parent's through `RpcRouteBase::ServerOpen` - goes through the engine's callspace routing (EX_VirtualFunction
    / EX_FinalFunction), and on the authority each runs the body C++ says: the parent's for the qualified call."""
    kid = asset('FuncRpcRouting')
    base = os.path.join(os.path.dirname(kid), 'RpcRouteBase')
    calls = [c for c in func_calls(kid) if c[3] & ROUTED]
    got = sorted((fn, where.rsplit('/', 1)[-1]) for fn, op, where, flags in calls)
    assert got == [('Inherited', 'RpcRouteBase.RpcRouteBase_C:AuthOnly'), ('Inherited', 'RpcRouteBase.RpcRouteBase_C:MultiPing'),
                   ('Inherited', 'RpcRouteBase.RpcRouteBase_C:Pretty'), ('OpenViaParent', 'RpcRouteBase.RpcRouteBase_C:ServerOpen'),
                   ('OwnRpc', 'FuncRpcRouting.FuncRpcRouting_C:ServerOpen')], got
    assert all(op in (0x1B, 0x1C) for _, op, _, _ in calls), calls
    for b in (kid, base):
        found = func_findings(b, ['call_local_unrouted', 'func_override_flags', 'func_override_params', 'func_super_link'])
        assert not found, found
    for fn, want in (('OpenViaParent', 1), ('Inherited', 114), ('OwnRpc', 7)):
        fields = {'Opened': 0}
        run_as([kid, base], fn, fields)
        assert fields['Opened'] == want, (fn, fields)
    print('ok  FuncRpcRouting: inherited, overridden and parent-qualified RPC / authority / cosmetic calls keep their routing')


def func_stub_iface():
    """FuncStubIface: an interface's own functions are stubs with no effect - whatever they return or take by
    reference - and the implementer's function and its stubs for the rest do what C++ says."""
    folder = os.path.dirname(asset('FuncStubIface'))
    iface, impl = os.path.join(folder, 'IStubbed'), asset('FuncStubIface')
    for b in (iface, impl):
        found = func_findings(b, ['interface_class_stubs', 'func_override_flags', 'func_override_params', 'func_super_link',
                                  'func_has_out_parms', 'func_parm_flags'])
        assert not found, found
    for fn, parms in (('Count', dict(By=3)), ('Touch', dict(By='x')), ('Pair', dict(A=1, Out=9))):
        me = {'Seen': 4}
        ret, env = run(iface, fn, self_vars=me, **parms)
        assert ret in (0, None) and me == {'Seen': 4} and env.get('Out', 9) == 9, (fn, ret, me, env)
    me = {'Seen': 2}
    assert run(impl, 'Count', self_vars=me, By=3)[0] == 5 and me == {'Seen': 5}, me
    ret, env = run(impl, 'Pair', self_vars=me, A=1, Out=9)
    assert ret == 0 and env['Out'] == 9 and me == {'Seen': 5}, (ret, env, me)
    assert run(impl, 'Touch', self_vars=me, By='x')[0] is None and me == {'Seen': 5}, me
    print('ok  FuncStubIface: interface stubs change nothing; the implementation and its own stubs run as written')


def func_refusals():
    """Implementing a native interface one of whose functions is native only (no BlueprintNativeEvent /
    BlueprintImplementableEvent): no Blueprint override of it is ever reached, so the class is refused."""
    refused('FuncNativeOnly', '', 'is native only',
            top='class NativeOnlyImpl : public AActor, public IGameplayTagAssetInterface {\npublic:\n};\n')
    print('ok  a class implementing a native interface with a native-only function is refused')


func_rpc_routing()
func_stub_iface()
func_refusals()


# ---- FUNC pending

def func_local_defaults():
    """FuncLocalDefaults: locals whose zeroed memory is not their value start constructed, as C++ has them."""
    base = asset('FuncLocalDefaults')
    assert run(base, 'EmptyText')[0] == ''
    assert run(base, 'IdentityScale')[0] == 2.0, run(base, 'IdentityScale')[0]
    assert run(base, 'HitTime')[0] == 1.0, run(base, 'HitTime')[0]
    assert run(base, 'StructDefault')[0] == 5, run(base, 'StructDefault')[0]


def func_cosmetic_static():
    """FuncCosmeticStatic: ApplyDamage (BlueprintAuthorityOnly) and PlaySound2D (BlueprintCosmetic) are called
    through CallFunction's callspace check, as the editor calls them."""
    if not SDK:
        print('--  FuncCosmeticStatic: skipped (needs --sdk: the native callees\' flags are read off the dump)')
        return False
    base = asset('FuncCosmeticStatic')
    calls = {(fn, where.rsplit(':', 1)[-1]): op for fn, op, where, flags in func_calls(base) if flags & ROUTED}
    assert set(calls) == {('Hit', 'ApplyDamage'), ('Beep', 'PlaySound2D')}, calls
    assert all(op in (0x1B, 0x1C) for op in calls.values()), 'called with %s' % {k: '%02x' % v for k, v in calls.items()}
    return True


def func_qualified_call():
    """FuncQualifiedCall: `QcParent::Fn()` on a QcKid runs QcParent's Fn - copied in, or, for an authority-only, RPC or
    noinline Fn or a UE_NO_OPTIMIZE caller, bound from the override of Fn the calling class gets, which forwards to
    QcParent's. That override has QcParent's flags and is QcKid's super; a call by name runs what it ran before it.
    A body making such a call is not copied into a subclass's function, where the call would be the subclass's: a
    QcRelayKid, which overrides AuthOnly, still runs QcParent's through FuncQualifiedCall::CallParentAuth, and its call
    to QcRelay::RelayServer leaves RelayServer's parent call bound from QcRelay, which has a ServerBump. Copied into
    QcFinalPlain, CallParentPlain's `QcParent::Plain()` is still QcParent's, not QcFinalPlain's final Plain."""
    folder = os.path.dirname(asset('FuncQualifiedCall'))
    chain = [os.path.join(folder, c) for c in ('QcKid', 'FuncQualifiedCall', 'QcParent')]
    relay = [os.path.join(folder, c) for c in ('QcRelayKid', 'QcRelay')] + chain[1:]
    final = [os.path.join(folder, 'QcFinalPlain')] + chain[1:]
    for b in chain + relay[:2] + final[:1]: keeps_invariants(b)
    for fn, fields, want in (('CallRelayAuth', {}, 3), ('CallRelayServer', {'Seen': 1}, 3)):
        run_as(relay, fn, fields)
        assert fields.get('Seen') == want, (fn, fields)
    got = run_as(final, 'CallRelayPlain', {})
    assert got == 5, 'QcParent::Plain() copied into QcFinalPlain returned %r' % got
    for fn in ('CallParentPlain', 'CallParentPlainSlow'):
        got = run_as(chain, fn, {})
        assert got == 5, '%s: QcParent::Plain() on a QcKid returned %r' % (fn, got)
    assert run_as(chain, 'CallParentKept', {}) == 2
    for fn, fields, want in (('CallParentAuth', {}, 3), ('CallParentServer', {'Seen': 1}, 5)):
        run_as(chain, fn, fields)
        assert fields.get('Seen') == want, (fn, fields)
    kid, mine, parent = (invariants.Package(b) for b in chain)
    for fn in ('AuthOnly', 'ServerBump', 'Kept', 'Plain'):
        sup = kid.struct(kid.find(fn)).super
        assert kid.path(sup).endswith('FuncQualifiedCall.FuncQualifiedCall_C:' + fn), (fn, kid.path(sup))
        got, want = mine.struct(mine.find(fn)).function_flags, parent.struct(parent.find(fn)).function_flags
        assert got == want, 'FuncQualifiedCall::%s FunctionFlags %#x, QcParent\'s %#x' % (fn, got, want)
    for objects, want in ((chain, 30), (chain[1:], 3)):     # `AuthOnly()`: the object's own, the forwarder's QcParent's
        fields = {}
        run_as(objects, 'CallAuth', fields)
        assert fields.get('Seen') == want, (os.path.basename(objects[0]), fields)
    assert run_as(chain[1:], 'Kept', {}, V=1) == 2


def func_qualified_multicast():
    """The same call to a multicast stays a call by name, with a warning: on a server a multicast runs here and is sent
    (Local | Remote, Actor.cpp 4270-4278), so a forwarding override would send it, and its call to the parent's again."""
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'QcMulti.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/QcMulti");\n'
                    'class QcMultiBase : public AActor {\npublic:\n  int32 Seen = 0;\n  UE_MULTICAST void Ping() { Seen = 1; }\n};\n'
                    'class QcMulti : public QcMultiBase {\npublic:\n  void Use() { QcMultiBase::Ping(); }\n};\n')
        proc = assetgen_compile([src, UEAPI, tmp])
        assert proc.returncode == 0 and 'QcMultiBase::Ping() is a call by name' in proc.stdout and 'multicast' in proc.stdout, proc.stdout
        assert 'Ping' not in exports_of(os.path.join(tmp, 'QcMulti')), exports_of(os.path.join(tmp, 'QcMulti'))


def func_ancestor_iface():
    """FuncAncestorIface: OnMessageAI on an AWoodLouse child replaces /Script/FSD.TriggerAI:OnMessageAI - linked as its
    super with the interface function's inherited flags and parameters - and runs as written."""
    base = asset('FuncAncestorIface')
    pkg = invariants.Package(base)
    fi = pkg.find('OnMessageAI')
    me = {}
    run(base, 'OnMessageAI', self_vars=me, TriggerName='Boom')
    assert me.get('Heard') == 'Boom', me
    st = pkg.struct(fi)
    got = pkg.path(st.super) if st.super else None
    assert got == '/Script/FSD.TriggerAI:OnMessageAI', 'OnMessageAI super %s, FunctionFlags %#x' % (got, st.function_flags)
    found = func_findings(base, ['func_override_flags', 'func_override_params', 'func_super_link'])
    assert not found, found


func_local_defaults()
print('ok  FuncLocalDefaults: FText / FTransform / FHitResult / defaulted-struct locals start constructed (FUNC_HasDefaults)')
if func_cosmetic_static():
    print('ok  FuncCosmeticStatic: ApplyDamage / PlaySound2D keep their callspace routing (not EX_CallMath)')
func_qualified_call()
print('ok  FuncQualifiedCall: Parent::Fn() runs the parent\'s function, copied in or bound from a forwarding override')
func_qualified_multicast()
print('ok  a qualified call to a multicast from a class without one warns and gets no forwarding override')
func_ancestor_iface()
print('ok  FuncAncestorIface: an override of a native ancestor\'s interface function links it as super (TriggerAI:OnMessageAI)')
refused('FuncAncestorParams', '', 'OnMessageAI',
        top='class AncestorLouse : public AWoodLouse {\npublic:\n  int32 Seen;\n'
            '  void OnMessageAI(int32 TriggerName) { Seen = TriggerName; }\n};\n')
print('ok  FuncAncestorParams: an override of a native ancestor\'s interface function with other parameters is refused')
# An override or an interface implementation keeps the parameters of the function it replaces: a caller lays them out
# for that one (ProcessEvent, an interface's Execute_, a received RPC) and ProcessEvent copies them into this one's
# frame (ScriptCore.cpp:1958-2016). Only a Blueprint event is replaced at all: C++ and bound calls keep a native one.
refused('FuncTickInt', '  int32 Seen;\n  void ReceiveTick(int32 Frames) { Seen = Frames; }\n',
        'FuncTickInt::ReceiveTick is void (int32), and the AActor::ReceiveTick it replaces is void (float)')
refused('FuncSigShadow', '', 'SigKid::Scale is int32 (float), and the SigBase::Scale it replaces is int32 (int32)',
        top='class SigBase : public AActor {\npublic:\n  int32 Scale(int32 X) { return X; }\n};\n'
            'class SigKid : public SigBase {\npublic:\n  int32 Scale(float X) { return 0; }\n};\n')
refused('FuncRpcKidParams', '', 'RpcParamsKid::ServerNudge is void (float, int32), and the RpcKidParamsBase::ServerNudge '
        'it replaces is void (int32)',
        top='class RpcKidParamsBase : public AActor {\npublic:\n  UE_SERVER void ServerNudge(int32 V) {}\n};\n'
            'class RpcParamsKid : public RpcKidParamsBase {\n  int32 Got = 0;\n\npublic:\n'
            '  void ServerNudge(float V, int32 W) { Got = W; }\n};\n')
refused('FuncIfaceParams', '', 'IfaceParamsKid::Score is int32 (float, int32), and the IScoredParams::Score it replaces '
        'is int32 (int32)',
        top='class IScoredParams {\npublic:\n  UE_INTERFACE;\n  int32 Score(int32 Times);\n};\n'
            'class IfaceParamsKid : public AActor, public IScoredParams {\npublic:\n'
            '  int32 Score(float Times, int32 Extra) { return Extra; }\n};\n')
refused('FuncHideNative', '  int32 Seen;\n  void K2_DestroyActor() { Seen = 1; }\n',
        'AActor::K2_DestroyActor is native and no Blueprint event')
print('ok  override refusals: other parameters than a native event\'s, a mod parent\'s, an RPC\'s or an interface\'s; '
      'a name of a native non-event')


def func_final_inherited():
    """FuncFinalInherited (`final`) calls FfBase's AuthOnly, ServerBump and Kept unqualified, FfOwn (`final`) its own
    AuthOnly, an override, and FfOther AuthOnly through a FuncFinalInherited pointer. None of them is FUNC_Final, so
    each is a call by name, which finds that same function, no class deriving from a final one (call_opcode_flags:
    the editor binds a call to a function without FUNC_Final only as a parent call); each runs that function."""
    kid = asset('FuncFinalInherited')
    folder = os.path.dirname(kid)
    base, own, other = (os.path.join(folder, c) for c in ('FfBase', 'FfOwn', 'FfOther'))
    for b in (kid, own, other): keeps_invariants(b)
    for chain, fn, want in (([kid, base], 'CallAuth', 3), ([kid, base], 'CallServer', 2), ([own, base], 'CallAuth', 30)):
        fields = {'Seen': 0}
        run_as(chain, fn, fields)
        assert fields['Seen'] == want, (os.path.basename(chain[0]), fn, fields)
    assert run_as([kid, base], 'CallKept', {}) == 2


func_final_inherited()
print('ok  FuncFinalInherited: a final class calls an inherited or overriding function that is not FUNC_Final by name')


def func_inline_parent():
    """FuncInlineParent: `IpBase::AuthOnly()` in an inline method runs IpBase's AuthOnly in each class its body is
    copied into - FuncInlineParent and IpKid, below IpMid's AuthOnly; IpDirect, with none between, also on an
    IpDirectKid, which overrides AuthOnly - bound from that class's own AuthOnly, an override forwarding to its
    parent's (call_opcode_flags), with no warning. A call by name to AuthOnly still runs the object's own."""
    top = asset('FuncInlineParent')
    p = lambda *cs: [os.path.join(os.path.dirname(top), c) for c in cs]
    mid = p('FuncInlineParent', 'IpMid', 'IpBase')
    kid, direct = p('IpKid') + mid, p('IpDirect', 'IpBase')
    dkid = p('IpDirectKid') + direct
    for b in mid[:2] + kid[:1] + direct[:1] + dkid[:1]: keeps_invariants(b)
    for chain, fn, want in ((mid, 'Use', 3), (kid, 'Use', 3), (kid, 'UseKid', 3), (direct, 'Use2', 3), (dkid, 'Use2', 3),
                            (dkid, 'UseKid2', 3), (kid, 'AuthOnly', 7), (dkid, 'AuthOnly', 70)):
        fields = {'Seen': 0}
        run_as(chain, fn, fields)
        assert fields['Seen'] == want, (os.path.basename(chain[0]), fn, fields)
    assert 'is a call by name' not in LOGS['FuncInlineParent'], LOGS['FuncInlineParent']


func_inline_parent()
print('ok  FuncInlineParent: Base::Fn() in an inline method binds Base\'s from each class the body is copied into')


def func_qualified_self():
    """FuncQualifiedSelf: `FuncQualifiedSelf::H()` in the class's own code runs its own H on an SqKid too, which
    overrides H: H's body is copied in. Auth's cannot be (authority-only), and a call bound to a function a subclass
    can override is no Blueprint's, so that one stays a call by name, with a warning."""
    kid = os.path.join(os.path.dirname(asset('FuncQualifiedSelf')), 'SqKid')
    chain = [kid, os.path.join(os.path.dirname(kid), 'FuncQualifiedSelf')]
    for b in chain: keeps_invariants(b)
    got = run_as(chain, 'CallH', {})
    assert got == 5, 'FuncQualifiedSelf::H() on an SqKid returned %r' % got
    log = LOGS['FuncQualifiedSelf']
    assert 'FuncQualifiedSelf::Auth() is a call by name' in log and 'FuncQualifiedSelf::H()' not in log, log


func_qualified_self()
print('ok  FuncQualifiedSelf: Self::Fn() in its own class runs its own Fn, copied in, or warns')


def func_forwarder_order():
    """FuncForwarderOrder: AaFoKid and ZzFoKid each get an override of Auth forwarding to FoMid's, itself one
    forwarding to FoRoot's. Each kid's calls the function it overrides, its super, as the editor's call to a parent
    function does, whichever side of FoMid the kid's name sorts on."""
    mid = os.path.join(os.path.dirname(asset('FuncForwarderOrder')), 'FoMid')
    folder = os.path.dirname(mid)
    for kid in ('AaFoKid', 'ZzFoKid'):
        base = os.path.join(folder, kid)
        pkg = invariants.Package(base)
        sup = pkg.path(pkg.struct(pkg.find('Auth')).super)
        calls = [where for fn, op, where, flags in func_calls(base) if fn == 'Auth']
        assert sup.endswith('/FoMid.FoMid_C:Auth') and calls == [sup], (kid, sup, calls)
        fields = {'Seen': 0}
        run_as([base, mid, os.path.join(folder, 'FoRoot')], 'KidCall', fields)
        assert fields['Seen'] == 3, (kid, fields)


func_forwarder_order()
print('ok  FuncForwarderOrder: a forwarding override calls its own super, whatever its class\'s name')


def func_iface_inherited():
    """FuncIfaceInherited implements IFiTell, whose Tell, Kept, Ping, Twice and Auth it inherits from FiRoot. Each of
    its own calls FiRoot's - Twice, inline, expanded in it; Auth, authority-only, bound and authority-only itself, as an
    override takes its parent's flags - so a call by name or through the interface runs FiRoot's: either finds the
    class's own function first (UClass::FindFunctionByName, Class.cpp 5281-5323), and with none would find the
    interface's empty one before the super's. FiKid's Tell and Auth override those two, and their `FiRoot::` calls run
    FiRoot's."""
    kid = asset('FuncIfaceInherited')
    folder = os.path.dirname(kid)
    chain = [kid, os.path.join(folder, 'FiRoot')]
    fikid = [os.path.join(folder, 'FiKid')] + chain
    for b in (kid, fikid[0]): keeps_invariants(b)
    assert {'Tell', 'Kept', 'Ping', 'Twice', 'Auth'} <= set(exports_of(kid)), exports_of(kid)
    got = run_as(chain, 'Tell', {}, V=2), run_as(chain, 'Kept', {}, V=2), run_as(chain, 'Twice', {}, V=2)
    assert got == (3, 20, 4), 'Tell(2), Kept(2), Twice(2) on a FuncIfaceInherited returned %r, %r, %r' % got
    for c, fn, parms, want, seen in ((chain, 'Ping', {}, None, 9), (chain, 'Auth', {'V': 2}, 6, 2), (fikid, 'Auth', {'V': 2}, 106, 2)):
        fields = {'Seen': 0}
        got = run_as(c, fn, fields, **parms)
        assert (want is None or got == want) and fields['Seen'] == seen, (os.path.basename(c[0]), fn, got, fields)
    assert run_as(fikid, 'Tell', {}, V=2) == 103
    pkg = invariants.Package(kid)
    assert pkg.struct(pkg.find('Auth')).function_flags & 0x4, 'FuncIfaceInherited::Auth is not BlueprintAuthorityOnly'
    pkg = invariants.Package(fikid[0])
    for fn in ('Tell', 'Auth'):
        sup = pkg.path(pkg.struct(pkg.find(fn)).super)
        assert sup.endswith('/FuncIfaceInherited.FuncIfaceInherited_C:' + fn), (fn, sup)


func_iface_inherited()
print('ok  FuncIfaceInherited: an interface function an ancestor has runs the ancestor\'s, not an empty stub')
# A multicast no forwarder can call (it would be sent twice on a server), so the stub would replace it: refused.
refused('FuncIfaceMulticast', '', 'the IfmRoot::Ping it inherits is a multicast',
        top='class IIfmPing {\npublic:\n  UE_INTERFACE;\n  void Ping();\n};\n'
            'class IfmRoot : public AActor {\npublic:\n  int32 Seen = 0;\n  UE_MULTICAST void Ping() { Seen = 1; }\n};\n'
            'class IfmKid : public IfmRoot, public IIfmPing {\npublic:\n};\n')
print('ok  FuncIfaceMulticast: an interface function inherited as a multicast, which no override can call, is refused')
# A static of that name: the class's function of that name, the stub too, is an override of the static to the editor
# (its super is ParentClass->FindFunctionByName), which refuses a non-static one ("Check flags: Exec, Final, Static").
refused('FuncIfaceStatic', '', 'the FsRoot::Tell it inherits is static: the editor takes such a function for an override '
        'of the static and refuses it',
        top='class IFsTell {\npublic:\n  UE_INTERFACE;\n  int32 Tell(int32 V);\n};\n'
            'class FsRoot : public AActor {\npublic:\n  static int32 Tell(int32 V) { return V + 1; }\n};\n'
            'class FsKid : public FsRoot, public IFsTell {\npublic:\n  int32 Ask() { return FsRoot::Tell(4); }\n};\n')
print('ok  FuncIfaceStatic: an interface function inherited as a static is refused, as the editor refuses its override')


def func_static_above():
    """A function named like a mod ancestor's static is an override of it to the editor (its super is
    ParentClass->FindFunctionByName, KismetCompiler.cpp 1733-1774), which refuses one that is not static, or a static
    over one that is not ("Check flags: Exec, Final, Static", 1855-1868): a method of the class's own, an interface
    implementation it declares, a static over a method. A static over a static splits no caller - each call to either is
    bound - so FuncStaticHide compiles, its Tell linked to FshRoot's as its super, and each call runs the one it names."""
    refused('StaticAboveOwn', '', 'SaRoot::Tell is static, and the editor takes a function of that name in a subclass '
            'for an override of it',
            top='class SaRoot : public AActor {\npublic:\n  static int32 Tell(int32 V) { return V + 1; }\n};\n'
                'class SaKid : public SaRoot {\npublic:\n  int32 Tell(int32 V) { return V * 2; }\n};\n')
    refused('StaticAboveIface', '', 'the SaiRoot::Tell it inherits is static: the editor takes such a function for an '
            'override of the static and refuses it',
            top='class ISaiTell {\npublic:\n  UE_INTERFACE;\n  int32 Tell(int32 V);\n};\n'
                'class SaiRoot : public AActor {\npublic:\n  static int32 Tell(int32 V) { return V + 1; }\n};\n'
                'class SaiKid : public SaiRoot, public ISaiTell {\npublic:\n  int32 Tell(int32 V) { return V * 4; }\n};\n')
    refused('StaticOverMethod', '', 'SomKid::Tell is static, and the SomRoot::Tell it hides is not',
            top='class SomRoot : public AActor {\npublic:\n  int32 Tell(int32 V) { return V + 1; }\n};\n'
                'class SomKid : public SomRoot {\npublic:\n  static int32 Tell(int32 V) { return V * 3; }\n};\n')
    kid = asset('FuncStaticHide')
    root = os.path.join(os.path.dirname(kid), 'FshRoot')
    for b in (kid, root): keeps_invariants(b)
    pkg = invariants.Package(kid)
    st = pkg.struct(pkg.find('Tell'))
    assert st.super and pkg.path(st.super).endswith('/FshRoot.FshRoot_C:Tell'), 'FuncStaticHide::Tell has no super'
    assert st.function_flags & 0x2000, 'FuncStaticHide::Tell FunctionFlags %#x is not static' % st.function_flags
    assert run_as([kid, root], 'Use', {}, V=2) == 603


func_static_above()
print('ok  FuncStaticHide: a function named like a mod ancestor\'s static is refused unless it is a static, whose super '
      'is that one')


def func_iface_unnamed():
    """FuncIfaceUnnamed gets an override of FiuRoot's Tell (for IFiuTell) and of Kept (for its `FiuRoot::Kept` call),
    each calling FiuRoot's though a parameter of it has no name: the override names it, as an editor override does.
    An inherited function of another signature, or a final one, cannot implement an interface function of its name,
    and the refusal says so in the interface's terms; so does one of the class's own that the inherited one's
    signature would make its super's."""
    kid = asset('FuncIfaceUnnamed')
    root = os.path.join(os.path.dirname(kid), 'FiuRoot')
    assert 'call by name' not in LOGS['FuncIfaceUnnamed'], LOGS['FuncIfaceUnnamed']
    keeps_invariants(kid)
    assert {'Tell', 'Kept'} <= set(exports_of(kid)), exports_of(kid)
    assert run_as([kid, root], 'Tell', {}, V=2) == 5
    fields = {'Seen': 0}
    assert run_as([kid, root], 'Use', fields) == 14 and fields['Seen'] == 2, fields
    refused('IfaceSigInherited', '', 'IsiKid implements IIsiTell, whose Tell is int32 (int32), and the IsiRoot::Tell it '
            'inherits is float (float)',
            top='class IIsiTell {\npublic:\n  UE_INTERFACE;\n  int32 Tell(int32 V);\n};\n'
                'class IsiRoot : public AActor {\npublic:\n  float Tell(float V) { return V; }\n};\n'
                'class IsiKid : public IsiRoot, public IIsiTell {\npublic:\n  int32 Other() { return 0; }\n};\n')
    refused('IfaceFinalInherited', '', 'the IfiRoot::Tell it inherits is final',
            top='class IIfiTell {\npublic:\n  UE_INTERFACE;\n  int32 Tell(int32 V);\n};\n'
                'class IfiRoot : public AActor {\npublic:\n  virtual int32 Tell(int32 V) final { return V; }\n};\n'
                'class IfiKid : public IfiRoot, public IIfiTell {\npublic:\n  int32 Other() { return 0; }\n};\n')
    refused('IfaceSigOwn', '', 'and replaces the IsoRoot::Tell it inherits, float (float)',
            top='class IIsoTell {\npublic:\n  UE_INTERFACE;\n  int32 Tell(int32 V);\n};\n'
                'class IsoRoot : public AActor {\npublic:\n  float Tell(float V) { return V; }\n};\n'
                'class IsoKid : public IsoRoot, public IIsoTell {\npublic:\n  int32 Tell(int32 V) { return V; }\n};\n')


func_iface_unnamed()
print('ok  FuncIfaceUnnamed: an inherited function with an unnamed parameter is forwarded; one of another signature, '
      'or final, is refused in the interface\'s terms')


def iface_over_stub_sig():
    """A mod ancestor that lists a mod interface and leaves its function out has the stub of it, and that stub is the
    parent function of any function of its name below (ParentClass->FindFunctionByName, KismetCompiler.cpp 1733-1734).
    The editor refuses an override whose parent has another signature ("Cannot override ... declared in a parent with a
    different signature", 1993-2011): an implementation of another interface's function of that name, the class's own
    or the stub of one it leaves out, is refused in the interfaces' terms."""
    two = ('class ISsA {\npublic:\n  UE_INTERFACE;\n  float Tell(float V);\n};\n'
           'class ISsB {\npublic:\n  UE_INTERFACE;\n  int32 Tell(int32 V);\n};\n'
           'class SsRoot : public AActor, public ISsA {\npublic:\n  int32 Other() { return 0; }\n};\n')
    refused('IfaceStubSigOwn', '', 'SsKid::Tell implements ISsB::Tell, int32 (int32), and replaces the Tell of ISsA, '
            'float (float), that SsRoot implements',
            top=two + 'class SsKid : public SsRoot, public ISsB {\npublic:\n  int32 Tell(int32 V) { return V * 2; }\n};\n')
    refused('IfaceStubSigStub', '', 'SsKid implements ISsB, whose Tell is int32 (int32), and replaces the Tell of ISsA, '
            'float (float), that SsRoot implements',
            top=two + 'class SsKid : public SsRoot, public ISsB {\npublic:\n  int32 Other2() { return 1; }\n};\n')


pending('FuncIfaceUnnamed: an implementation over an ancestor\'s interface stub of another signature is refused in the '
        'interfaces\' terms', iface_over_stub_sig)


def func_own_iface_final():
    """FuncOwnIfaceFinal: Tell and the stub Left implement IFoiTell, which the class itself lists, keeping the interface
    function's contract (func_override_flags) - BlueprintEvent, not Final - in a final class and in a UE_FINAL_AS base,
    and Ask's call to Tell reaches each class's own."""
    leaf = asset('FuncOwnIfaceFinal')
    p = lambda c: os.path.join(os.path.dirname(leaf), c)
    final, base = p('FoiFinal'), p('FoiBase')
    for b in (final, base, leaf): keeps_invariants(b)
    for b in (final, base):
        pkg = invariants.Package(b)
        for fn in ('Tell', 'Left'):
            got = pkg.struct(pkg.find(fn)).function_flags
            assert got & 0x08000000 and not got & 0x1, '%s::%s FunctionFlags %#x' % (os.path.basename(b), fn, got)
    assert run_as([final], 'Ask', {}, V=2) == 30
    assert run_as([leaf, base], 'Ask', {}, V=2) == 40


func_own_iface_final()
print('ok  FuncOwnIfaceFinal: an implementation of an interface a final class lists keeps the interface function\'s '
      'contract, not Final')


def func_template_call():
    """FuncTemplateCall: a member template's body is copied into each caller and read in the class it is written in.
    `AuthOnly()` in Helper is a call by name from FtKid's Use, so on an FtKid it runs FtMid's override (7), as C++ does,
    and FtKid, which declares no AuthOnly, gets no function of that name; `FuncTemplateCall::AuthOnly()` in HelperQ runs
    FuncTemplateCall's (3) on an FtQKid. The inline Plain is the same call, read the same way."""
    top = asset('FuncTemplateCall')
    p = lambda *cs: [os.path.join(os.path.dirname(top), c) for c in cs]
    mid = p('FtMid', 'FuncTemplateCall')
    kid, qkid = p('FtKid') + mid, p('FtQKid') + mid
    for b in kid[:1] + qkid[:1] + mid[:1]: keeps_invariants(b)
    assert 'AuthOnly' not in exports_of(kid[0]), exports_of(kid[0])
    for chain, fn, want in ((kid, 'Use', 7), (kid, 'UsePlain', 7), (qkid, 'UseQ', 3), (qkid, 'AuthOnly', 7)):
        fields = {'Seen': 0}
        run_as(chain, fn, fields)
        assert fields['Seen'] == want, (os.path.basename(chain[0]), fn, fields)


func_template_call()
print('ok  FuncTemplateCall: an unqualified call in a member template goes by name from each class it is copied into')


def ns_parent_call():
    """NsTest's Pistol: `Weapons::Rifle::Pull(Times)` in its own Pull runs Rifle's, however many parts the qualifier
    has (5 + 3, then + 100). By name it would be Pistol's own Pull, calling itself forever."""
    content = os.path.join(ROOT, 'NsTest', 'FSD', 'Content')
    chain = [os.path.join(content, 'NsTestAbs', 'Pistol'), os.path.join(content, '_ElytrasMods', 'NsTest', 'Weapons', 'Rifle')]
    fields = dict(Shots=5)
    got = run_as(chain, 'Pull', fields, Times=3)
    assert got == 108 and fields == dict(Shots=8), (got, fields)


ns_parent_call()
print('ok  NsTest: Weapons::Rifle::Pull() in Pistol\'s own Pull is the parent\'s, its qualifier in parts')


# ---- OPERANDS: operands the VM resolves against the object they run on - jumps, instance variables, calls by name,
# field paths, object operands, arity, out and reference arguments (invariant_rules/operands.py)

from invariant_rules import operands as operand_rules
import invariants

OPERAND_RULES = ('jump_targets_executable', 'return_only_top_level', 'ubergraph_entry_offsets', 'latent_linkage_offsets',
                 'out_param_access', 'object_operand_kinds', 'field_paths_resolve', 'instance_var_owner',
                 'call_target_owner', 'call_opcode_flags', 'call_names_resolve', 'interface_context_placement',
                 'call_args_fit_callee', 'out_args_addressable')


def operand_findings(base, only=None):
    """What invariants.py's rules, the operand rules among them, find on the package at base."""
    return invariants.check(invariants.Package(base), set(only) if only else None)


def opnd_ref_args():
    """OpndRefArgs: every lvalue form a C++ reference argument takes reaches the script callee as that variable - its
    writes land in the local, the member, the native struct's member, the array element - and an rvalue bound to a
    const reference is read as its value."""
    base = asset('OpndRefArgs')

    def fields(**kw):
        return dict(Member=kw.get('Member', 0), Spot=dict(kw.get('Spot', {'X': 0, 'Y': 0})), Slots=list(kw.get('Slots', [0, 0, 0])))

    def forms(f, V):                                     # OpndRefArgs.cpp's Forms, as Python
        f['Member'], f['Spot']['Y'], f['Slots'][1] = wrap(V + 1), wrap(V + 2), wrap(V + 3)
        return wrap(wrap(V + (5 + 1)) + wrap(wrap(V * 2) + 1))

    def swapped(f, A, B):
        f['Spot']['X'], f['Slots'][0] = f['Slots'][0], f['Spot']['X']
        return wrap(B * 1000 + A)

    n = 0
    for fn, model, cases in (('Forms', forms, [dict(V=v) for v in EDGE]),
                             ('Swapped', swapped, [dict(A=a, B=b) for a, b in ((1, 2), (-7, 40), (0, 2**31 - 1))])):
        for parms in cases:
            for start in (dict(Member=9, Spot={'X': 4, 'Y': 5}, Slots=[6, 7, 8]), dict()):
                mine, theirs = fields(**start), fields(**start)
                got, want = run(base, fn, self_vars=mine, **parms)[0], model(theirs, **parms)
                assert (got, mine) == (want, theirs), (fn, parms, start, got, want, mine, theirs)
                n += 1
    print('ok  OpndRefArgs: reference arguments reach the callee as the local, member, struct member, array element  (%d cases)' % n)
    # On another object: Virt is found on Other's class by name and runs there, Other->Member is Other's, and the
    # argument Member + X is read on this object (the arguments of a call under EX_Context run on the caller).
    for mine, theirs, x in ((3, 40, 5), (0, -2, 7)):
        vm = VM(base, {}, Member=mine)
        vm.self.vars['Other'] = vm.new(Member=theirs)
        assert vm.call('OnOther', x) == wrap((mine + x + 100) * 1000 + theirs), (mine, theirs, vm.log)
    vm = VM(base, {}, Member=1)
    vm.self.vars['Other'] = None
    assert vm.call('OnOther', 5) == -1
    print('ok  OpndRefArgs: a call by name and a member read on another object, its argument read on this one')
    # Reference arguments on another object (FillOther): each write lands in Other's member, in Other's struct's
    # member, in Other's array element - never in this object's, never in a copy. ref_params writes what the callee
    # left in a reference parameter back through its argument, as the FOutParmRec at the argument's address does.
    for v in (7, -2**31):
        vm = VM(base, {}, Member=1, Spot={'X': 2, 'Y': 3}, Slots=[4, 5])
        vm.ref_params = True
        other = vm.new(Member=10, Spot={'X': 20, 'Y': 30}, Slots=[40, 50])
        vm.self.vars['Other'] = other
        vm.call('FillOther', v)
        assert other.vars == dict(Member=v, Spot={'X': wrap(v + 1), 'Y': 30}, Slots=[wrap(v + 2), 50]), (v, other.vars)
        assert vm.self.vars == dict(Member=1, Spot={'X': 2, 'Y': 3}, Slots=[4, 5], Other=other), (v, vm.self.vars)
    vm = VM(base, {}, Member=1)
    vm.ref_params, vm.self.vars['Other'] = True, None
    vm.call('FillOther', 7)
    assert vm.self.vars == dict(Member=1, Other=None) and not vm.accessed_none, (vm.self.vars, vm.accessed_none)
    # An interface value a call returns, as a call's object: Ping runs on the object behind it, with this one's X.
    pal = Obj('OpndPinger_C', Mult=3)
    vm = VM(base, {'Ping': lambda vm, ctx, x: ctx.vars['Mult'] * x}, Pal=pal)
    assert vm.call('PingPal', 7) == 21 and vm.log == [('Ping', pal, [7])], vm.log
    vm.self.vars['Pal'] = None
    assert vm.call('PingPal', 7) == -1 and not vm.log[1:], vm.log
    print('ok  OpndRefArgs: reference arguments on another object write its member, struct member, array element; '
          'a call through a returned interface runs on its object')


def opnd_rules_hold():
    """OpndRefArgs' classes - reference arguments on this object and on another, a call and a member read under
    EX_Context, an interface value a call returns as a context - keep every operand rule: each operand resolves to what
    the VM needs of it on the object it runs on."""
    folder = os.path.dirname(asset('OpndRefArgs'))
    bases = invariants.packages([folder])
    operand_rules.OPERAND_STATS.clear()
    for b in bases:
        found = operand_findings(b, OPERAND_RULES)            # the other rules: sweep() runs them on every package
        assert not found, '%s: %s' % (os.path.basename(b), found[:3])
    stats = {r: operand_rules.OPERAND_STATS.get(r, {}) for r in ('instance_var_owner', 'out_args_addressable', 'call_names_resolve',
                                                         'interface_context_placement')}
    if SDK:     # every operand was resolved; a native callee only resolves off the dump
        assert all(s and not any('unknown' in k for k in s) for s in stats.values()), stats
    print('ok  OpndRefArgs: %d packages keep the %d operand rules%s' % (len(bases), len(OPERAND_RULES),
                                                                          ', every context and callee resolved' if SDK else ''))


def opnd_latent_ref_refused():
    """A function that resumes later cannot write through a reference parameter: the caller has returned by then, and
    the ubergraph's frame holds a copy, not the caller's variable (a write would be lost silently)."""
    refused('OpndLatentRef', '  int32 Member;\n  void WaitRef(int32 &X) {\n    UKismetSystemLibrary::Delay(1.0f);\n'
            '    X = Member;\n  }\n', 'reference parameters')
    print('ok  OpndLatentRef: a latent function taking a reference parameter is refused')


def opnd_event_ref():
    """OpndEventRef: an override of ReceiveHit, whose Hit is a const FHitResult&, with no latent call. Called as the
    engine calls it (the parameters in its frame) and from script (Poke passes a local), it reads the caller's Hit;
    the engine facts it needs: Hit an out parameter, the function FUNC_HasOutParms, and every read of Hit through
    EX_LocalOutVariable (out_param_access, with the other operand rules)."""
    base = asset('OpndEventRef')
    pkg = invariants.Package(base)
    st = pkg.struct(pkg.find('ReceiveHit'))
    hit = next(p for p in st.props if p.name == 'Hit')
    assert hit.flags & 0x180 == 0x180 and st.function_flags & 0x400000, (hex(hit.flags), hex(st.function_flags))
    found = operand_findings(base, OPERAND_RULES)
    assert not found, found[:3]
    vm = VM(base, {})
    vm.call('ReceiveHit', None, None, None, False, None, None, None, {'Time': 0.25, 'Distance': 7.5})
    assert (vm.self.vars.get('HitTime'), vm.self.vars.get('HitDistance')) == (0.25, 7.5), vm.self.vars
    for t in (0.25, 1.5, -3.0):
        vm = VM(base, {})
        assert vm.call('Poke', t) == t * 3 and vm.self.vars.get('Seen') == t * 3, (t, vm.self.vars)
    print('ok  OpndEventRef: an event override reads its const-reference parameter as the engine and a script caller pass it')


opnd_ref_args()
opnd_rules_hold()
opnd_event_ref()
opnd_latent_ref_refused()


def opnd_callspace():
    """OpndCallspace: ApplyDamage (BlueprintAuthorityOnly) and PlaySound2D (BlueprintCosmetic) are called so the engine
    asks their callspace - through CallFunction on the library's CDO, never EX_CallMath - with the arguments given."""
    base = asset('OpndCallspace')
    paths = import_paths(base)
    for fn in ('ApplyDamage', 'PlaySound2D', 'Abs'):
        assert any(p.endswith(':' + fn) for p in paths), (fn, paths)
    vm = VM(base, {})
    target = vm.new()
    vm.call('Damage', target)
    vm.call('Sound')
    # PlaySound2D's world context, which the C++ leaves out, is this object.
    assert [(n, list(a)) for n, _, a in vm.log] == [('ApplyDamage', [target, 2.0, None, vm.self, None]),
                                                     ('PlaySound2D', [vm.self, None, 0.5, 1.0, 0.0, None, None, False])], vm.log
    found = operand_findings(base, ['call_opcode_flags'])
    assert not found, found[0][2]


def opnd_latent_hit():
    """OpndLatentHit: a latent call in an override of ReceiveHit, whose Hit is a const FHitResult&: the stub hands
    Hit to the ubergraph through EX_LocalOutVariable, and the body sees Hit's values before and after the Delay."""
    base = asset('OpndLatentHit')
    keeps_invariants(base)
    vm = VM(base, {'Delay': latent_call})
    vm.call('ReceiveHit', None, None, None, False, None, None, None, {'Time': 0.25, 'Distance': 7.5})
    assert vm.self.vars.get('HitTime') == 0.25 and 'HitDistance' not in vm.self.vars, vm.self.vars
    vm.fire(0)
    assert vm.self.vars.get('HitDistance') == 7.5, vm.self.vars


def opnd_dispatch_ref_refused():
    """A dispatcher whose signature takes a non-const reference: C++'s Broadcast copies each reference parameter back
    after the handlers ran (UHT's delegate wrapper), but execCallMulticastDelegate copies the argument into its own
    parameter block and never back (ScriptCore.cpp 3032-3075), so the caller would silently keep its old value -
    Fire(10) returns 10 cooked, 15 in C++. Nothing in the VM can write it back: the compiler must refuse it."""
    refused('OpndDispatchRef', '  UE_DISPATCHER(OnRef, int32 &Count, int32 Plain);\n'
            '  void  Add(int32 &Count, int32 Plain) { Count += Plain; }\n'
            '  int32 Fire(int32 V) {\n    OnRef.Add(this, &OpndDispatchRef::Add);\n    int32 C = V;\n'
            '    OnRef.Broadcast(C, 5);\n    return C;\n  }\n', 'OnRef.Broadcast: Count is a non-const reference, which a Broadcast '
            'never writes back')
    print('ok  OpndDispatchRef: a Broadcast with a non-const reference parameter is refused')


opnd_dispatch_ref_refused()
opnd_callspace()
print('ok  OpndCallspace: an authority-only / cosmetic static is called through a context, so GetFunctionCallspace '
      'can absorb it')
opnd_latent_hit()
print('ok  OpndLatentHit: a latent call in an event override taking a const reference (ReceiveHit), the stub copying '
      'it into the frame through EX_LocalOutVariable')


# ---- TYPING: literals, assignments, struct members, casts, returns, container literals, interface casts
# (invariant_rules/operand_types.py)

import invariants
from invariant_rules import operand_types as typing_rules


def typing_resolved(base, fn, what):
    """(op, destination Ty) of each assignment in fn, as the typing rules resolve it: a rule that cannot type a
    position does not check it, so a test that relies on a rule first makes sure the position was typed."""
    pkg = invariants.Package(base)
    i = pkg.find(fn)
    T, nodes = typing_rules.nodes_of(pkg, i)
    out = [(n.op, T.of(n.kids[0])) for n, ctx in nodes if n.op in typing_rules.LETS]
    assert out and all(t is not None for op, t in out), (what, out)
    return out


def typing_lits():
    """TypingLits: each literal writes its receiver's type - members, script and native parameters, a return - and
    the two native bitfield bools are written with the op that masks (the typing_* rules, over the whole package)."""
    base = asset('TypingLits')
    keeps_invariants(base)
    f = {}
    run(base, 'Fill', self_vars=f)
    assert f == dict(B=200, P=2, F=True, W=5000000000, R=2.5, N='tag', S='s', T='t', I=7), f
    vm = VM(base)
    vm.call('FillVector')
    assert list(vm.self.vars['V']) == [1.0, 2.0, 3.0], vm.self.vars
    print('ok  TypingLits.Fill / FillVector: a byte, an enum, a bool, an int64, a float, a name, a string, a text, a vector')
    check('TypingLits', 'Calls', lambda: 44 - 2 + 1 + 10 + 3 + 2, [dict()])
    check('TypingLits', 'Natives', lambda X: 250 + X, [dict(X=x) for x in (0, 5, 255)])
    vm = VM(base)
    vm.call('Hide')
    assert vm.self.vars == {'bHidden': True, 'bCanBeDamaged': False}, vm.self.vars
    if SDK:     # the typing of a native member: only the dump describes AActor's and FHitResult's bitfields
        kinds = typing_resolved(base, 'Hide', 'Hide')
        assert [t.bitfield for op, t in kinds] == [True, True], kinds
    print('ok  TypingLits.Hide: AActor\'s bitfield bools bHidden / bCanBeDamaged, written without clobbering their byte')
    vm = VM(base)
    vm.call('Flags')
    assert vm.self.vars == {'Hit': {'bStartPenetrating': True, 'bBlockingHit': False, 'Time': 0.5}}, vm.self.vars
    if SDK:
        kinds = typing_resolved(base, 'Flags', 'Flags')
        assert [(t.cls, t.bitfield) for op, t in kinds] == [('BoolProperty', True), ('BoolProperty', True), ('FloatProperty', False)], kinds
    print('ok  TypingLits.Flags: FHitResult\'s bitfield bools bStartPenetrating / bBlockingHit, members of a member, '
          'written without clobbering their byte')
    for n in (0, 4):
        f = {'Touched': n}
        assert run(base, 'Relay', self_vars=f)[0] is None and f == {'Touched': n + 1}, f
    print('ok  TypingLits.Relay: `return Touch();` in a void function runs the call and returns nothing')


def typing_sets():
    """TypingSets: container literals fill their variables with elements of the inner / key / value type."""
    base = asset('TypingSets')
    keeps_invariants(base)
    f = {}
    run(base, 'Fill', self_vars=f)
    assert f == {'Seen': [1, 2, 3], 'Rates': {1: 0.5, 2: 1.5}, 'Bytes': [7, 250]}, f
    vm = VM(base)
    vm.call('FillPoints')
    assert [list(p) for p in vm.self.vars['Points']] == [[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]], vm.self.vars
    assert run(base, 'Second', self_vars={'Bytes': [7, 250]})[0] == 250
    print('ok  TypingSets: a set, a map, a byte array and a vector array from literals; an element read in place')


def typing_iface():
    """TypingIface: object <-> interface casts on objects that implement both interfaces, one, or neither."""
    base = asset('TypingIface')
    keeps_invariants(base)
    implements = {'TypingIface_C': {'ITypingMark_C', 'ITypingMore_C', 'Actor', 'Object'},
                  'Half_C': {'ITypingMark_C', 'Actor', 'Object'}, 'Plain_C': {'Actor', 'Object'}}
    isa = lambda o, cls: isinstance(o, Obj) and cls in implements.get(o.cls, ())
    vm = VM(base, natives={'Mark': lambda vm, ctx: 5}, isa=isa)
    n = 0
    for a in (vm.new(), Obj('Half_C'), Obj('Plain_C'), None):
        impl = implements.get(a.cls, set()) if a is not None else set()
        mark = 'ITypingMark_C' in impl
        assert vm.call('Has', a) == mark, ('Has', a)
        assert vm.call('Back', a) is (a if mark else None), ('Back', a)
        assert vm.call('Cross', a) == ('ITypingMore_C' in impl), ('Cross', a)
        vm.call('Keep', a)
        assert vm.self.vars['Held'] is (a if mark else None), ('Keep', a, vm.self.vars)
        assert vm.call('MarkOf', a) == {'TypingIface_C': 3, 'Half_C': 5}.get(a.cls if mark else None, -1), ('MarkOf', a)
        n += 5
    print('ok  TypingIface: object -> interface, interface -> interface, interface -> object, (bool) of one  (%d cases)' % n)


def typing_refusals():
    """`return (void)<value>;` in a void function evaluates the value and returns, as C++ does. A value that can do
    nothing is dropped, so none is ever stepped into the null result a function with no return value has
    (ScriptCore.cpp 1123-1133); the plain return still leaves the function there."""
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'TypingVoidCast.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/TypingVoidCast");\n'
                    'class TypingVoidCast : public AActor {\npublic:\n  int32 N;\n'
                    '  void Cast5() { N = 1; return (void)5; N = 2; }\n'
                    '  void CastVar() { N = 7; return (void)N; N = 8; }\n};\n')
        proc = assetgen_compile([src, UEAPI, tmp])
        assert proc.returncode == 0, proc.stdout + proc.stderr
        for fn, want in (('Cast5', 1), ('CastVar', 7)):
            me = {'N': 0}
            run(os.path.join(tmp, 'TypingVoidCast'), fn, self_vars=me)
            assert me['N'] == want, (fn, me)
    print('ok  `return (void)5;` / `return (void)N;` in a void function return there, no value stepped')


def typing_arr_null():
    base = asset('TypingArrNull')
    vm = VM(base)
    vm.call('Poke')
    assert vm.self.vars.get('Done') == 1, vm.self.vars
    keeps_invariants(base)


typing_lits()
typing_sets()
typing_iface()
typing_refusals()
typing_arr_null()
print('ok  TypingArrNull: Array_Add on a container of a null object is called inside EX_Context on the library '
      'default object, so the failed thunk is skipped, not EX_CallMath')


# ---- VMSEM: what the VM does at run time with a value the bytecode hands it - a None context's r-value, a statement's
# discarded result, a native's out array, a struct constant's members, a function's constructed locals, an interface
# cast's slot (invariant_rules/vm_semantics.py)

import invariants
from invariant_rules import vm_semantics as vmsem_rules


def vmsem_holds(base, *names, fn=None):
    """The package at base keeps invariants rules `names` - in function `fn` only, when given."""
    found = [f for f in invariants.check(invariants.Package(base), only=set(names)) if fn is None or f[1] == fn]
    assert not found, '; '.join('%s %s: %s' % f for f in found[:3])


def vmsem_dump_has(*paths):
    """Whether the Dumper-7 dump (sdkinfo.py) knows these /Script structs and interfaces. Without it the rules skip
    native operands, and a check that only runs a rule would pass for want of anything to check: with --sdk a missing
    one fails, without it the caller skips the rule and this says so."""
    missing = [p for p in paths if vmsem_rules.struct_members(None, p) is None and not vmsem_rules.sdkinfo.is_interface(p)]
    if missing and not SDK:
        print('--  the rule on %s: skipped (needs --sdk: what the engine links there is read off the dump)' % ', '.join(missing))
        return False
    assert not missing, 'no Dumper-7 dump of %s (sdkinfo.py): the rule cannot see what the engine links there' % missing
    return True


def ctx_null_reads():
    """A member read through a None object reads zero: the context's r-value names the member, and ProcessContextOpcode
    clears it in the destination (ScriptCore.cpp 2940-2953) - a Let's variable, a return value, either link of a chain.
    A discarded int result is a harmless statement."""
    base = asset('CtxNullRead')
    vm = VM(base, Score=5, Calls=0, Seen=3, Peer=None)
    vm.null_rvalues = True
    far = vm.new(Score=9, Peer=None)
    near = vm.new(Score=4, Peer=far)
    for p, want in ((None, 0), (near, 4), (far, 9)):
        got = vm.call('FieldOf', p)
        assert got == want, ('FieldOf', p, got, want)
    for p, want in ((None, 0), (far, 0), (near, 9)):
        got = vm.call('ChainOf', p)
        assert got == want, ('ChainOf', p, got, want)
    vm.call('Remember')
    assert vm.self.vars['Seen'] == 0, vm.self.vars
    vm.self.vars['Peer'] = near
    vm.call('Remember')
    assert vm.self.vars['Seen'] == 4, vm.self.vars
    vm.call('Touch')
    assert vm.self.vars['Calls'] == 2, vm.self.vars
    vmsem_holds(base, *(set(vmsem_rules.VMSEM_RULES) - set(KNOWN_RULES)))
    print('ok  CtxNullRead: a member read through None reads zero, in a Let, a return and a chain; a discarded int result runs')


def struct_link_order():
    """A struct literal with no super, and one whose members all come from its super, list their members in
    PropertyLink order, which is the order C++ declares them: what the VM reads back is what the constructor was given."""
    base = asset('StructLinkOrder')
    vm = VM(base)
    vm.call('Fill')
    assert vm.self.vars['Aim'] == [1.0, 2.0, 3.0] and vm.self.vars['Tint'] == [0.25, 0.5, 0.75, 1.0], vm.self.vars
    vm.call('Clear')
    assert vm.self.vars['Aim'] == [0.0] * 3 and vm.self.vars['Tint'] == [0.0] * 4, vm.self.vars
    vmsem_holds(base, 'struct_const_members')
    print('ok  StructLinkOrder: FVector_NetQuantize and FLinearColor literals follow PropertyLink (super-only, no super)')


ctx_null_reads()
struct_link_order()


def ctx_null_call():
    """A call through a None object, used as a value, zeroes its destination: the context names the Let's destination
    as its r-value (KismetCompilerVMBackend.cpp 1241-1244) and ProcessContextOpcode clears it (ScriptCore.cpp 2950-2953)."""
    base = asset('CtxNullCall')
    vm = VM(base, Peer=None, Got=7)
    vm.null_rvalues = True
    vm.call('Read')
    assert vm.self.vars['Got'] == 0, 'Read on a None Peer leaves Got = %r, not 0' % (vm.self.vars['Got'],)
    got = vm.call('ReadLocal', None)
    assert got == 0, 'ReadLocal(None) = %r, not 0' % (got,)
    peer = vm.new(Peer=None, Got=0)
    vm.self.vars['Peer'] = peer
    vm.call('Read')
    assert vm.self.vars['Got'] == 5 and vm.call('ReadLocal', peer) == 5, vm.self.vars
    vmsem_holds(base, 'context_rvalue')


def drop_result():
    """A call whose FString / TArray result a statement throws away needs a local to land in: the statement buffer is
    64 raw bytes nothing constructs or destroys (ScriptCore.cpp 1058, 1120). The calls still run, each once."""
    base = asset('DropResult')
    vmsem_holds(base, 'discarded_result_fits_scratch')
    vm = VM(base, Name='n', Items=[1], Calls=0)
    vm.call('Run')
    assert vm.self.vars['Calls'] == 111, vm.self.vars


def out_array_reset():
    """A native's out TArray arrives empty (KismetCompilerVMBackend.cpp 1152-1174), so a native written against that
    contract - modelled here by one that only appends - leaves exactly what it found."""
    base = asset('OutArrayReset')
    found = Obj('Found_C')

    def appends(vm, ctx, wco, cls, out):         # a native written against the editor's contract: it only appends
        out.append(found)
    vm = VM(base, {'GetAllActorsOfClass': appends}, Found=[])
    vm.call('Run')
    assert vm.self.vars['Found'] == [found], 'Found = %r, not [what the call found]' % (vm.self.vars['Found'],)
    vmsem_holds(base, 'native_out_arrays_emptied')


def set_to_array_append():
    """The engine's own appending native: GenericSet_ToArray adds each element onto whatever Result holds
    (BlueprintSetLibrary.cpp 53-70), runscript's model below does the same for this test, and the editor empties Result
    first. So ToArray leaves exactly the set, and a range-for over a set in an outer loop sees it once per round."""
    base = asset('SetToArrayAppend')
    set_to_array_runs(base)
    vmsem_holds(base, 'native_out_arrays_emptied')


def set_to_array_runs(base):
    real = runscript.CONTAINERS['Set_ToArray']
    runscript.CONTAINERS['Set_ToArray'] = lambda ev, store, a: runscript._made(ev, store, a[1], []).extend(
        copy.deepcopy(list(runscript._made(ev, store, a[0], []))))
    try:
        mine = dict(Picks=[1, 2], Listed=[9])
        run(base, 'List', self_vars=mine)
        assert mine['Listed'] == [1, 2], 'ToArray leaves Listed = %r, not the set [1, 2]' % (mine['Listed'],)
        got = run(base, 'SumTwice', self_vars=dict(Picks=[1, 2]))[0]
        assert got == 6, 'SumTwice over {1, 2} = %r, not 6: the second round walks the first round\'s copy too' % (got,)
    finally:
        runscript.CONTAINERS['Set_ToArray'] = real


def no_world_warning():
    """self handed to a world-context parameter from a class with no world of its own is warned about; the editor
    refuses it ("Pin must have a connection", CallFunctionHandler.cpp 547-598). An actor stays silent."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'NoWorldCtx.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/NoWorldCtx");\n'
                    'class NoWorldCtx : public UObject {\npublic:\n  APawn *Pawn;\n'
                    '  void Run() { Pawn = UGameplayStatics::GetPlayerPawn(0); }\n};\n'
                    'class NoWorldCtxActor : public AActor {\npublic:\n  APawn *Pawn;\n'
                    '  void Run() { Pawn = UGameplayStatics::GetPlayerPawn(0); }\n};\n')
        proc = assetgen_compile([src, UEAPI, tmp])
    warns = [l.strip() for l in proc.stdout.splitlines() if l.strip().startswith('warning:')]
    assert any('NoWorldCtx::Run' in w and 'world' in w.lower() for w in warns), \
        'no warning that NoWorldCtx::Run hands GetPlayerPawn a world context with no world (exit %d): %s' % (proc.returncode, warns)
    assert not any('NoWorldCtxActor' in w for w in warns), warns
    print('ok  NoWorldCtx: self passed as a world context from a class with no world is warned about; an actor is not')


def local_ctor_flags():
    """A function with a local that is not zero-constructible is FUNC_HasDefaults (KismetCompiler.cpp 2328-2335), so
    the VM constructs that local (ScriptCore.cpp 909-916): FHitResult() has Time 1, FTransform the identity scale, an
    FText a TextData to read."""
    base = asset('LocalCtorFlags')
    fns = ('BlankTime', 'ScaleX', 'TextString', 'MapCount')
    flags = lambda fn: int(re.search(r'FunctionFlags (\S+)', dump('dumpstruct.py', base, export_index(base, fn))).group(1), 16)
    missing = [fn for fn in fns if not flags(fn) & 0x800000]
    assert not missing, 'no FUNC_HasDefaults on %s, whose locals the VM must construct' % missing
    vmsem_holds(base, 'locals_constructed_have_defaults')
    for fn, want in (('BlankTime', 1.0), ('ScaleX', 1.0), ('TextString', ''), ('MapCount', 1)):
        got = run(base, fn)[0]
        assert got == want, '%s() = %r, want %r' % (fn, got, want)


def iface_cast_slot():
    """Cast<IHealth> writes a 16-byte FScriptInterface (ScriptCore.cpp 3634-3641): it must land in a 16-byte slot, and
    the `IHealth*` it gives is the object, taken out of it. A None Other is no IHealth."""
    base = asset('IfaceCastSlot')
    if vmsem_dump_has('/Script/FSD.Health'):
        vmsem_holds(base, 'interface_value_slot')
    for other, want in ((None, -7), (Obj('HealthThing'), 7)):   # every Obj here implements IHealth
        got = VM(base, isa=lambda o, cls: isinstance(o, Obj), Other=other, Guard=7).call('Probe')
        assert got == want, 'Probe() with Other %r = %r, not %r' % (other, got, want)


def derived_literal(fn, struct):
    """A native struct literal's members follow PropertyLink - the struct's own first, then its super's - and leave out
    Transient ones (ScriptCore.cpp 3376-3405; Class.cpp 944-982)."""
    if vmsem_dump_has(struct):
        vmsem_holds(asset('DerivedLiteral'), 'struct_const_members', fn=fn)


ctx_null_call()
print('ok  CtxNullCall: a call through a None object zeroes its Let destination (the context names its r-value)')
drop_result()
print('ok  DropResult: a discarded FString / TArray result lands in a local, not the 64-byte statement buffer')
out_array_reset()
print("ok  OutArrayReset: a native's out TArray is emptied just before the call (EX_SetArray), so it holds only what the "
      "call found")
set_to_array_append()
print('ok  SetToArrayAppend: ToArray and a set range-for empty the array Set_ToArray appends to')
no_world_warning()
local_ctor_flags()
print('ok  LocalCtorFlags: FUNC_HasDefaults on a function with an FHitResult / FTransform / FText / TMap local')
iface_cast_slot()
print('ok  IfaceCastSlot: Cast<IHealth> takes the object out of its 16-byte interface value')
derived_literal('Angle', '/Script/Engine.LightmassDirectionalLightSettings')
derived_literal('Output', '/Script/Engine.MaterialAttributesInput')
assert 'FMaterialAttributesInput::PropertyConnectedBitmask is Transient' in LOGS['DerivedLiteral'], LOGS['DerivedLiteral']
derived_literal('ResetHandle', '/Script/Engine.TimerHandle')
print("ok  DerivedLiteral: a derived struct literal lists its own members before its super's, and leaves out Transient "
      "ones (a value given for one is warned about); FTimerHandle() writes no member")


def struct_lit_expr():
    """A whole-struct literal whose members are not all constants - a `?:`, a `&&`, a call, a member of the variable it
    is assigned to, a value for a Transient member - is what C++ makes of it, in a local, a member variable, an argument,
    a return value, a nested struct, a TArray's elements and a UE_STRUCT's braces: the members run left to right, each
    once, and one that reads the destination reads it as it was. execStructConst steps each member straight into the
    destination the Let names (ScriptCore.cpp 2647-2686, 3376-3405), which runvm models once struct_const names the
    members."""
    import runvm
    base = asset('StructLitExpr')
    keeps_invariants(base)
    members = {'Vector2D': ['X', 'Y'], 'IntPoint': ['X', 'Y'], 'Box2D': ['Min', 'Max', 'bIsValid']}
    cases = {'Local': lambda M: 12.0 + M, 'Paren': lambda M: 21.0 + 10 * M, 'Member': lambda M: 46.0 + 10 * M,
             'Arg': lambda M: 17.0 + M, 'ArgBraced': lambda M: 71.0 + 10 * M, 'Ret': lambda M: 13.0 + 10 * M,
             'Nested': lambda M: 42.0 + M, 'Array': lambda M: 42.0 + M, 'Both': lambda M: 10 * M + (0 < M < 5),
             'Call': lambda M: 20 * M + 1, 'Order': lambda M: 110 if M == 0 else 100, 'Swap': lambda M: 21.0,
             'SwapMember': lambda M: 21.0, 'ReadBack': lambda M: 31.0, 'Mod': lambda M: 12 + M, 'ModSwap': lambda M: 21,
             'Transient': lambda M: 43 + 10 * M}
    for fn, want in cases.items():
        for m in (0, 1, 7) if fn == 'Both' else (0, 1):
            vm = VM(base, Count=0)
            vm.struct_const = lambda name, vals: runvm.Written(zip(members[name], vals))
            got = vm.call(fn, M=m)
            assert got == want(m), 'StructLitExpr.%s(%d) = %r, want %r' % (fn, m, got, want(m))


struct_lit_expr()
print('ok  StructLitExpr: a struct literal with a member that is not a constant is a Make Struct, and runs as C++ runs it')


# ---- UBER: ubergraphs along a class chain, their frames and names, latent resumes, awaits in overrides
# (invariant_rules/ubergraph.py)

import invariants
from invariant_rules import ubergraph as uber_rules

_PARAMS_OF = runscript.params_of


class ChainVM(VM):
    """runvm's VM over a class chain - package bases, most derived first - dispatching as the engine does: a call by
    name, a delegate and a latent resume find the most derived function (UObject::FindFunction, the object's own
    FuncMap first), EX_FinalFunction runs exactly the parent's function its import names, and each class's ubergraph
    has a persistent frame of its own on the object (GetPersistentUberGraphFrame keeps one per class)."""
    def __init__(s, chain, natives=None, **self_vars):
        VM.__init__(s, chain[0], natives, **self_vars)
        s.chain = [os.path.abspath(b) for b in chain]
        s.names = {b: {e['name'] for e in dumpexp.load(b)[5]} for b in s.chain}
        s.exports = set().union(*s.names.values())
        s.cache, s.ran, s.finals, s.pin = {}, [], {}, None
        key = lambda b: os.path.normcase(os.path.abspath(b))
        for b in s.chain:                                  # a final call on a function of a class up the chain
            pkg = invariants.Package(b)
            for k, imp in enumerate(pkg.imports):
                r = pkg.resolve(-k - 1) if imp['class_name'] == 'Function' else None
                if r and key(r[0].base) in map(key, s.chain):
                    s.finals[b, imp['name']] = [key(c) for c in s.chain].index(key(r[0].base))

    def where(s, fn):
        """(base, name) of the function a call of fn reaches: fn@k is the chain's k-th class's; a bare name the most
        derived one declaring it."""
        if '@' in fn:
            name, k = fn.rsplit('@', 1)
            return s.chain[int(k)], name
        return next(b for b in s.chain if fn in s.names[b]), fn

    def script(s, fn):
        if s.pin and s.pin[1] == fn: (b, name), s.pin = s.pin, None     # the call below runs a pinned final one
        else: b, name = s.where(fn)
        if (b, name) not in s.cache:
            stmts = runscript.script_of(b, name)
            stack = list(stmts)
            while stack:                                   # EX_FinalFunction on a parent's import: exactly that one
                n = stack.pop()
                stack += n.kids
                if n.op in (0x1C, 0x46) and not n.own and (b, n.val) in s.finals:
                    n.val = '%s@%d' % (n.val, s.finals[b, n.val]); s.exports.add(n.val)
            s.cache[b, name] = (stmts, {n.mem: i for i, n in enumerate(stmts)}, _PARAMS_OF(b, name))
        return s.cache[b, name]

    def frame(s, me, uber):
        return s.frames.setdefault((id(me), s.where(uber)[0], uber), {})

    def call(s, fn, *args, on=None, **parms):
        b, name = s.where(fn)
        s.ran.append((b, name))
        s.pin = (b, name) if '@' in fn else None
        before = runscript.params_of                        # VM.call reads a callee's reference parameters by name
        runscript.params_of = lambda base, f, *flag: _PARAMS_OF(*s.where(f), *flag)
        try:
            return VM.call(s, name, *args, on=on, **parms)
        finally:
            runscript.params_of = before


def class_export(pkg):
    return next(ci for ci, st in invariants.classes(pkg))


def uber_name(pkg):
    """The name of the function the class's UberGraphFunction tag names."""
    ci = class_export(pkg)
    t = pkg.tag(ci, 'UberGraphFunction')
    return pkg.exports[int.from_bytes(t['value'], 'little', signed=True) - 1]['name']


def latent_uuids(pkg):
    """The UUID of every LatentActionInfo literal in the package's functions."""
    out = []
    for i, st in invariants.functions(pkg):
        for n in invariants.statements(pkg, i)[0]:
            if n.op == 0x2F and pkg.path(n.ops[0][1]) == '/Script/Engine.LatentActionInfo':
                out.append(n.kids[1].ops[0][1])
    return out


def uber_chain_frames():
    """UberChain: on one UberChainKid, WaitA (the child's, which runs the parent's first) and WaitB leave three
    actions pending: the parent's Delay, the child's WaitA Delay, WaitB's. Each resumes - by name, found on the object
    as FindFunction finds it - in the ubergraph of the class that made the call, and reads back the local it computed
    before the wait from its own class's frame, whichever fires first: the two WaitA segments' L share a name but not
    a frame. Each class keeps its own ubergraph and UberGraphFrame (the uber_* rules), and the calls' UUIDs differ,
    so no action drops another."""
    folder = os.path.dirname(asset('UberChain'))
    base, kid = asset('UberChain'), os.path.join(folder, 'UberChainKid')
    for b in (base, kid): keeps_invariants(b)
    assert uber_name(invariants.Package(base)).lower() != uber_name(invariants.Package(kid)).lower()
    seconds = {11: 0.1, 13: 0.3, 12: 0.2}                 # the value each action logs, by its Delay
    for order in ((0, 0, 0), (2, 1, 0), (1, 1, 0)):   # indices into the shrinking pending list
        vm = ChainVM([kid, base], {'Delay': latent_call}, Stage=10, Log=[])
        vm.call('WaitA')
        vm.call('WaitB')
        waits = [round(l[2][1], 4) for l in vm.log if l[0] == 'Delay']
        assert waits == [0.1, 0.3, 0.2] and len(vm.latent) == 3, (vm.latent, vm.log)
        resumes = [vm.where(info[2])[0] for _, _, info, _ in vm.latent]   # FindFunction(ExecutionFunction) on the kid
        assert resumes == [vm.chain[1], vm.chain[0], vm.chain[0]] and all(info[3] is vm.self for _, _, info, _ in vm.latent), vm.latent
        vm.self.vars['Stage'] = 50                        # a local recomputed after the wait would show
        fired = []
        for i in order:
            fired.append(waits.pop(i))
            vm.fire(i)
        assert vm.self.vars['Log'] == [next(v for v, s in seconds.items() if s == f) for f in fired] and not vm.latent, \
            (order, fired, vm.self.vars)
    vm = ChainVM([base], {'Delay': latent_call}, Stage=1, Log=[])          # and on a plain UberChain
    vm.call('WaitA')
    vm.fire()
    assert vm.self.vars['Log'] == [2], vm.self.vars
    print('ok  UberChain: a parent\'s and a child\'s waits on one object each resume in their own class\'s ubergraph and frame')


def uber_same_leaf():
    """UberChain's UberA::Gun and UberB::Gun: one leaf name, two folders, the second the first's child. Their
    ubergraphs are two names (FName-compared, as FindFunction does), neither a function of the other class, their
    latent UUIDs are disjoint, and on one UberB::Gun both waits resume, each in its own class's ubergraph with the
    parameter it copied into its own frame."""
    folder = os.path.dirname(asset('UberChain'))
    a, b = os.path.join(folder, 'UberA', 'Gun'), os.path.join(folder, 'UberB', 'Gun')
    pa, pb = invariants.Package(a), invariants.Package(b)
    for p in (pa, pb): keeps_invariants(p.base)
    assert pb.path(pb.struct(class_export(pb)).super) == '/Game/_ElytrasMods/UberChain/UberA/Gun.Gun_C'
    ua, ub = uber_name(pa), uber_name(pb)
    fa = {n.lower() for n, v in pa.struct(class_export(pa)).func_map}
    fb = {n.lower() for n, v in pb.struct(class_export(pb)).func_map}
    assert ua.lower() != ub.lower() and ub.lower() not in fa and ua.lower() not in fb, (ua, ub, fa, fb)
    assert latent_uuids(pa) and latent_uuids(pb) and not set(latent_uuids(pa)) & set(latent_uuids(pb))
    for order in ((0, 0), (1, 0)):
        vm = ChainVM([b, a], {'Delay': latent_call}, Hits=0, Kicks=0)
        vm.call('Fire', 2)
        vm.call('Kick', 3)
        assert len(vm.latent) == 2 and [vm.where(info[2])[0] for _, _, info, _ in vm.latent] == [vm.chain[1], vm.chain[0]], vm.latent
        for i in order: vm.fire(i)
        assert (vm.self.vars['Hits'], vm.self.vars['Kicks']) == (2, 3) and not vm.latent, (order, vm.self.vars)
    print('ok  UberChain: UberA::Gun / UberB::Gun keep two ubergraphs and UUID sets; each wait resumes in its own')


def uber_await_kid():
    """UberAwaitKid: the parent's Fetch, run on an UberAwaitKid by the override's UberAwaitBase::Fetch, binds its
    task's OnSuccess by name on self. The name resolves on the child first, so it must be no function of the child:
    the parent's download completes into the parent (Image, then Base = 1), the child's into the child (Kid = 1)."""
    kid = asset('UberAwaitKid')
    base = os.path.join(os.path.dirname(kid), 'UberAwaitBase')
    pk, pb = invariants.Package(kid), invariants.Package(base)
    bound = {n.ops[0][1] for i, st in invariants.functions(pb) for n in invariants.statements(pb, i)[0] if n.op == 0x4B}
    mine = {n.lower() for n, v in pk.struct(class_export(pk)).func_map}
    assert bound and not {b.lower() for b in bound} & mine, \
        'UberAwaitKid declares %s, which UberAwaitBase binds by name' % sorted(b for b in bound if b.lower() in mine)
    made = []
    natives = {'DownloadImage': lambda vm, ctx, *a: made.append(Obj('AsyncTaskDownloadImage', args=a)) or made[-1]}
    vm = ChainVM([kid, base], natives)
    vm.call('Fetch', 'http://x')
    assert len(made) == 2 and made[0].vars['args'] == ('http://x',), made
    vm.broadcast(made[0], 'OnSuccess', 'tex')
    assert (vm.self.vars.get('Image'), vm.self.vars.get('Base'), vm.self.vars.get('Kid')) == ('tex', 1, None), vm.self.vars
    vm.broadcast(made[1], 'OnSuccess', 'tex2')
    assert vm.self.vars.get('Kid') == 1 and vm.self.vars.get('Image') == 'tex', vm.self.vars


def uber_defer_guard():
    """UberDeferGuard with the engine faked: each deferred actor or component is finished once, the spawn's own
    result, and only when there is one - behind a null test, from another event, at another transform - and the
    package keeps every rule: correct code the editor's nodes never write is what shows a finish rule is no stricter
    than the engine (at most one finish, Actor.cpp 3206)."""
    base = asset('UberDeferGuard')
    keeps_invariants(base)
    where, there, zero = ('xf', 1), ('xf', 2), ('xf', (0.0, 0.0, 0.0))

    def run(fn, *args, gives=True):
        made = []
        natives = {'Conv_VectorToTransform': lambda vm, ctx, v: ('xf', tuple(v)),
                   'BeginDeferredActorSpawnFromClass': lambda vm, ctx, *a: (made.append(Obj(a[1], args=a)) or made[-1]) if gives else None,
                   'AddComponentByClass': lambda vm, ctx, *a: (made.append(Obj(a[0], args=a)) or made[-1]) if gives else None,
                   'FinishSpawningActor': lambda vm, ctx, a, xf: a}
        vm = VM(base, natives)
        vm.call(fn, *args)
        return vm, vm.self, made, [l for l in vm.log if l[0] not in ('Conv_VectorToTransform', 'IsValid')]

    for fn, tag in (('GuardIf', 1), ('GuardReturn', 2)):
        vm, me, made, calls = run(fn, where)
        begin = ('BeginDeferredActorSpawnFromClass', me, [me, 'UberDeferGuard_C', where, 0, None])
        assert calls == [begin, ('set', made[0], 'Tag'), ('FinishSpawningActor', me, [made[0], where])], (fn, calls)
        assert made[0].vars['Tag'] == tag
        vm, me, made, calls = run(fn, where, gives=False)                 # no actor: nothing set, nothing finished
        assert calls == [('BeginDeferredActorSpawnFromClass', me, [me, 'UberDeferGuard_C', where, 0, None])], (fn, calls)
    vm, me, made, calls = run('Moved', where, there)                    # begun at one transform, finished at another
    assert calls == [('BeginDeferredActorSpawnFromClass', me, [me, 'UberDeferGuard_C', where, 0, None]),
                     ('set', made[0], 'Tag'), ('FinishSpawningActor', me, [made[0], there])], calls
    vm, me, made, calls = run('Begin', where)                           # begun by one event, finished by another
    vm.call('Finish', there)
    assert vm.self.vars['Pending'] is made[0] and [l for l in vm.log if l[0] == 'FinishSpawningActor'] == [
        ('FinishSpawningActor', me, [made[0], there])], vm.log
    vm, me, made, calls = run('CompGuard')
    assert calls == [('AddComponentByClass', me, ['SceneComponent', False, zero, True]), ('set', made[0], 'bHiddenInGame'),
                     ('FinishAddComponent', me, [made[0], False, zero]), ('set', me, 'Part')], calls
    vm, me, made, calls = run('CompGuard', gives=False)
    assert calls == [('AddComponentByClass', me, ['SceneComponent', False, zero, True]), ('set', me, 'Part')], calls
    assert vm.self.vars['Part'] is None
    print('ok  UberDeferGuard: deferred spawns and adds finished once - guarded, from another event, elsewhere - keep the rules')


UBER_SHADOW_TOP = ('class UberShadowBase : public AActor {\npublic:\n  int32 N;\n'
                   '  void Wait() { UKismetSystemLibrary::Delay(0.1f); N = 1; }\n};\n'
                   'class UberShadowKid : public UberShadowBase {\npublic:\n  int32 M;\n'
                   '  void ExecuteUbergraph_UberShadowBase(int32 EntryPoint) { M = EntryPoint; }\n};\n')


uber_chain_frames()
uber_same_leaf()
uber_defer_guard()
uber_await_kid()
print('ok  UberAwaitKid: a parent\'s await in an overridden method resumes in the parent on a child object')
# A child method named like its parent's ubergraph would catch the parent's latent resumes (FindFunction, most
# derived first): refused, like a method named like the class's own ubergraph.
refused('UberShadow', '  int32 X;\n', 'UberShadowKid::ExecuteUbergraph_UberShadowBase: ExecuteUbergraph_<Class> is the name '
        'of a class\'s ubergraph', top=UBER_SHADOW_TOP)
print('ok  UberShadow: a method named like an ubergraph is refused')


# ---- DELEG: delegates and event dispatchers - signatures, binds, broadcasts, timers by name (invariant_rules/delegates.py)

import invariants
from invariant_rules import delegates as deleg_rules

DELEG_RULES = {'dispatcher_signature_export', 'delegate_property_signature', 'delegate_signature_not_called',
               'multicast_operand_is_dispatcher', 'broadcast_matches_signature', 'delegate_bind_matches_signature',
               'timer_by_name_resolves'}


def deleg_rules_hold():
    """Every package built here keeps the DELEG rules: each dispatcher names its own FUNC_Delegate signature, no call
    reaches a signature, every multicast opcode works on a dispatcher variable, every broadcast passes its signature's
    parameters, and every function bound by name is found on the bound object's class with the signature's parameter
    chain. (test_bytecode.py's sweep() runs them over every suite package once they are in invariants.py.)"""
    bases = invariants.packages([ROOT])
    found = [(os.path.relpath(b, ROOT), *f) for b in bases for f in invariants.check(invariants.Package(b), only=DELEG_RULES)]
    assert not found, '\n'.join('%s  %s %s: %s' % f for f in found[:20])
    print('ok  %d packages keep the %d DELEG rules' % (len(bases), len(DELEG_RULES)))


def dispatch_facts():
    """What a Blueprint binding to DispatchRuns sees: each UE_DISPATCHER is a MulticastInlineDelegateProperty whose
    SignatureFunction is the class's own <Name>__DelegateSignature, FUNC_Delegate, with the declared parameters - a
    by-value struct as CPF_Parm, a const reference as CPF_Parm | CPF_OutParm | CPF_ReferenceParm - and every handler
    takes exactly that chain."""
    pkg = invariants.Package(asset('DispatchRuns'))
    ci = next(i for i, st in invariants.classes(pkg))
    disp = {p.name: p for p in pkg.struct(ci).props if p.type.endswith('DelegateProperty')}
    assert sorted(disp) == ['OnNote', 'OnPing', 'OnScore'], disp
    sigs = {}
    for name, p in disp.items():
        e = pkg.exports[p.ref - 1] if p.ref > 0 else None
        assert p.type == 'MulticastInlineDelegateProperty' and e and e['name'] == name + '__DelegateSignature' \
            and e['outer'] == ci + 1 and pkg.struct(p.ref - 1).function_flags & 0x00100000, (name, p.type, e)
        sigs[name] = [(q.type, q.name, q.flags & 0x8000180, pkg.path(q.ref) if q.ref else None)
                      for q in deleg_rules.parm_list(pkg, p.ref - 1)]
    note = '/Game/_ElytrasMods/DispatchRuns/FDispatchNote.FDispatchNote'
    assert sigs == {'OnScore': [('IntProperty', 'Points', 0x80, None), ('ObjectProperty', 'By', 0x80, '/Script/Engine.Actor')],
                    'OnNote': [('StructProperty', 'Note', 0x80, note), ('StructProperty', 'Seen', 0x8000180, note)],
                    'OnPing': []}, sigs
    for handler, name in (('HandleScore', 'OnScore'), ('HandleDouble', 'OnScore'), ('HandleNote', 'OnNote'),
                          ('HandlePing', 'OnPing')):
        why = deleg_rules.mismatch((pkg, pkg.find(handler)), ('disk', pkg, disp[name].ref - 1))
        assert why is None, (handler, why)
    # As the editor makes a dispatcher's signature (all 166 in every third game package): BlueprintEvent |
    # BlueprintCallable | Delegate | Public, no super function, and a body that does nothing when run.
    vm = VM(asset('DispatchRuns'), {}, Total=0, Pings=0)
    for name, p in disp.items():
        sig = pkg.struct(p.ref - 1)
        assert sig.function_flags & 0x0C120000 == 0x0C120000 and sig.super == 0, (name, hex(sig.function_flags), sig.super)
        args = [(1, vm.self), ({'Points': 1, 'Tag': 'x'},) * 2, ()][['OnScore', 'OnNote', 'OnPing'].index(name)]
        assert vm.call(name + '__DelegateSignature', *args) is None
    assert vm.self.vars == dict(Total=0, Pings=0) and not vm.log and not vm.binds, (vm.self.vars, vm.log, vm.binds)
    print('ok  DispatchRuns: each dispatcher names its own FUNC_Delegate signature with the declared parameters, '
          'which every handler shares; the signature is a callable, public, parentless stub')


def dispatch_runs():
    """DispatchRuns as the VM runs it. Add binds (object, name) once - AddUnique - Remove drops that binding, Clear
    drops all, Broadcast runs each bound handler on its object with the arguments (a struct by value and by const
    reference among them); with nothing bound it does nothing. A binding on another object's dispatcher runs when
    that object broadcasts, on the object that bound it."""
    base = asset('DispatchRuns')

    def fresh():
        return VM(base, {}, Total=0, Pings=0)
    for setup, fire, total in ((['AddBoth'], 5, 15), (['AddTwice'], 3, 3), (['AddBoth', 'RemoveScore'], 4, 8),
                               (['AddBoth', 'ClearScore'], 4, 0), (['AddTwice', 'RemoveScore'], 4, 0), ([], 4, 0)):
        vm = fresh()
        for fn in setup: vm.call(fn)
        vm.call('Fire', fire)
        assert vm.self.vars['Total'] == total and vm.self.vars.get('LastBy', vm.self) is vm.self, (setup, vm.self.vars)
    vm = fresh()
    vm.call('FireNote', 7)
    assert vm.self.vars['Total'] == 707 and vm.self.vars['LastTag'] == 'hit', vm.self.vars
    vm = fresh()
    vm.call('PingTwice')
    assert vm.self.vars['Pings'] == 2, vm.self.vars
    vm = fresh()
    vm.call('FireEmpty')
    assert vm.self.vars == dict(Total=0, Pings=0), vm.self.vars
    vm = fresh()
    other = vm.self.vars['Other'] = vm.new(Total=0, Pings=0)
    vm.call('HookOther')                                  # self's HandleScore on the other object's OnScore
    assert vm.binds == [(other, 'OnScore', 'HandleScore', vm.self)], vm.binds
    vm.call('Fire', 2)                                    # self's own OnScore has nothing bound
    assert vm.self.vars['Total'] == 0 and other.vars['Total'] == 0
    vm.call('FireOther', 6)
    assert vm.self.vars['Total'] == 6 and vm.self.vars['LastBy'] is vm.self and other.vars['Total'] == 0, vm.self.vars
    print('ok  DispatchRuns: Add (once per handler), Remove, Clear and Broadcast run as the engine\'s invocation list does')


def dispatch_kid():
    """DispatchKid binds its parent's handler and its own to the dispatchers it inherits: the bindings name the
    handlers on the kid itself (FindFunction walks up to the parent), and broadcasting OnPing runs KidPing."""
    kid = os.path.join(os.path.dirname(asset('DispatchRuns')), 'DispatchKid')
    vm = VM(kid, {}, Pings=0)
    vm.call('Hook')
    assert vm.binds == [(vm.self, 'OnScore', 'HandleDouble', vm.self), (vm.self, 'OnPing', 'KidPing', vm.self)], vm.binds
    vm.broadcast(vm.self, 'OnPing')
    assert vm.self.vars['Pings'] == 10, vm.self.vars
    pkg = invariants.Package(kid)
    ci = next(i for i, st in invariants.classes(pkg))
    for name in ('HandleDouble', 'KidPing'):
        found = deleg_rules.find_function(pkg, ci, name)
        assert found and found[0] == 'disk' and found[1].exports[found[2]]['name'] == name, (name, found)
    print('ok  DispatchKid: its parent\'s dispatchers take its own and its parent\'s handlers, found up the class chain')


def types_dispatcher():
    """TypesTest.HandleTimer broadcasts OnScored(1, self): with nothing bound it changes nothing; with HandleScored
    bound, as ReceiveBeginPlay's Add binds it, the handler runs once with the arguments (Scores gets the 1)."""
    vm = VM(asset('TypesTest'), {}, Scores=[])
    vm.call('HandleTimer')
    assert vm.self.vars == dict(Scores=[]), vm.self.vars
    vm.binds.append((vm.self, 'OnScored', 'HandleScored', vm.self))
    vm.call('HandleTimer')
    assert vm.self.vars == dict(Scores=[1]), vm.self.vars
    print('ok  TypesTest.HandleTimer: its broadcast reaches what is bound to OnScored, and nothing when nothing is')


def types_begin_play_dispatch():
    """TypesTest.ReceiveBeginPlay as the VM runs it, through TypesTest.cpp 211-217: OnScored.Add binds HandleScored,
    the Broadcast runs it once with First (1, what the container calls before it leave) and the actor, Remove and Clear
    leave nothing on OnScored, OnDestroyed keeps HandleDestroyed on this actor, and the timer gets HandleTimer bound on
    this object. When the actor is a Pawn, the Pawn's OnDestroyed.Add is the same binding on the same object, which
    AddUnique keeps once. Set_Add and Conv_VectorToString are the engine calls faked."""
    def set_add(vm, ctx, s, x):
        if x not in s: s.append(x)
    for pawn in (False, True):
        vm = VM(asset('TypesTest'), {'Set_Add': set_add, 'Conv_VectorToString': lambda vm, ctx, v: str(v)},
                isa=(lambda o, cls: cls in ('Pawn', 'TypesTest_C')) if pawn else None,
                Scores=[], Points=[], Weights={}, Seen=[])
        heard, real = [], vm.call

        def call(fn, *a, on=None, **k):
            if fn == 'HandleScored': heard.append((a, on))
            return real(fn, *a, on=on, **k)
        vm.call = call
        vm.call('ReceiveBeginPlay')
        me = vm.self
        timers = [a for n, c, a in vm.log if n == 'K2_SetTimerDelegate']
        assert heard == [((1, me), me)], (pawn, heard)
        assert vm.binds == [(me, 'OnDestroyed', 'HandleDestroyed', me)], (pawn, vm.binds)
        assert timers == [[('delegate', 'HandleTimer', me), 1.0, False, 0.0, 0.0]], (pawn, timers)
    print('ok  TypesTest.ReceiveBeginPlay: OnScored\'s Broadcast reaches HandleScored once, Remove / Clear empty it, '
          'OnDestroyed and the timer hold this actor\'s handlers')


def scoreboard_broadcast():
    """examples/Scoreboard: Score broadcasts OnScored with the FScoreChange it built - feat, name and running total -
    by value, and the board itself."""
    base = os.path.join(ROOT, 'Scoreboard', 'FSD', 'Content', '_AssetGenExamples', 'Scoreboard', 'Scoreboard')
    vm = VM(base, {}, Scores={})
    got, real = [], vm.call

    def call(fn, *a, on=None, **k):                       # a listener standing in for Announce
        if fn == 'Heard': got.append((base_names(dict(a[0])), a[1], on))
        else: return real(fn, *a, on=on, **k)
    vm.call = call
    vm.binds.append((vm.self, 'OnScored', 'Heard', vm.self))
    for feat in (1, 1, 3): vm.call('Score', feat)
    me = vm.self
    assert got == [(dict(Feat=1, Name='Salutes', Total=1), me, me), (dict(Feat=1, Name='Salutes', Total=2), me, me),
                   (dict(Feat=3, Name='Flares', Total=1), me, me)], got
    print('ok  Scoreboard.Score: OnScored carries the FScoreChange and the board to what is bound')


def timers_by_event_and_name():
    """A timer by event hands K2_SetTimerDelegate Tock bound on this object; one by name hands K2_SetTimer this object
    and the name "Tock", a zero-parameter function of the class, with no warning."""
    vm = VM(asset('DispatchRuns'), {})
    vm.call('ArmByEvent')
    vm.call('ArmByName')
    calls = [(n, a) for n, c, a in vm.log if n.startswith('K2_SetTimer')]
    assert calls == [('K2_SetTimerDelegate', [('delegate', 'Tock', vm.self), 0.5, True, 0.0, 0.0]),
                     ('K2_SetTimer', [vm.self, 'Tock', 1.0, False, 0.0, 0.0])], calls
    warned = [l for l in LOGS['DispatchRuns'].splitlines() if 'warning' in l.lower() and 'Tock' in l]
    assert not warned, warned
    print('ok  DispatchRuns: timers by event and by name name Tock on this object')


def compiled_with_flag(mod, body, name):
    """The compiler refuses the mod (the class body given) naming `name`, or compiles it with a warning naming it."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, mod + '.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/%s");\n'
                    'class %s : public AActor {\npublic:\n%s};\n' % (mod, mod, body))
        proc = assetgen_compile([src, UEAPI, tmp])
        warned = [l for l in proc.stdout.splitlines() if 'warning' in l.lower() and name in l]
        assert (proc.returncode != 0 and name in proc.stdout) or warned, \
            '%s compiles (exit %d) with no refusal or warning naming %s' % (mod, proc.returncode, name)


def delegate_var():
    """A TDelegate<void()> class variable is a DelegateProperty naming a delegate signature function (every
    DelegateProperty the package has does: keeps_invariants, delegate_property_signature), and what reaches
    K2_SetTimerDelegate from the variable and from a local is OnTimer bound on this object, which the variable holds."""
    base = asset('DelegateVar')
    keeps_invariants(base)
    pkg = invariants.Package(base)
    ci = next(i for i, st in invariants.classes(pkg))
    stored = next(p for p in pkg.struct(ci).props if p.name == 'Stored')
    assert stored.type == 'DelegateProperty' and pkg.obj(stored.ref)['name'].endswith('__DelegateSignature'), stored
    vm = VM(base, {}, Fired=0)
    vm.call('Arm')
    vm.call('ArmLocal')
    bound = ('delegate', 'OnTimer', vm.self)
    calls = [(n, a) for n, c, a in vm.log if n == 'K2_SetTimerDelegate']
    assert calls == [('K2_SetTimerDelegate', [bound, 0.5, False, 0.0, 0.0]),
                     ('K2_SetTimerDelegate', [bound, 2.0, True, 0.0, 0.0])] and vm.self.vars['Stored'] == bound, (calls, vm.self.vars)


def inherited_fire():
    """DispatchInheritedFire broadcasts its parent's OnHit, running the kid's bound handler with the argument; the
    broadcast names the parent's OnHit__DelegateSignature (keeps_invariants: broadcast_matches_signature)."""
    base = asset('DispatchInheritedFire')
    keeps_invariants(base)
    vm = VM(base, {}, Got=0)
    vm.call('Hook')
    vm.call('Fire', 4)
    assert vm.self.vars['Got'] == 40, vm.self.vars


deleg_rules_hold()
dispatch_facts()
dispatch_runs()
dispatch_kid()
types_dispatcher()
types_begin_play_dispatch()
scoreboard_broadcast()
timers_by_event_and_name()
delegate_var()
print('ok  DelegateVar: a TDelegate<void()> variable is a DelegateProperty naming its signature, holding OnTimer on this')
inherited_fire()
print('ok  DispatchInheritedFire: a child broadcasts its parent\'s dispatcher through the parent\'s signature')
# A method of another class bound with `this`: EX_InstanceDelegate binds the name on this object, whose class has no
# such function, so the broadcast or the timer silently skips it (ScriptDelegates.h 38-49, 479-502).
refused('DelegateForeign', '  UE_DISPATCHER(OnHit, int32 Points);\n  void F() { OnHit.Add(this, &DelegateOther::ForeignHit); }\n',
        'cannot bind DelegateOther::ForeignHit',
        top='class DelegateOther : public AActor {\npublic:\n  void ForeignHit(int32 Points) {}\n};\n')
refused('DelegateForeignTimer', '  void F() { UKismetSystemLibrary::K2_SetTimerDelegate({this, &DelegateOther::ForeignTick}, '
        '1.0f, false, 0.0f, 0.0f); }\n', 'cannot bind DelegateOther::ForeignTick',
        top='class DelegateOther : public AActor {\npublic:\n  void ForeignTick() {}\n};\n')
# UE_DISPATCHER's signature function is a real member: calling it runs its empty stub and broadcasts nothing, a call
# the editor's backend never emits (KismetCompilerVMBackend.cpp 1248-1252).
refused('DispatchSigCall', '  UE_DISPATCHER(OnHit, int32 Points);\n  void F() { OnHit__DelegateSignature(1); }\n',
        'OnHit__DelegateSignature is the dispatcher\'s signature')
# K2_SetTimer by name: a method with parameters, an inline one, a misspelled one - the engine sets no timer
# (KismetSystemLibrary.cpp 449-497).
refused('TimerByNameParms', '  int32 N;\n  void Poll(int32 X) { N = X; }\n'
        '  void Start() { UKismetSystemLibrary::K2_SetTimer(this, "Poll", 1.0f, true, 0.0f, 0.0f); }\n',
        'K2_SetTimer by name Poll names a function that takes parameters')
refused('TimerByNameInline', '  int32 N;\n  inline void Blink() { N = 2; }\n'
        '  void Start() { Blink(); UKismetSystemLibrary::K2_SetTimer(this, "Blink", 1.0f, true, 0.0f, 0.0f); }\n',
        'K2_SetTimer by name Blink names an inline method')
refused('TimerByNameMissing', '  int32 N;\n  void Tock() { N = 3; }\n'
        '  void Start() { UKismetSystemLibrary::K2_SetTimer(this, "Tik", 1.0f, true, 0.0f, 0.0f); }\n',
        'K2_SetTimer by name Tik names no function of the class')
print('ok  delegate refusals: a bind of another class\'s method on this, a call to a dispatcher\'s signature, a timer by a '
      'name with parameters, inline or missing')


# ---- SCS: the SimpleConstructionScript, its nodes, the component templates and the inheritable component handler
# (invariant_rules/scs.py)

import struct
import invariants

from invariant_rules import scs
assert {'scs_ownership', 'scs_forest', 'scs_ich_records'} <= set(invariants.RULES), sorted(invariants.RULES)


# ---- an offline model of what spawning an actor builds through its construction scripts

def bp_class(base):
    """(Package, the class export's FPackageIndex) of the package at base."""
    p = invariants.Package(base)
    return p, next(i for i, st in invariants.classes(p)) + 1


def actual_template(chain, owner, node):
    """USCS_Node::GetActualComponentTemplate (SCS_Node.cpp 27-52): from the actor's own class down to, not including,
    the class that owns the node, the first InheritableComponentHandler record keyed on it - FComponentKey::Match
    compares OwnerClass and AssociatedGuid (InheritableComponentHandler.cpp 557-560) - whose template is not null; else
    the node's own ComponentTemplate. As (package, export index), or None when nothing is constructed."""
    op, ok = owner
    for p, k in chain:
        if (p, k) == owner: break
        for cls, h, recs in scs.handlers(p):
            if cls != k: continue
            r = next((r for r in recs if r.owner and p.path(r.owner).lower() == op.path(ok).lower() and r.guid == node.guid), None)
            if r and r.template > 0: return p, r.template - 1
    return (op, node.template - 1) if node.template > 0 else None


def construct(base):
    """What AActor::ExecuteConstruction builds for an actor of the class at base whose native parent has no scene
    subobject (AActor's case): each Blueprint class's SCS runs, oldest first (ActorConstruction.cpp 754-766).
    ExecuteScriptOnActor takes the actor's root once and hands it to every root node, skipping the DefaultSceneRootNode
    when there is one (SimpleConstructionScript.cpp 640-688); with no root nodes and no root it makes a plain
    SceneComponent (690-702). ExecuteNodeOnActor builds the node's actual template; a scene component with no parent
    becomes the actor's root, else it attaches to the parent; its children get it as their parent, a non-scene
    component's children its own parent (SCS_Node.cpp 97-199). The component is stored in the actor's object property of
    the node's variable name when the class has one it IsA (159-175).
    Returns (root name or None, {component: attach parent or None}, {component: (package, template export)},
    {component: whether a variable holds it})."""
    p, cls = bp_class(base)
    chain = [(q, k + 1) for q, k, path in scs.class_chain(p, cls) if q is not None]
    assert scs.class_chain(p, cls)[-1][2] is not None, 'the class chain of %s is not all found' % base
    state = dict(root=None)
    attach, made, stored = {}, {}, {}

    def execute(s, x, parent):
        n = s.node(x)
        assert n is not None, 'node %d of %s is no SCS_Node' % (x, s.class_name())
        assert not n.parent_name, 'this model does not follow ParentComponentOrVariableName (%s)' % n.var
        t = actual_template(chain, (s.pkg, s.cls), n)
        if t is None: return
        tp, ti = t
        made[n.var] = t
        scene = scs.derives(tp, tp.exports[ti]['cls'], scs.SCENE)
        assert scene is not None, 'cannot tell whether %s is a scene component' % n.var
        if scene:
            if parent is None: state['root'] = n.var
            attach[n.var] = parent
        q, prop = scs.find_object_property(p, cls, n.var)
        stored[n.var] = q is not None and q.path(prop.ref).lower() in [c[2].lower() for c in scs.class_chain(tp, tp.exports[ti]['cls']) if c[2]]
        for c in n.children: execute(s, c, n.var if scene else parent)

    for q, k in reversed(chain):
        s = scs.own_scs(q, k - 1)
        if s is None: continue
        if s.roots:
            captured = state['root']
            for x in s.roots:
                if x and (x != s.dsr or captured is None): execute(s, x, captured)
        elif state['root'] is None:
            state['root'] = '(a new SceneComponent)'
    return state['root'], attach, made, stored


def effective(p, i, prop):
    """What an object loaded from export i holds for a tagged property: its own tag, else the value of the object it
    was constructed from, up the TemplateIndex chain (the event-driven loader constructs an export from its
    TemplateIndex object, AsyncLoading.cpp 2954-2971, then serializes its tags on top). A /Script CDO ends the chain:
    None, the class's native default."""
    while True:
        t = p.tag(i, prop)
        if t is not None:
            if t['type'] == 'BoolProperty': return t['bool']
            if t['type'] == 'FloatProperty': return struct.unpack('<f', t['value'])[0]
            return t['value']
        r = p.resolve(p.exports[i]['tmpl'])
        if not r: return None
        p, i = r


# ---- ScsShapes: the shapes AssetGen's construction scripts take, three classes deep

def scs_shapes():
    """ScsShapes declares a movement component before its first scene component: the actor's root is that first
    SCENE component (Pivot), the later scene components attach to it, the movement components attach to nothing and
    none has children. ScsShapesKid's own scene component attaches to the root its parent's construction script made.
    Every component lands in its variable, and the defaults each class sets reach the component the actor gets: an
    inherited component's through the nearest class's override record (ScsShapesGrandkid's Glow is ScsShapesKid's)."""
    folder = os.path.dirname(asset('ScsShapes'))
    own = {'Mover', 'Pivot', 'Arm', 'Glow', 'Spinner'}
    for cls, extra in (('ScsShapes', set()), ('ScsShapesKid', {'Hum'}), ('ScsShapesGrandkid', {'Hum'})):
        root, attach, made, stored = construct(os.path.join(folder, cls))
        want = {'Pivot': None, 'Arm': 'Pivot', 'Glow': 'Pivot'}
        want.update({h: 'Pivot' for h in extra})
        assert root == 'Pivot' and attach == want, (cls, root, attach)
        assert set(made) == own | extra and all(stored.values()), (cls, sorted(made), stored)
    speed = lambda cls, var: (lambda t: effective(t[0], t[1], var[1]))(construct(os.path.join(folder, cls))[2][var[0]])
    for cls, var, want in (('ScsShapes', ('Mover', 'InitialSpeed'), 1200.0), ('ScsShapes', ('Glow', 'Intensity'), 1000.0),
                           ('ScsShapesKid', ('Glow', 'Intensity'), 250.0), ('ScsShapesKid', ('Hum', 'VolumeMultiplier'), 0.75),
                           ('ScsShapesKid', ('Mover', 'InitialSpeed'), 1200.0),
                           ('ScsShapesGrandkid', ('Glow', 'Intensity'), 250.0), ('ScsShapesGrandkid', ('Arm', 'bVisible'), 0),
                           ('ScsShapesGrandkid', ('Hum', 'VolumeMultiplier'), 0.75), ('ScsShapesGrandkid', ('Hum', 'PitchMultiplier'), 2.0)):
        got = speed(cls, var)
        assert got == want, '%s: %s.%s loads %r, want %r' % (cls, var[0], var[1], got, want)
    # An override record's template is an importable archetype that GetArchetype resolves through the supers: RF_Public |
    # RF_ArchetypeObject | RF_InheritableComponentTemplate (UObjectArchetype.cpp 88-108).
    for cls in ('ScsShapesKid', 'ScsShapesGrandkid'):
        p, ci = bp_class(os.path.join(folder, cls))
        recs = [r for c, h, rs in scs.handlers(p) for r in rs]
        assert recs and all(p.exports[r.template - 1]['flags'] & 0x400021 == 0x400021 for r in recs), (cls, [hex(p.exports[r.template - 1]['flags']) for r in recs])
    for cls in ('ScsShapes', 'ScsShapesKid', 'ScsShapesGrandkid'): keeps_invariants(os.path.join(folder, cls))
    print('ok  ScsShapes: the first scene component is the root, a subclass\'s attaches to it, every component lands in '
          'its variable with the nearest class\'s defaults')


def scs_no_scene_root():
    """An actor whose only own component is not a scene component still ends its construction with a root:
    ExecuteScriptOnActor makes one only when RootNodes is empty, so the SCS must list a scene root (the editor keeps its
    DefaultSceneRoot node in RootNodes until another scene component takes its place). A root node listed for this is a
    node like any other to keeps_invariants: in AllNodes too, with its own VariableGuid (what a subclass's override of
    it is keyed on), and its variable (CompRootVariable)."""
    b = asset('ScsNoSceneRoot')
    root, attach, made, stored = construct(b)
    assert 'Spinner' in made and stored['Spinner'], (sorted(made), stored)
    assert root is not None, 'ScsNoSceneRoot_C ends its construction scripts without a RootComponent (it constructs only %s)' % sorted(made)
    keeps_invariants(b)
    print('ok  ScsNoSceneRoot: an actor whose only component is not a scene component gets the default scene root')


scs_shapes()
scs_no_scene_root()


# ---- Refusals the compiler does not make yet: a CreationMethod of Instance on a template, or anything but Native on a
# CDO's component subobject (BPGC-30). A component built on it as its archetype - an actor's native subobject off a CDO
# subobject, a subclass override's instance off an SCS template - skips AddOwnedComponent in
# UActorComponent::PostInitProperties (ActorComponent.cpp 282-287), so the actor never lists, registers or attaches to
# it; a native subobject marked UserConstructionScript is no longer addressable by name over the network
# (ActorComponent.cpp 1912) nor a native parent SCS nodes can attach to (ActorConstruction.cpp 737).

for mod, body, top in (
        ('ScsCreationInstance', '  UE_COMPONENT(UStaticMeshComponent, Body);\n'
                                '  UE_DEFAULTS { Body->CreationMethod = EComponentCreationMethod::Instance; }\n', ''),
        ('ScsCreationInstanceDso', '', 'class ScsCreationChar : public ACharacter {\n'
                                       '  UE_DEFAULTS { Mesh->CreationMethod = EComponentCreationMethod::Instance; }\n};\n'),
        ('ScsCreationUcsDso', '', 'class ScsCreationUcsChar : public ACharacter {\n'
                                  '  UE_DEFAULTS { Mesh->CreationMethod = EComponentCreationMethod::UserConstructionScript; }\n};\n')):
    refused(mod, body, '->CreationMethod is set by the engine when it makes the component', top)
print('ok  CreationMethod set in UE_DEFAULTS is refused, on a UE_COMPONENT and on a native default subobject')


# ---- COMP: component behaviour and refusals, the construction script, ticking (invariant_rules/components.py)

import struct, sys, tempfile
import invariants
from invariant_rules import components as comp


# ---- helpers: a cooked class read through invariants.Package

def class_pkg(base):
    """(Package, the class export's index) of the package at base."""
    p = invariants.Package(base)
    return p, comp.bp_class(p)


def template(p, ci, var):
    """The export index of the class's own <var>_GEN_VARIABLE (an SCS template or an override template)."""
    hits = [i for i, e in enumerate(p.exports) if e['name'] == var + '_GEN_VARIABLE' and e['outer'] == ci + 1]
    assert len(hits) == 1, '%s: %d exports named %s_GEN_VARIABLE under the class' % (p.base, len(hits), var)
    return hits[0]


def effective(p, i, prop):
    """What an object built from export i reads for a tagged property: its own tag, else its archetype's, up the
    TemplateIndex chain through the /Game packages (a /Script CDO ends it: None, the class's native default)."""
    while True:
        t = p.tag(i, prop)
        if t is not None:
            return t['bool'] if t['type'] == 'BoolProperty' else struct.unpack('<f', t['value'])[0] if t['type'] == 'FloatProperty' else t['value']
        r = comp.resolve(p, p.exports[i]['tmpl'])
        if not r: return None
        p, i = r


def node_named(p, ci, var):
    s = comp.scs(p, ci)
    return next((n for n in s[1].values() if n.name == var), None) if s else None


def compile_to(src_text, mod, extra=()):
    """Compiles a source text as tests/<mod>.cpp would be, into a fresh temp dir: (dir, the package folder, stdout)."""
    tmp = tempfile.mkdtemp()
    src = os.path.join(tmp, mod + '.cpp')
    with open(src, 'w', encoding='utf-8') as f: f.write(src_text)
    out = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', mod)
    os.makedirs(out)
    proc = assetgen_compile([src, UEAPI, out] + list(extra))
    assert proc.returncode == 0, proc.stdout + proc.stderr
    return tmp, out, proc.stdout


# ---- CompTwoSet: two defaults on one inherited component, and a parent's rebuild

def comp_two_set():
    """Two UE_DEFAULTS statements on one inherited component are one override: one handler record, keyed on the
    parent's node (FComponentKey: OwnerClass + AssociatedGuid), and one template holding both values."""
    folder = os.path.dirname(asset('CompTwoSet'))
    kid, ci = class_pkg(os.path.join(folder, 'CompTwoSet'))
    base, bi = class_pkg(os.path.join(folder, 'TwoSetBase'))
    hi, records = comp.ich_records(kid, ci)
    assert len(records) == 1, [sorted(el) for _, el, _ in records]
    _, el, key = records[0]
    assert kid.path(struct.unpack('<i', key['OwnerClass']['value'])[0]) == '/Game/_ElytrasMods/CompTwoSet/TwoSetBase.TwoSetBase_C', key
    assert comp.tag_name(kid, key['SCSVariableName']) == 'Lamp' and key['AssociatedGuid']['value'] == node_named(base, bi, 'Lamp').guid, key
    lamp = template(kid, ci, 'Lamp')
    assert struct.unpack('<i', el['ComponentTemplate']['value'])[0] == lamp + 1, el['ComponentTemplate']
    assert effective(kid, lamp, 'Intensity') == 300.0 and effective(kid, lamp, 'bVisible') == 0, \
        (effective(kid, lamp, 'Intensity'), effective(kid, lamp, 'bVisible'))
    for c in ('CompTwoSet', 'TwoSetBase'): keeps_invariants(os.path.join(folder, c))
    print('ok  CompTwoSet: two defaults on one inherited component make one record and one template holding both')


def comp_guid_stable():
    """A parent Blueprint shipped again with a component added before Lamp and a method more keeps Lamp's node guid,
    so the record a child already ships with still matches it (FComponentKey::Match compares the guid, and a changed
    one orphans the child's override silently)."""
    folder = os.path.dirname(asset('CompTwoSet'))
    src = open(os.path.join(TESTS, 'CompTwoSet.cpp'), encoding='utf-8-sig').read()
    lamp_decl = '  UE_COMPONENT(UPointLightComponent, Lamp);\n'
    probe = '  int32 Probe() { return 1; }\n'
    assert src.count(lamp_decl) == 1 and src.count(probe) == 1, 'CompTwoSet.cpp changed shape'
    variant = src.replace(lamp_decl, '  UE_COMPONENT(UStaticMeshComponent, Extra);\n' + lamp_decl) \
                 .replace(probe, probe + '  int32 Probe2() { return 2; }\n')
    tmp, out, _ = compile_to(variant, 'CompTwoSet')
    try:
        old_base, obi = class_pkg(os.path.join(folder, 'TwoSetBase'))
        new_base, nbi = class_pkg(os.path.join(out, 'TwoSetBase'))
        old, new, extra = node_named(old_base, obi, 'Lamp'), node_named(new_base, nbi, 'Lamp'), node_named(new_base, nbi, 'Extra')
        assert extra is not None and old.guid == new.guid != bytes(16), (old.guid.hex(), new.guid.hex())
        assert extra.guid not in (new.guid, bytes(16)), extra.guid.hex()
        for kid_base in (os.path.join(folder, 'CompTwoSet'), os.path.join(out, 'CompTwoSet')):
            kid, ci = class_pkg(kid_base)
            (_, _, key), = comp.ich_records(kid, ci)[1]
            assert key['AssociatedGuid']['value'] == old.guid, (kid_base, key['AssociatedGuid']['value'].hex())
        keeps_invariants(os.path.join(out, 'TwoSetBase'))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('ok  CompTwoSet: a parent rebuilt with a component before Lamp and a method more keeps Lamp\'s node guid')


comp_two_set()
comp_guid_stable()


# ---- CompUcs: the construction script

def comp_ucs():
    """UserConstructionScript is an override of AActor's event, reached by name - in FuncMap, no parameters (the event
    thunk passes none), not native (a native one is bound to no script) - and only the most derived one runs: a
    parent's body runs in a child through the explicit parent call alone, once."""
    folder = os.path.dirname(asset('CompUcs'))
    base = os.path.join(folder, 'UcsBase')
    for cls, parent_ucs in (('UcsBase', '/Script/Engine.Actor:UserConstructionScript'),
                            ('CompUcs', '/Game/_ElytrasMods/CompUcs/UcsBase.UcsBase_C:UserConstructionScript'),
                            ('UcsSolo', '/Game/_ElytrasMods/CompUcs/UcsBase.UcsBase_C:UserConstructionScript'),
                            ('UcsAdder', '/Script/Engine.Actor:UserConstructionScript')):
        p, ci = class_pkg(os.path.join(folder, cls))
        u = comp.ucs_function(p, ci)
        assert u is not None and ('UserConstructionScript', u + 1) in p.struct(ci).func_map, (cls, p.struct(ci).func_map)
        st = p.struct(u)
        assert not [x.name for x in st.props if x.flags & invariants.CPF_Parm], (cls, st.props)
        assert not st.function_flags & comp.FUNC_Native, (cls, hex(st.function_flags))
        # The super is editor convention, not dispatch (a call by name resolves through FuncMap): Actor's event or a
        # Blueprint parent's override of it.
        assert p.path(st.super).endswith(':UserConstructionScript') and p.path(st.super) in (
            '/Script/Engine.Actor:UserConstructionScript', parent_ucs), (cls, p.path(st.super))
        keeps_invariants(os.path.join(folder, cls))
    n = 0
    for chain, want in (([base], 1), ([os.path.join(folder, 'CompUcs'), base], 11), ([os.path.join(folder, 'UcsSolo'), base], 100)):
        for start in (0, 5):
            fields = {'Hits': start}
            run_as(chain, 'UserConstructionScript', fields)
            assert fields['Hits'] == start + want, (os.path.basename(chain[0]), start, fields)
            n += 1
    # Adding a component is what the construction script is for: UcsAdder's reaches AddComponentByClass.
    p, ci = class_pkg(os.path.join(folder, 'UcsAdder'))
    assert '/Script/Engine.Actor:AddComponentByClass' in {t for _, t in comp.calls(p, comp.ucs_function(p, ci))}
    print('ok  CompUcs: UserConstructionScript overrides by name with no parameters; the parent\'s runs only through '
          'the explicit parent call  (%d cases)' % n)


comp_ucs()


# ---- CompTick: ReceiveTick makes the class tick

def comp_tick():
    """An actor with its own ReceiveTick has PrimaryActorTick.bCanEverTick set on its CDO - AActor's is false, and
    the tick function registers only when it is set - and one without does not tick (no tick function spent on it).
    Also for the examples the docs point at: WaitForPlayer and NetworkedSwitch's SwitchRemote tick, HelloWorld does
    not."""
    folder = os.path.dirname(asset('CompTick'))
    cases = [(os.path.join(folder, 'CompTick'), True), (os.path.join(folder, 'TickLess'), False)]
    for example, cls, ticks in (('WaitForPlayer', 'WaitForPlayer', True), ('NetworkedSwitch', 'SwitchRemote', True),
                                ('HelloWorld', 'HelloWorld', False)):
        b = os.path.join(ROOT, example, 'FSD', 'Content', '_AssetGenExamples', example, cls)
        if os.path.exists(b + '.uasset'): cases.append((b, ticks))
    assert len(cases) == 5, 'the examples are not built under ' + ROOT
    for b, ticks in cases:
        p, ci = class_pkg(b)
        cdo = p.struct(ci).cdo - 1
        t = p.tag(cdo, 'PrimaryActorTick')
        can = comp.bool_in(p, cdo, t, 'bCanEverTick') if t else None
        assert (can == 1) if ticks else not can, (os.path.basename(b), can)
        keeps_invariants(b)
    fields = {'Frames': 3}
    run(os.path.join(folder, 'CompTick'), 'ReceiveTick', self_vars=fields, DeltaSeconds=0.25)
    assert fields == {'Frames': 4}, fields
    print('ok  CompTick: a class with its own ReceiveTick can ever tick, one without cannot (and the examples)')


def comp_tick_component():
    """A component Blueprint with its own ReceiveTick registers its tick: its CDO's PrimaryComponentTick.bCanEverTick
    is set (UActorComponent's is false), and the ReceiveTick it ships runs."""
    b = asset('CompTickComponent')
    p, ci = class_pkg(b)
    cdo = p.struct(ci).cdo - 1
    t = p.tag(cdo, 'PrimaryComponentTick')
    assert t and comp.bool_in(p, cdo, t, 'bCanEverTick') == 1, 'Default__CompTickComponent_C has no PrimaryComponentTick.bCanEverTick'
    fields = {'N': 2}
    run(b, 'ReceiveTick', self_vars=fields, DeltaSeconds=0.25)
    assert fields == {'N': 3}, fields
    keeps_invariants(b)
    print('ok  CompTickComponent: a component Blueprint with its own ReceiveTick sets PrimaryComponentTick.bCanEverTick')


def comp_tick_patch():
    """A UE_PATCH that adds ReceiveTick to a Blueprint whose parent is AActor (CompTest, standing in for a game
    Blueprint) must set its CDO's PrimaryActorTick.bCanEverTick, or the added ReceiveTick never runs."""
    comp_game = os.path.join(ROOT, 'CompTest', 'FSD', 'Content')
    src = ('#include "UeApi/Types.h"\n#include "UeApi/Engine.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/CompTickPatch");\n'
           'class CompTest : public AActor {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/CompTest/CompTest", "CompTest_C");\n'
           '  int32 Ticks;\n};\n'
           'class Tweaks : public CompTest {\n  UE_PATCH;\n  void ReceiveTick(float DeltaSeconds) { Ticks = Ticks + 1; }\n};\n')
    tmp, out, _ = compile_to(src, 'CompTickPatch', ['--game', comp_game])
    try:
        b = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', 'CompTest', 'CompTest')
        fields = {'Ticks': 1}
        run(b, 'ReceiveTick', self_vars=fields, DeltaSeconds=0.5)
        assert fields == {'Ticks': 2}, fields
        p, ci = class_pkg(b)
        cdo = p.struct(ci).cdo - 1
        t = p.tag(cdo, 'PrimaryActorTick')
        assert t and comp.bool_in(p, cdo, t, 'bCanEverTick') == 1, 'the patched Default__CompTest_C has no PrimaryActorTick.bCanEverTick'
        keeps_invariants(b)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('ok  CompTickPatch: a patch that adds ReceiveTick sets the CDO\'s PrimaryActorTick.bCanEverTick, and it runs')


def comp_override_chain():
    """Three levels of one component's defaults fold as C++ constructors do: the grandchild's Lamp has ChainMid's
    Intensity 250 and its own bVisible false, its override template archetyped on ChainMid's (the nearest one, which
    GetArchetype finds by name) and built after it."""
    folder = os.path.dirname(asset('CompOverrideChain'))
    p, ci = class_pkg(os.path.join(folder, 'CompOverrideChain'))
    lamp = template(p, ci, 'Lamp')
    got = effective(p, lamp, 'Intensity'), effective(p, lamp, 'bVisible')
    assert got == (250.0, 0), 'CompOverrideChain\'s Lamp loads Intensity, bVisible %s, want (250.0, 0)' % (got,)
    e = p.exports[lamp]
    assert p.path(e['tmpl']) == '/Game/_ElytrasMods/CompOverrideChain/ChainMid.ChainMid_C:Lamp_GEN_VARIABLE', p.path(e['tmpl'])
    assert e['tmpl'] in e['deps'][2], e['deps']
    for cls in ('CompOverrideChain', 'ChainMid', 'ChainBase'): keeps_invariants(os.path.join(folder, cls))
    print('ok  CompOverrideChain: an override template is archetyped on the nearest ancestor\'s override, and loads its values')


def world_location(p, ci, var):
    """Where component `var` of class ci ends up relative to the spawned actor, as the SCS builds it: its
    RelativeLocation plus its parent's, up the ChildNodes. A root node naming a parent hangs off that ancestor
    Blueprint's node, or off a native subobject (only the native root, at the actor's origin, is known here). A
    parentless root node hangs off the actor's root when the actor already has one - an ancestor Blueprint's SCS built
    one, or the native class has one (SimpleConstructionScript.cpp 643, 686) - and otherwise IS the actor's root, put at
    the spawn transform whatever its template says (SCS_Node.cpp 122-135). A native class has one when it has a scene
    component default subobject: its constructor names the root (ACharacter's capsule, Character.cpp 59), or
    ExecuteConstruction takes the first unattached native scene component (ActorConstruction.cpp 736-746) - so also
    where UeApi leaves RootComponent unmarked because two subobjects fit it. Rotation and scale must be absent
    (identity), so locations add."""
    si, nodes, roots, dsr = comp.scs(p, ci)
    parent_of = {c: k for k, n in nodes.items() for c in n.children}
    k = next(k for k, n in nodes.items() if n.name == var)
    total = (0.0, 0.0, 0.0)
    add = lambda a, b: tuple(round(x + y, 4) for x, y in zip(a, b))
    while True:
        n = nodes[k]
        assert n.template > 0, '%s: node %s has no template' % (p.exports[ci]['name'], n.name)
        for prop, one in (('RelativeRotation', (0.0, 0.0, 0.0)), ('RelativeScale3D', (1.0, 1.0, 1.0))):
            v = effective(p, n.template - 1, prop)
            assert v is None or struct.unpack('<3f', v) == one, '%s: %s has %s %s' % (p.exports[ci]['name'], n.name, prop, struct.unpack('<3f', v))
        v = effective(p, n.template - 1, 'RelativeLocation')
        rel = struct.unpack('<3f', v) if v is not None else (0.0, 0.0, 0.0)
        if k in parent_of:
            total, k = add(total, rel), parent_of[k]
            continue
        links, native = comp.chain(p, ci)
        if n.parent != 'None':
            if n.native:
                assert native and n.parent.lower() == (comp.native_root(native) or '').lower(), (n.name, n.parent)
                return add(total, rel)
            q, qi = next((q, qi) for q, qi in links[1:] if q.exports[qi]['name'].lower() == n.owner.lower())
            return add(add(total, rel), world_location(q, qi, n.parent))
        native_root = native and (comp.native_root(native)
                                  or any(comp.native_is_scene(c) for c in (comp.native_subobjects(native) or {}).values()))
        inherited = bool(native_root) or any(comp.executed(q, qi, False) for q, qi in links[1:])
        return add(total, rel) if inherited else total


def comp_root_keep():
    """In a subclass the actor already has a root when the class's SCS runs (a mod parent's, or ACharacter's capsule),
    so its first own scene component attaches to that root and keeps its offset: Pivot sits 100 above the actor's
    origin and Glow, attached to Pivot, 10 in front of it. RigSpot's Spot, a light, sits 50 up. None is warned about as
    the actor's root."""
    folder = os.path.dirname(asset('CompRootKeep'))
    for cls, want in (('CompRootKeep', {'Pivot': (0.0, 0.0, 100.0), 'Glow': (10.0, 0.0, 100.0)}),
                      ('RigChar', {'Pivot': (0.0, 0.0, 100.0), 'Glow': (10.0, 0.0, 100.0)}),
                      ('RigSpot', {'Spot': (0.0, 0.0, 50.0)})):
        p, ci = class_pkg(os.path.join(folder, cls))
        got = {var: world_location(p, ci, var) for var in want}
        assert got == want, '%s: components sit at %s from the actor, want %s' % (cls, got, want)
    tmp, out, log = compile_to(open(os.path.join(TESTS, 'CompRootKeep.cpp'), encoding='utf-8-sig').read(), 'CompRootKeep')
    shutil.rmtree(tmp, ignore_errors=True)
    assert "is the actor's root" not in log, log
    for cls in ('CompRootKeep', 'RigChar', 'RigSpot', 'RigBase'): keeps_invariants(os.path.join(folder, cls))
    print('ok  CompRootKeep: a subclass\'s first scene component attaches to the inherited root and keeps its transform')


def comp_char_root():
    """components.py's hierarchy(), the construction scs_names_distinct checks names against, is the one the engine
    runs: it builds no DefaultSceneRoot node for CompCharRoot, a character, or NoiseKid, below a Blueprint parent with a
    root, whether or not their SCS lists one. ExecuteScriptOnActor skips that node when the actor has a root already
    (SimpleConstructionScript.cpp 648), as an ACharacter always does: its capsule (Character.cpp 59), and failing that
    the first unattached native scene component (ActorConstruction.cpp 736-746). UeApi marks no RootComponent subobject
    for ACharacter, since two of its subobjects fit the member."""
    folder = os.path.dirname(asset('CompCharRoot'))
    for cls in ('CompCharRoot', 'NoiseKid'):
        p, ci = class_pkg(os.path.join(folder, cls))
        built = [n.name for _, _, ns, _ in comp.hierarchy(p, ci)[0] for n in ns]
        assert 'DefaultSceneRoot' not in built, 'hierarchy() builds %s on %s, whose actor has a root already' % (built, cls)
        keeps_invariants(os.path.join(folder, cls))
    print('ok  CompCharRoot: the rules\' construction of an actor with a root already skips a DefaultSceneRoot node, as '
          'the engine does')


def comp_default_root_inherited():
    """The DefaultSceneRoot node is in an SCS's RootNodes and AllNodes exactly when no root is there before the SCS
    runs and none of the class's own components is a scene component: the editor drops it once
    GetSceneRootComponentTemplate finds a root - the native CDO's root or a scene subobject, or a scene root node of a
    parent Blueprint's (ValidateSceneRootNodes, SimpleConstructionScript.cpp 1006-1150), and every one of the game's
    1,885 SCS classes is saved that way. CompCharRoot, a character, and NoiseKid, below NoiseBase's root, list none;
    ScsNoSceneRoot, an AActor of a movement component alone, lists it. The two below AActor end their construction
    with a root (construct(); the character's is its capsule)."""
    folder = os.path.dirname(asset('CompCharRoot'))
    for base, listed in ((os.path.join(folder, 'CompCharRoot'), False), (os.path.join(folder, 'NoiseKid'), False),
                         (asset('ScsNoSceneRoot'), True)):
        p, ci = class_pkg(base)
        si, nodes, roots, dsr = comp.scs(p, ci)
        every = [x - 1 for x in comp.tag_objects(comp.tags_at(p, si).get('AllNodes')) if x > 0]
        where = [name for name, of in (('RootNodes', roots), ('AllNodes', every)) if dsr is not None and dsr in of]
        assert where == (['RootNodes', 'AllNodes'] if listed else []), \
            '%s lists its DefaultSceneRoot node in %s' % (os.path.basename(base), where or 'neither RootNodes nor AllNodes')
        if not base.endswith('CompCharRoot'):
            assert construct(base)[0] is not None, '%s ends its construction with no root' % os.path.basename(base)
    print('ok  CompDefaultRoot: the DefaultSceneRoot node is listed only where the actor has no root before the SCS and '
          'no own scene component, as the editor saves it')


comp_tick()
comp_tick_component()
comp_tick_patch()
comp_override_chain()
comp_root_keep()
comp_char_root()
comp_default_root_inherited()


def comp_no_components():
    """An actor class that declares no UE_COMPONENT and inherits no root ends its construction with the DefaultSceneRoot
    node's component as its root, as the editor builds it: the editor keeps that node in RootNodes and AllNodes while no
    scene component takes its place, as 40 of the game's classes save it (ENE_EnemySpawner). That component is named
    DefaultSceneRoot and net addressable (SCS_Node.cpp 99, 107). With neither list ExecuteScriptOnActor makes a plain
    SceneComponent instead (SimpleConstructionScript.cpp 690-702), which nothing marks net addressable, so no reference
    to it crosses the network (ActorComponent.cpp 1901-1913). NoCompKid's Lamp attaches to that root, 40 above it."""
    base = asset('CompNoComponents')
    for cls, attached in (('CompNoComponents', {}), ('NoCompKid', {'Lamp': 'DefaultSceneRoot'})):
        b = os.path.join(os.path.dirname(base), cls)
        root, attach, made, stored = construct(b)
        assert root == 'DefaultSceneRoot', 'an actor of %s ends its construction with %s as its root' % (cls, root)
        assert attach == dict(attached, DefaultSceneRoot=None), (cls, attach)
        keeps_invariants(b)
    p, ci = class_pkg(os.path.join(os.path.dirname(base), 'NoCompKid'))
    assert world_location(p, ci, 'Lamp') == (0.0, 0.0, 40.0), world_location(p, ci, 'Lamp')
    print('ok  CompNoComponents: an actor with no components gets the DefaultSceneRoot node as its root, as in the '
          'editor')


comp_no_components()


def comp_root_variable():
    """Where an SCS lists its DefaultSceneRoot node, the class has a variable of that name holding the component, as the
    editor gives each node it lists one (KismetCompiler.cpp 884-898) and 40 of the game's classes have it
    (ENE_EnemySpawner): ExecuteNodeOnActor stores the component there, where with none it logs on every spawn that the
    class has no such property (SCS_Node.cpp 159-178). CompRootVariable has no component, RootVarMover a movement
    component alone, and RootVarKid finds its parent's (FindFProperty walks the supers)."""
    base = asset('CompRootVariable')
    for cls in ('CompRootVariable', 'RootVarMover', 'RootVarKid'):
        b = os.path.join(os.path.dirname(base), cls)
        root, attach, made, stored = construct(b)
        assert root == 'DefaultSceneRoot' and stored.get('DefaultSceneRoot'), \
            '%s: no variable holds its root, the DefaultSceneRoot node\'s component (%s)' % (cls, stored)
        keeps_invariants(b)
    print('ok  CompRootVariable: a listed DefaultSceneRoot node has its variable on the class, which holds the root')


comp_root_variable()


def comp_attach_inherited():
    """SetupAttachment in UE_DEFAULTS places a component as a constructor does. Attached to an inherited one, it is a
    root node naming that parent: an ancestor Blueprint's node by its variable and class (Glow on AttachBase_C's Lamp),
    or a native default subobject by its object name (AttachChar's Glow on CharacterMesh0, ACharacter's Mesh). Below
    ACharacter, Mesh is still CharacterMesh0 though APlayerCharacter's FPMesh is a skeletal mesh too (AttachPlayer, at
    its socket), and a default set through it overrides that subobject. Attached
    to one of the class's own, it is one of that node's ChildNodes, at its socket (Tip on Glow, at Bulb). Each keeps its
    offset under its parent; with no inherited root, the root is the first scene component left alone (AttachOwn's
    Root, not Bulb), and its offset passes to what hangs below it. In a function, SetupAttachment attaches at once and
    keeps the relative transform: AttachToComponent with KeepRelative (0), no welding."""
    folder = os.path.dirname(asset('CompAttachInherited'))
    for cls, parent, owner, native, socket in (('CompAttachInherited', 'Lamp', 'AttachBase_C', False, None),
                                               ('AttachChar', 'CharacterMesh0', 'None', True, None),
                                               ('AttachPlayer', 'CharacterMesh0', 'None', True, 'S_Lamp')):
        p, ci = class_pkg(os.path.join(folder, cls))
        si, nodes, roots, dsr = comp.scs(p, ci)
        glow = next(n for n in nodes.values() if n.name == 'Glow')
        assert glow.index in roots and (glow.parent, glow.owner, glow.native) == (parent, owner, native), (
            cls, glow.parent, glow.owner, glow.native)
        at = comp.tags_at(p, glow.index).get('AttachToName')
        assert (at and comp.tag_name(p, at)) == (socket or None), (cls, at and comp.tag_name(p, at))
    b = os.path.join(folder, 'AttachPlayer')
    ex = dumpexp.load(b)[5]
    k = next(i for i, e in enumerate(ex) if e['name'] == 'CharacterMesh0')
    got = tuple(ref(b, ex[k][f]) for f in ('cls', 'tmpl', 'outer'))
    assert got == ('/Script/Engine.SkeletalMeshComponent', '/Script/FSD.Default__PlayerCharacter:CharacterMesh0',
                   'Default__AttachPlayer_C'), got
    assert 'bVisible [0] BoolProperty size=0 value=0' in dump('dumptags.py', b, k), dump('dumptags.py', b, k)
    p, ci = class_pkg(os.path.join(folder, 'CompAttachInherited'))
    glow, tip = node_named(p, ci, 'Glow'), node_named(p, ci, 'Tip')
    assert glow.children == [tip.index] and comp.tag_name(p, comp.tags_at(p, tip.index)['AttachToName']) == 'Bulb'
    want = {'Glow': (10.0, 0.0, 100.0), 'Tip': (10.0, 5.0, 100.0), 'Pivot': (0.0, 0.0, 20.0)}
    got = {var: world_location(p, ci, var) for var in want}
    assert got == want, 'components sit at %s from the actor, want %s' % (got, want)
    p, ci = class_pkg(os.path.join(folder, 'AttachOwn'))
    si, nodes, roots, dsr = comp.scs(p, ci)
    assert [nodes[r].name for r in roots] == ['Root'], [nodes[r].name for r in roots]
    want = {'Arm': (100.0, 0.0, 50.0), 'Bulb': (100.0, 0.0, 60.0)}
    got = {var: world_location(p, ci, var) for var in want}
    assert got == want, 'components sit at %s from the actor, want %s' % (got, want)
    vm = VM(asset('CompAttachInherited'), {}, Pivot=Obj('SceneComponent'), Lamp=Obj('PointLightComponent'))
    vm.call('ReceiveBeginPlay')
    assert vm.log == [('K2_AttachToComponent', vm.self.vars['Pivot'], [vm.self.vars['Lamp'], 'None', 0, 0, 0, False])], vm.log
    for cls in ('CompAttachInherited', 'AttachBase', 'AttachChar', 'AttachPlayer', 'AttachOwn'):
        keeps_invariants(os.path.join(folder, cls))
    print('ok  CompAttachInherited: SetupAttachment attaches a component to an inherited Blueprint or native one, or at a\n'
          '    socket of one of its own class\'s, each keeping its offset; in a function it attaches at once')


comp_attach_inherited()


def comp_attach_root():
    """`Glow->SetupAttachment(RootComponent)` in UE_DEFAULTS puts Glow under the actor's root, whichever component that
    is, as a constructor's call does. Below a parent that gives the actor a root, ACharacter's capsule (RootChar) or a
    Blueprint parent's root (RootKid), Glow's node is a root node naming no parent, which ExecuteScriptOnActor attaches
    to that root (SimpleConstructionScript.cpp 686). With none to inherit, the root is the first of the class's own
    scene components left alone, Base though Glow is declared first (RootOwn), and with none of those the
    DefaultSceneRoot node, which keeps Glow as its child, as the editor saves a component added under it
    (CompAttachRoot). Glow sits 30 above the root, and RootOwn's Base hands its own 50 on to it. At a socket
    (RootSock), the node keeps it as AttachToName, which ExecuteNodeOnActor passes to SetupAttachment (SCS_Node.cpp
    152)."""
    base = asset('CompAttachRoot')
    folder = os.path.dirname(base)
    for cls, root in (('RootKid', 'Root'), ('RootOwn', 'Base'), ('CompAttachRoot', 'DefaultSceneRoot')):
        got, attach, made, stored = construct(os.path.join(folder, cls))
        assert (got, attach.get('Glow')) == (root, root), '%s: the root is %s and Glow attaches to %s' % (cls, got, attach.get('Glow'))
    for cls, socket in (('RootChar', None), ('RootSock', 'Sock')):
        p, ci = class_pkg(os.path.join(folder, cls))
        si, nodes, roots, dsr = comp.scs(p, ci)
        glow = node_named(p, ci, 'Glow')
        assert glow.index in roots and glow.parent == 'None', (cls, glow.parent, [nodes[r].name for r in roots])
        at = comp.tags_at(p, glow.index).get('AttachToName')
        assert (at and comp.tag_name(p, at)) == socket, '%s: Glow attaches at socket %r, want %r' % (
            cls, at and comp.tag_name(p, at), socket)
    keeps_invariants(os.path.join(folder, 'RootSock'))
    for cls, want in (('RootChar', 30.0), ('RootKid', 30.0), ('RootOwn', 80.0), ('CompAttachRoot', 30.0)):
        p, ci = class_pkg(os.path.join(folder, cls))
        assert world_location(p, ci, 'Glow') == (0.0, 0.0, want), (cls, world_location(p, ci, 'Glow'))
        keeps_invariants(os.path.join(folder, cls))
    print('ok  CompAttachRoot: SetupAttachment(RootComponent) puts a component under the actor\'s root, inherited, own or '
          'the default one, at a socket of it')


comp_attach_root()


def comp_attach_body():
    """SetupAttachment in a function attaches at once and keeps the relative transform: K2_AttachToComponent with
    KeepRelative (0) for location, rotation and scale and no welding, which is what the engine's own call leads to when
    the component registers (SceneComponent.cpp 667-683). On a component already registered, as an actor's are once it
    is constructed, the engine's own call does nothing but fail an ensure (1750), so the compiler says, naming the
    function, that it attached anyway."""
    base = asset('CompAttachBody')
    log = LOGS['CompAttachBody']
    warned = [l for l in log.splitlines() if 'warning:' in l and 'SetupAttachment' in l]
    assert warned and all('CompAttachBody::ReceiveBeginPlay' in l for l in warned), log
    vm = VM(base, {}, Pivot=Obj('SceneComponent'), Lamp=Obj('PointLightComponent'))
    vm.call('ReceiveBeginPlay')
    assert vm.log == [('K2_AttachToComponent', vm.self.vars['Pivot'], [vm.self.vars['Lamp'], 'None', 0, 0, 0, False])], vm.log
    print('ok  CompAttachBody: SetupAttachment in a function attaches at once, keeping the relative transform, and warns '
          'that the engine\'s own would not')


comp_attach_body()


# ---- Refusals: each of these would build a package the engine mishandles

CLASH_BASE = ('class ClashBase : public AActor {\npublic:\n  UE_COMPONENT(USceneComponent, Root);\n'
              '  UE_COMPONENT(UPointLightComponent, Lamp);\n};\n')
OBJECTS = '#include "%s"\n' % os.path.join(AG, 'include', 'Objects.h').replace(os.sep, '/')
for mod, body, why, top in (
        # BPGC-18: the class's own DefaultSceneRoot node and template already have that name.
        ('ClashDefaultRoot', '  UE_COMPONENT(USceneComponent, DefaultSceneRoot);\n  UE_COMPONENT(UStaticMeshComponent, Body);\n',
         'DefaultSceneRoot', ''),
        # ...and its variable, which ExecuteNodeOnActor stores the root in: a second one, or a subclass's found first.
        ('ClashRootVariable', '  USceneComponent* DefaultSceneRoot;\n', 'the variable of the root', ''),
        # A parent Blueprint's component: two nodes, one name, and the second rebuilds the first in place.
        ('ClashInherited', '', 'Lamp', CLASH_BASE + 'class ClashKid : public ClashBase {\npublic:\n'
                                                    '  UE_COMPONENT(UPointLightComponent, Lamp);\n};\n'),
        # ACharacter's default subobjects (CollisionCylinder, CharacterMesh0): another class under that name is a Fatal.
        ('ClashCharMesh', '', 'CharacterMesh0', 'class ClashChar : public ACharacter {\npublic:\n'
                                                '  UE_COMPONENT(UStaticMeshComponent, CharacterMesh0);\n};\n'),
        ('ClashCapsule', '', 'CollisionCylinder', 'class ClashChar : public ACharacter {\npublic:\n'
                                                  '  UE_COMPONENT(USceneComponent, CollisionCylinder);\n};\n'),
        # ACharacter's member Mesh: a second property of that name on the class, which the editor never allows.
        ('ClashCharMember', '', 'Mesh', 'class ClashChar : public ACharacter {\npublic:\n'
                                        '  UE_COMPONENT(UStaticMeshComponent, Mesh);\n};\n'),
        # A game Blueprint's SCS node (BP_LightPost01's Scene).
        ('ClashGameScs', '', 'Scene', '#include "UeApi/Game/BP_LightPost01_C.h"\n'
                                      'class ClashPost : public Game::Art::Environments::SpaceRig::BP_LightPost01_C {\npublic:\n'
                                      '  UE_COMPONENT(USceneComponent, Scene);\n};\n'),
        # BPGC-43 / NODE-19: the construction script runs with bIsRunningConstructionScript set, and SpawnActor returns
        # None. The editor keeps a spawn out of the construction script graph itself, so a spawn written there is refused.
        ('UcsSpawn', '  AActor* Made;\n  void UserConstructionScript() { Made = SpawnActor<AActor>(AActor::StaticClass(), FTransform()); }\n',
         'UserConstructionScript', OBJECTS),
        ('UcsSpawnStatics', '  AActor* Made;\n  void UserConstructionScript() {\n'
                            '    Made = UGameplayStatics::BeginDeferredActorSpawnFromClass(this, AActor::StaticClass(), FTransform(),\n'
                            '        ESpawnActorCollisionHandlingMethod::Undefined, nullptr);\n  }\n', 'UserConstructionScript', OBJECTS),
        # BPGC-32 / NODE-23: AddComponent finds a template by name in ComponentTemplates, which a mod class has none of.
        ('AddByName', '  void ReceiveBeginPlay() { AddComponent(FName("X"), false, FTransform(), nullptr, false); }\n',
         'component template', ''),
        # SetupAttachment: nodes in a cycle are reached from no root node, so none is made; an inherited component's
        # node is its own class's to place; a socket that is no literal name would be dropped.
        ('AttachCycle', '  UE_COMPONENT(USceneComponent, A);\n  UE_COMPONENT(USceneComponent, B);\n'
                        '  UE_DEFAULTS { A->SetupAttachment(B); B->SetupAttachment(A); }\n', 'a cycle', ''),
        ('AttachInherited', '', 'an inherited one', CLASH_BASE + 'class AttachKid : public ClashBase {\npublic:\n'
                                                    '  UE_COMPONENT(USceneComponent, Own);\n'
                                                    '  UE_DEFAULTS { Lamp->SetupAttachment(Own); }\n};\n'),
        ('AttachSocket', '  UE_COMPONENT(USceneComponent, A);\n  UE_COMPONENT(USceneComponent, B);\n  FName Where;\n'
                         '  UE_DEFAULTS { B->SetupAttachment(A, Where); }\n', 'literal name', '')):
    refused(mod, body, why, top)
print('ok  refused: component names already taken under the actor (DefaultSceneRoot and its variable, a parent\n'
      '    Blueprint\'s component, a native default subobject or member, a game Blueprint\'s SCS node), a spawn in\n'
      '    UserConstructionScript, AddComponent by template name, SetupAttachment in a cycle, of an inherited component\n'
      '    or at a computed socket')


def refused_or_warned(mod, body, why, top=''):
    """A mod the compiler must refuse, or build with a `warning:` line, either naming `why` (refused()'s source)."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, mod + '.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/%s");\n%s'
                    'class %s : public AActor {\npublic:\n%s};\n' % (mod, top, mod, body))
        proc = assetgen_compile([src, UEAPI, tmp])
        warned = [l for l in proc.stdout.splitlines() if 'warning:' in l and why in l]
        assert (proc.returncode != 0 and why in proc.stdout) or (proc.returncode == 0 and warned), (mod, proc.stdout)


# NODE-19 through a helper only the construction script calls: the editor does not look into a helper, but on this
# path its spawn returns None all the same, so the compiler warns: the helper may run elsewhere too.
refused_or_warned('UcsSpawnHelper', '  AActor* Made;\n'
                  '  void Build() { Made = SpawnActorDeferred<AActor>(AActor::StaticClass(), FTransform()); }\n'
                  '  void UserConstructionScript() { Build(); }\n', 'Build spawns an actor and UserConstructionScript calls it',
                  OBJECTS)
print('ok  UcsSpawnHelper: a helper UserConstructionScript calls that spawns is warned about')


# ---- PROPS: property fields and tagged defaults - bool layout, field classes, flags, element sizes, containers
# (invariant_rules/properties.py)

def fstring_at(raw, o):
    """An FString at o as the engine reads it: int32 length, then that many ANSI bytes (each widened to a TCHAR, so
    Latin-1) or, for a negative length, UTF-16 code units; the terminator dropped. (text, end)."""
    import struct
    n = struct.unpack_from('<i', raw, o)[0]; o += 4
    if n == 0: return '', o
    if n < 0: return raw[o:o - 2 * n - 2].decode('utf-16-le'), o - 2 * n
    return raw[o:o + n - 1].decode('latin-1'), o + n


def text_at(raw, o):
    """An FText at o as FText::SerializeText reads it (Core/Private/Internationalization/Text.cpp 882-995): int32 Flags,
    int8 HistoryType, then the history. Base (0) is Namespace, Key, SourceString; any type outside 0..12 - None is -1 -
    is bool32 bHasCultureInvariantString and the string when set. (display string, end); another history raises."""
    import struct
    kind = struct.unpack_from('<b', raw, o + 4)[0]; o += 5
    if kind == 0:
        _, o = fstring_at(raw, o); _, o = fstring_at(raw, o)
        return fstring_at(raw, o)
    assert not 0 < kind <= 12, 'FText history %d: not a literal' % kind
    has = struct.unpack_from('<i', raw, o)[0]; o += 4
    return fstring_at(raw, o) if has else ('', o)


def prop_text_defaults():
    """An FText default reads back as the text the source gives it: a literal, ASCII or not, is the text; an empty or
    unset one is empty (no tag at all, the class default, is empty too). Read as the engine reads a TextProperty tag
    (TextProperty.cpp 103-107), so any history that carries the same string passes. A text map value too."""
    import invariants, struct
    pkg = invariants.Package(asset('PropDefaults'))
    cdo = pkg.find('Default__PropDefaults_C')
    got = {}
    for name in ('Label', 'Wide', 'Empty', 'Blank'):
        t = pkg.tag(cdo, name)
        if t is None: got[name] = ''; continue
        assert t['type'] == 'TextProperty', t
        got[name], end = text_at(t['value'], 0)
        assert end == t['size'], (name, t['value'].hex())
    assert got == {'Label': 'Ready', 'Wide': 'Größe', 'Empty': '', 'Blank': ''}, got
    t = pkg.tag(cdo, 'Labels')
    raw = t['value']
    removed, n = struct.unpack_from('<ii', raw, 0)
    key = struct.unpack_from('<i', raw, 8)[0]
    text, end = text_at(raw, 12)
    assert (removed, n, key, text, end) == (0, 1, 1, 'one', len(raw)), raw.hex()
    print('ok  PropDefaults: FText defaults read back as their literal (ASCII and UTF-16), empty ones empty, a map\'s too')


def reset_predicates(flags):
    """What AActor::ResetPropertiesForConstruction asks of a variable before it resets it to its default
    (Engine/Private/ActorConstruction.cpp 111-112): (bCanEditInstanceValue, bCanBeSetInBlueprints)."""
    return (not flags & 0x10000 and bool(flags & 0x1)), (bool(flags & 0x4) and not flags & 0x10)


def prop_flag_predicates():
    """Plain, const and component variables carry the descriptive flags the engine reads at run time as the editor's
    do - not the raw bits, the two predicates built from them: a plain variable (editor 0x10005) and a component's
    (editor 0x400080004) are settable from a Blueprint and not instance-editable, a const one (BlueprintReadOnly,
    0x10015) neither."""
    import invariants
    want = {'Plain': (False, True), 'Limit': (False, False), 'Root': (False, True), 'Mesh': (False, True),
            'Ticks': (False, True), 'Lamp': (False, True), 'Rocks': (False, True), 'Grass': (False, True)}
    seen = {}
    for mod in ('PropDefaults', 'CompTest'):
        pkg = invariants.Package(asset(mod))
        st = pkg.struct(pkg.find(mod + '_C'))
        for p in st.props:
            if p.name in want: seen[p.name] = reset_predicates(p.flags)
    assert seen == want, seen
    print('ok  PropDefaults / CompTest: plain, const and component variables reset at construction as the editor\'s do')


def prop_hash_keys():
    """Set elements and map keys a mod declares are of hashable types (the engine check()s CPF_HasGetValueTypeHash on
    every Add / Find and on loading a default: Property.cpp 1517-1522): a name, an int, FVector (which has
    GetTypeHash), and a nested array - wrapped in a UserDefinedStruct, which always hashes."""
    import invariants
    pkg = invariants.Package(asset('PropDefaults'))
    props = {p.name: p for p in pkg.struct(pkg.find('PropDefaults_C')).props}
    keys = {n: (props[n].subs[0].type, pkg.class_of(props[n].subs[0].ref) if props[n].subs[0].ref else None,
                pkg.obj(props[n].subs[0].ref)['name'] if props[n].subs[0].ref else None)
            for n in ('Names', 'Labels', 'Spots', 'Lists')}
    assert keys['Names'][0] == 'NameProperty' and keys['Labels'][0] == 'IntProperty', keys
    assert keys['Spots'] == ('StructProperty', 'ScriptStruct', 'Vector') and keys['Lists'][:2] == ('StructProperty', 'UserDefinedStruct'), keys
    print('ok  PropDefaults: set elements and map keys of hashable types, a nested array as a struct')


EKIND = 'enum class EKind : uint8 { A, B };\nUE_ENUM(EKind);\n'


def prop_enum_casts():
    """An enum default must be one of its enumerators: the loader maps any other name to _MAX (EnumProperty.cpp
    129-170), and a value past the enum has no name to write. A cast of an out-of-range integer is refused, naming the
    member, whether it reaches the default directly, through a constexpr or as a UE_DEFAULTS on a native member.
    Today the compiler refuses every enum cast in a default (even static_cast<EKind>(1)) as not a value known at build
    time; the day it accepts casts, these must still be refused (or, per sugar-with-a-warning, change this test)."""
    refused('PropEnumCast', '  EKind K = static_cast<EKind>(9);\n', 'K: ', top=EKIND)
    refused('PropEnumCastC', '  EKind K = (EKind)9;\n', 'K: ', top=EKIND)
    refused('PropEnumCastK', '  static constexpr EKind kBad = static_cast<EKind>(9);\n  EKind K = kBad;\n', 'K: ', top=EKIND)
    refused('PropEnumCastNative', '  UE_DEFAULTS { AutoReceiveInput = (EAutoReceiveInput)9; }\n', 'AutoReceiveInput: ')
    print('ok  an out-of-range enum default is refused: static_cast, C cast, constexpr, UE_DEFAULTS on a native member')


def fname_at(names, raw, o):
    import struct
    i, n = struct.unpack_from('<ii', raw, o)
    return (names[i] + ('_%d' % (n - 1) if n else '')).lower()


def loaded_container(pkg, cdo, name, kind, start):
    """The value the loader leaves for tag `name` of CDO export cdo, from `start` - the parent CDO's loaded value for an
    inherited property, empty for the class's own (UnrealType.h 439-446: a defaults pointer only inside the parent's
    layout): the listed removals taken out, then each element added (a set, PropertySet.cpp 285-358) or each pair set
    (a map, PropertyMap.cpp 316-400). `name` may be a tuple, a property and then members of structs written as tags: a
    struct loads each member over the parent's struct's (Class.cpp 2775), so the same reading holds there. No tag (or
    no member's tag): start unchanged. TSet<int32> and TMap<FName, int32> only."""
    import struct
    path = (name,) if isinstance(name, str) else name
    t = pkg.tag(cdo, path[0])
    for member in path[1:]:
        if t is None: break
        t = next((x for x in pkg.tags(cdo, t['at']) if x['name'] == member), None)
    if t is None: return start
    raw, o = t['value'], 0
    out = set(start) if kind == 'set' else dict(start)
    removed = struct.unpack_from('<i', raw, o)[0]; o += 4
    for _ in range(removed):
        if kind == 'set': out.discard(struct.unpack_from('<i', raw, o)[0]); o += 4
        else: out.pop(fname_at(pkg.names, raw, o), None); o += 8
    n = struct.unpack_from('<i', raw, o)[0]; o += 4
    for _ in range(n):
        if kind == 'set': out.add(struct.unpack_from('<i', raw, o)[0]); o += 4
        else: out[fname_at(pkg.names, raw, o)] = struct.unpack_from('<i', raw, o + 8)[0]; o += 12
    assert o == len(raw), (name, raw.hex())
    return out


def prop_set_delta():
    """A child Blueprint's own default for an inherited TSet / TMap is what its CDO loads: the parent's {1, 2} and
    {a: 1, c: 3} become {2, 3} and {a: 5, b: 2}, read as the loader reads the child's tag on top of the parent CDO's
    loaded value (see loaded_container). So does one inside an inherited struct the child assigns whole (Held), whose
    members load over the parent's struct's; Deep, which it leaves alone, keeps the parent's. Any encoding that loads
    that value passes."""
    import invariants
    base = asset('PropSetDelta')
    parent = invariants.Package(os.path.join(os.path.dirname(base), 'PropSetBase'))
    child = invariants.Package(base)
    pc, cc = parent.find('Default__PropSetBase_C'), child.find('Default__PropSetDelta_C')
    for where, ids_want, score_want in (((), {2, 3}, {'a': 5, 'b': 2}), (('Held',), {2, 3}, {'a': 5, 'b': 2}),
                                        (('Deep', 'In'), {1, 2}, {'a': 1, 'c': 3})):
        ids = loaded_container(child, cc, where + ('Ids',), 'set', loaded_container(parent, pc, where + ('Ids',), 'set', set()))
        score = loaded_container(child, cc, where + ('Score',), 'map', loaded_container(parent, pc, where + ('Score',), 'map', {}))
        label = '.'.join(where + ('',))
        assert ids == ids_want, 'PropSetDelta loads %sIds = %s, the source says %s' % (label, sorted(ids), sorted(ids_want))
        assert score == score_want, 'PropSetDelta loads %sScore = %s, the source says %s' % (label, score, score_want)
    keeps_invariants(base)
    print('ok  PropSetDelta: an inherited TSet / TMap default, a property or one in a struct assigned whole, lists the '
          'parent\'s elements it drops as removed, and loads as its own value, not the union')


def prop_enum_class():
    """A property of a native `enum class : uint8` is an EnumProperty over a ByteProperty, as the editor makes it
    (KismetCompilerMisc.cpp 1071-1094; every EnumProperty of the game's Blueprints is over a ByteProperty): a variable, a
    parameter, a return value, a local, a container's element and key, a UE_STRUCT member, a delegate's parameter and an
    override's parameter, which then has its native parent's type (FEnumProperty::SameType, EnumProperty.cpp 395-398). A
    namespaced enum (EAttachLocation) stays a ByteProperty. Its tags - the CDO's, a UE_DEFAULTS one on a native
    EnumProperty member, an array's and a map's, a UserDefinedStruct's default and a native struct's member - are
    EnumProperty tags (the GetID(), PropertyTag.cpp 17, 30-36) naming the enum and holding the enumerator's FName; an
    empty array, set or map of it, its counts alone. Its values run as bytes: a switch, a compare, a cast, a map lookup
    and a native struct literal."""
    import invariants, runvm
    base = asset('PropEnumClass')
    folder = os.path.dirname(base)
    pkg = invariants.Package(base)
    rule_enum = '/Script/Engine.EAttachmentRule'

    def is_enum_class(p, enum, owner=pkg):
        return (p.type, [s.type for s in p.subs], owner.path(p.enum) if p.enum else owner.path(p.ref)) == \
            ('EnumProperty', ['ByteProperty'], enum)
    props = {p.name: p for p in pkg.struct(pkg.find('PropEnumClass_C')).props}
    assert is_enum_class(props['Rule'], rule_enum), \
        'Rule is a %s over %s, not an EnumProperty over a ByteProperty' % (props['Rule'].type, [s.type for s in props['Rule'].subs])
    assert (props['Where'].type, pkg.path(props['Where'].ref)) == ('ByteProperty', '/Script/Engine.EAttachLocation'), \
        'Where, of a namespaced enum, is a %s' % props['Where'].type
    # EAbilityIndex's form comes off one parameter the SDK respells (Index -> Index_0); a ByteProperty means it was lost
    assert is_enum_class(props['Ability'], '/Script/FSD.EAbilityIndex'), 'Ability is a %s' % props['Ability'].type
    for name, sub in (('Order', 0), ('Seen', 0), ('Cost', 0)):
        assert is_enum_class(props[name].subs[sub], rule_enum), '%s holds %s' % (name, props[name].subs[sub].type)
    fn_props = lambda owner, fn: {p.name: p for p in owner.struct(owner.find(fn)).props}
    for fn, names in (('Pick', ('In', 'ReturnValue')), ('Score', ('In', 'Local')), ('Weight', ('K',)),
                      ('OnRule__DelegateSignature', ('Param0',))):
        for name in names:
            assert is_enum_class(fn_props(pkg, fn)[name], rule_enum), '%s.%s is a %s' % (fn, name, fn_props(pkg, fn)[name].type)
    assert is_enum_class(fn_props(pkg, 'Weight')['W'].subs[0], rule_enum), 'a local map\'s key'
    crystal = invariants.Package(os.path.join(folder, 'PropEnumCrystal'))
    assert is_enum_class(fn_props(crystal, 'Receive_EnteredState')['State'], '/Script/FSD.ECoreCorruptionCrystalState', crystal), \
        'the override\'s parameter is not its native parent\'s EnumProperty'
    slot = invariants.Package(os.path.join(folder, 'FRuleSlot'))
    member = next(p for p in slot.struct(0).props if p.name.startswith('Rule_'))
    assert is_enum_class(member, rule_enum, slot), 'the UE_STRUCT member is a %s' % member.type
    t = next(t for t in slot.struct(0).defaults if t['name'].startswith('Rule_'))
    assert (t['type'], t['enum'], fname_at(slot.names, t['value'], 0)) == ('EnumProperty', 'EAttachmentRule', 'eattachmentrule::snaptotarget'), t
    # Its empty array, set and map of the enum: the default instance tags each with its counts alone.
    empty = {t['name'].split('_')[0]: t['value'] for t in slot.struct(0).defaults
             if t['name'].split('_')[0] in ('Vis', 'Met', 'Toll')}
    assert empty == {'Vis': bytes(4), 'Met': bytes(8), 'Toll': bytes(8)}, empty

    cdo = pkg.find('Default__PropEnumClass_C')
    for name, enum, value in (('Rule', 'EAttachmentRule', 'keepworld'), ('Ability', 'EAbilityIndex', 'esecondary'),
                              ('UpdateOverlapsMethodDuringLevelStreaming', 'EActorUpdateOverlapsMethod', 'alwaysupdate'),
                              ('Where', 'EAttachLocation', 'snaptotarget')):
        t = pkg.tag(cdo, name)
        want = 'ByteProperty' if name == 'Where' else 'EnumProperty'
        assert t and t['type'] == want and t['enum'] == enum, (name, t)
        assert fname_at(pkg.names, t['value'], 0).split('::')[-1] == value, (name, fname_at(pkg.names, t['value'], 0))
    order, cost = pkg.tag(cdo, 'Order'), pkg.tag(cdo, 'Cost')
    assert order['inner'] == 'EnumProperty' and [fname_at(pkg.names, order['value'], 4 + 8 * k) for k in range(2)] == \
        ['eattachmentrule::snaptotarget', 'eattachmentrule::keeprelative'], order
    assert (cost['inner'], cost['value_type']) == ('EnumProperty', 'IntProperty') and \
        fname_at(pkg.names, cost['value'], 8) == 'eattachmentrule::keepworld', cost
    shake = {u['name']: u for u in pkg.tags(cdo, pkg.tag(cdo, 'Shake')['at'])}
    assert (shake['Type']['type'], shake['Type']['enum'], fname_at(pkg.names, shake['Type']['value'], 0)) == \
        ('EnumProperty', 'ECameraShakeDurationType', 'ecamerashakedurationtype::custom'), shake['Type']
    for b in (base, crystal.base, slot.base):
        keeps_invariants(b)

    for rule in (1, 2):
        for In in (0, 1, 2):
            for M in (0, 1):
                local = In if M == 0 else 2
                want = 12 if local == 0 else 21 if local == 1 else 100 if local == rule else local
                got = run(base, 'Score', {'Rule': rule}, In=In, M=M)[0]
                assert got == want, 'Score(%d, %d) with Rule %d = %r, want %r' % (In, M, rule, got, want)
    assert [run(base, 'Weight', K=k)[0] for k in (0, 1, 2)] == [-1, 3, 5]
    vm = runvm.VM(base)
    vm.struct_const = lambda name, vals: runvm.Written(zip(['Duration', 'Type'], vals)) if name == 'CameraShakeDuration' \
        else runvm.Struct(name, vals)
    assert [vm.call('ShakeType', M=m) for m in (0, 1)] == [0, 1]
    print('ok  PropEnumClass: a native enum class is an EnumProperty over a ByteProperty wherever it lands, tagged '
          'EnumProperty, and runs as a byte')


prop_text_defaults()
prop_flag_predicates()
prop_hash_keys()
prop_enum_casts()
prop_set_delta()
prop_enum_class()


def value_init_scalar():
    """Braces or `T()` around something that is not a struct are C++'s: `E R{}`, `T()` and `{}` the type's zero
    (value-initialisation), `{V}` the value V - an enum, an own UE_ENUM, an int, an int64, a byte, a float, a bool and an
    object pointer, in a local, an assignment, an argument, a return value, an array element and a member of a braced
    UE_STRUCT (`{}` there is the member's zero, not its default, and a member the braces leave out keeps its default,
    an enum's included: runscript reads a UserDefinedStruct's enum defaults), and as the default of a member and of a UE_STRUCT
    member (a zero one writes no tag on the class default object; a UserDefinedStruct's default instance tags every
    member: with no defaults to diff against, Class.cpp 1547 writes each), and as a braced asset's value, where `{}` is
    written as the zero it is. A UE_STRUCT's `T()` default is its defaults.
    A `{}` for a member of a class type - an engine struct, a TArray, an FName, a UE_STRUCT - is that type's fresh value
    too, not the member's default: in a function body (where the UE_STRUCT's frame local starts as its default instance,
    UUserDefinedStruct::InitializeStruct, so a member left unstored would read its default), in a class default and in
    a braced asset. An empty container is a value: assigned, passed, constructed and returned."""
    import struct
    base = asset('ValueInitScalar')
    keeps_invariants(base)
    for fn, want in (('EnumBraces', 0), ('EnumEqBraces', 0), ('EnumParens', 0), ('EnumValue', 1), ('EnumArg', 20),
                     ('EnumAssign', 0), ('EnumReturn', 10), ('OwnEnum', 20), ('IntBraces', 7), ('IntParens', 0),
                     ('WideBraces', 0), ('Elements', 10), ('SlotBraces', 0), ('SlotOmit', 215)):
        for m in (0, 3):
            got = run(base, fn, {'Held': 1}, M=m)[0]
            assert got == want + m, 'ValueInitScalar.%s(%d) = %r, want %r' % (fn, m, got, want + m)
    for fn, want in (('NativeBraces', lambda m: 10 * m), ('NativeDesig', lambda m: 10 * m),
                     ('NativeOmit', lambda m: 10 * m + 2), ('NativeMacro', lambda m: 10 * m), ('NativeArray', lambda m: m),
                     ('NativeNested', lambda m: 12 + m),
                     ('NativeName', lambda m: True), ('EmptyAssign', lambda m: m), ('EmptyArg', lambda m: m)):
        for m in (0, 3):
            got = run(base, fn, {'Items': [1, 2]}, M=m)[0]
            assert got == want(m), 'ValueInitScalar.%s(%d) = %r, want %r' % (fn, m, got, want(m))
    assert run(base, 'FloatBraces', F=2.0)[0] == 3.5 and run(base, 'BoolBraces', M=1)[0] is False
    assert run(base, 'ObjBraces', M=0)[0] == 1 and run(base, 'ObjAssign', {'Seen': None}, M=0)[0] == 1
    pkg = invariants.Package(base)
    cdo = pkg.find('Default__ValueInitScalar_C')
    for name in ('Rule', 'Count', 'Who', 'Parens', 'RuleParens', 'Fresh'):
        assert not pkg.tag(cdo, name), 'the zero default of %s writes a tag: %r' % (name, pkg.tag(cdo, name))
    cleared = {u['name'].split('_')[0]: u for u in pkg.tags(cdo, pkg.tag(cdo, 'Cleared')['at'])}
    assert fname_at(pkg.names, cleared['Kept']['value'], 0) == 'eattachmentrule::keeprelative', cleared['Kept']
    assert struct.unpack('<i', cleared['Five']['value'])[0] == 0, cleared['Five']
    kept, seven, half = pkg.tag(cdo, 'Kept'), pkg.tag(cdo, 'Seven'), pkg.tag(cdo, 'Half')
    assert kept and fname_at(pkg.names, kept['value'], 0) == 'eattachmentrule::keepworld', kept
    assert seven and struct.unpack('<i', seven['value'])[0] == 7, seven
    assert half and struct.unpack('<f', half['value'])[0] == 0.5, half
    slot = invariants.Package(os.path.join(os.path.dirname(base), 'FValueSlot'))
    tags = {t['name'].split('_')[0]: t for t in slot.struct(0).defaults}
    assert fname_at(slot.names, tags['Zeroed']['value'], 0) == 'eattachmentrule::keeprelative', tags['Zeroed']
    assert fname_at(slot.names, tags['Kept']['value'], 0) == 'eattachmentrule::keepworld', tags['Kept']
    assert [struct.unpack('<i', tags[n]['value'])[0] for n in ('Nil', 'Five')] == [0, 5], (tags['Nil'], tags['Five'])
    asset_pkg = invariants.Package(os.path.join(os.path.dirname(base), 'VD_Braces'))
    named = {t['name']: t for t in asset_pkg.tags(asset_pkg.find('VD_Braces'))}
    assert set(named) == {'Count', 'Rule'} and struct.unpack('<i', named['Count']['value'])[0] == 0, named
    assert fname_at(asset_pkg.names, named['Rule']['value'], 0) == 'eattachmentrule::keeprelative', named['Rule']

    def fresh(pkg, i, tags, where):
        """V zero, L empty, In FValueIn's own defaults (1, 2), N None: each `{}` of the value tagged in tags."""
        inner = {u['name'].split('_')[0]: u for u in pkg.tags(i, tags['In']['at'])}
        assert struct.unpack('<ff', tags['V']['value']) == (0.0, 0.0), (where, tags['V'])
        assert struct.unpack('<i', tags['L']['value'][:4])[0] == 0, (where, tags['L'])
        assert [struct.unpack('<i', inner[n]['value'])[0] for n in ('P', 'Q')] == [1, 2], (where, inner)
        assert fname_at(pkg.names, tags['N']['value'], 0) == 'none', (where, tags['N'])
    held = {u['name'].split('_')[0]: u for u in pkg.tags(cdo, pkg.tag(cdo, 'NativeHeld')['at'])}
    assert struct.unpack('<i', held['A']['value'])[0] == 7, held['A']
    fresh(pkg, cdo, held, 'NativeHeld')
    native = invariants.Package(os.path.join(os.path.dirname(base), 'VN_Braces'))
    named = {t['name']: t for t in native.tags(native.find('VN_Braces'))}
    assert set(named) == {'Count', 'V', 'L', 'In', 'N'}, named
    fresh(native, native.find('VN_Braces'), named, 'VN_Braces')
    omit = invariants.Package(os.path.join(os.path.dirname(base), 'VN_Omit'))
    assert [t['name'] for t in omit.tags(omit.find('VN_Omit'))] == ['Count'], 'a member VN_Omit leaves out is written'


value_init_scalar()
print('ok  ValueInitScalar: braces or T() around an enum, a number or a pointer are its zero, or the value braced; '
      '{} for a struct, container or name member is its fresh value, not the default; an empty container is a value')


def tenum_value_init():
    """TEnumValueInit: `{}` and `TEnum<E>()` are a TEnum<E>'s zero enumerator, as they are an E's: assigned (Held's
    default is Two, Rule's KeepWorld), passed, returned, as a struct literal's member over a non-zero default (P One,
    R KeepWorld; a member left out keeps its default), as an array's element, as a conditional's arm, and as a game
    function's TEnum<E> arguments."""
    base = asset('TEnumValueInit')
    keeps_invariants(base)
    for fn, want in (('Assign', lambda m: m), ('AssignParens', lambda m: m), ('Arg', lambda m: 300 + m),
                     ('Return', lambda m: m), ('Literal', lambda m: (m + 1) * 10), ('Designated', lambda m: 350 + m),
                     ('Kept', lambda m: 351 + m), ('Elements', lambda m: 20 + m), ('Choose', lambda m: 0 if m == 0 else 2)):
        for m in (0, 3):
            got = run(base, fn, {'Held': 2, 'Rule': 1}, M=m)[0]
            assert got == want(m), 'TEnumValueInit.%s(%d) = %r, want %r' % (fn, m, got, want(m))
    runscript.MATH['K2_DetachFromActor'] = lambda *a: None
    try:
        del runscript.CALLS[:]
        run(base, 'Detach')
        assert runscript.CALLS == [('K2_DetachFromActor', (0, 0, 0))], runscript.CALLS
    finally:
        del runscript.MATH['K2_DetachFromActor']


tenum_value_init()
print('ok  TEnumValueInit: {} and TEnum<E>() are the zero enumerator wherever a TEnum<E> is assigned, passed or returned')


def tenum_holders():
    """TEnumHolders: a TArray<TEnum<E>> has an array's methods (Add, Num, Contains, on a variable, a local and a
    parameter), and a dispatcher with a TEnum<E> parameter binds and broadcasts; Name() on an element is still E's."""
    base = asset('TEnumHolders')
    keeps_invariants(base)
    names = []
    vm = VM(base, {'GetEnumeratorName': lambda vm, ctx, e, v: names.append((e, v)) or 'Three'}, Items=[1, 3], Seen=0)
    assert vm.call('Fire', 4) == 24 and vm.self.vars['Seen'] == 24, vm.self.vars
    assert vm.call('Count', 5) == 36 and vm.self.vars['Items'] == [1, 3, 2], vm.self.vars
    assert vm.call('Local', 5) == 127, vm.call('Local', 5)
    assert vm.call('NameOf') == 'Three' and names == [('EThPick', 3)], names


tenum_holders()
print('ok  TEnumHolders: a TArray of TEnum<E> and a dispatcher with a TEnum<E> parameter keep their own methods')


def tenum_map_key():
    """TEnumMapKey: a TMap keyed by a TEnum<E> whose value is a template too (TSubclassOf, TEnum, TArray, TSoftObjectPtr)
    is a map of the enum to that value, as a variable, a local and a parameter."""
    import invariants
    base = asset('TEnumMapKey')
    keeps_invariants(base)
    pkg = invariants.Package(base)
    props = {p.name: [s.type for s in p.subs] for p in pkg.struct(pkg.find('TEnumMapKey_C')).props}
    assert props['Classes'] == ['ByteProperty', 'ClassProperty'], props
    # A container value is held in the wrapper struct a nested container needs.
    assert props['Rules'] == ['ByteProperty', 'EnumProperty'] and props['Lists'] == ['ByteProperty', 'StructProperty'], props
    for fn, vars_, parms, want in (('Rule', {'Rules': {1: 1}}, {}, 1), ('Local', {}, {}, 20), ('Pass', {}, {'R': {2: 2}}, 2)):
        got = run(base, fn, vars_, M=3, **parms)[0]
        assert got == want + 3, (fn, got, want + 3)


tenum_map_key()
print('ok  TEnumMapKey: a TMap keyed by a TEnum<E> takes a templated value type')


# -- pending

for _mod, _body in (('PropSetBool', '  TSet<bool> Flags;\n'), ('PropMapBool', '  TMap<bool, int32> ByFlag;\n'),
                    ('PropSetText', '  TSet<FText> Labels;\n'), ('PropMapText', '  TMap<FText, int32> ByLabel;\n'),
                    ('PropSetHit', '  TSet<FHitResult> Hits;\n'), ('PropSetRotator', '  TSet<FRotator> Turns;\n'),
                    ('PropSetBoolLocal', '  int32 F() { TSet<bool> S; S.Add(true); return S.Num(); }\n'),
                    ('PropSetBoolParam', '  int32 F(TSet<bool> S) { return S.Num(); }\n')):
    refused(_mod, _body, 'cannot hash, and the engine hashes each one')
print('ok  a set element / map key that cannot hash is refused: bool, FText, a native struct without GetTypeHash; as '
      'a variable, a local and a parameter')


def prop_enum_forms():
    """A variable of a native enum whose form only a native delegate's parameter or a container's element shows is
    the property the editor makes of it (KismetCompilerMisc.cpp 1071-1094): an EnumProperty over a ByteProperty for an
    `enum class`, a ByteProperty naming the enum for a TEnumAsByte one. The forms are the object dump's delegate
    signatures' and the game's own packages' (PropEnumForms.cpp says where each comes from)."""
    import invariants
    base = asset('PropEnumForms')
    pkg = invariants.Package(base)
    props = {p.name: p for p in pkg.struct(pkg.find('PropEnumForms_C')).props}
    want = {'Severity': 'EnumProperty', 'QuartzEvent': 'EnumProperty', 'PurchaseStatus': 'EnumProperty',
            'Treasure': 'EnumProperty', 'AppState': 'ByteProperty', 'PathEvent': 'ByteProperty',
            'QueryStatus': 'ByteProperty', 'PurchaseState': 'ByteProperty', 'Cleaned': 'ByteProperty'}
    got = {n: (props[n].type, [s.type for s in props[n].subs]) for n in want}
    assert got == {n: (t, ['ByteProperty'] if t == 'EnumProperty' else []) for n, t in want.items()}, got
    keeps_invariants(base)


prop_enum_forms()
print('ok  PropEnumForms: an enum only a native delegate\'s parameter or a container\'s element shows the form of is '
      'the property the editor makes')


# ---- TYPES: UE_STRUCT default instances, UE_ENUM payloads and names (invariant_rules/user_types.py)

import struct
import invariants
from invariant_rules import user_types as types_rules


def tag_values(pkg, i, tags):
    """A tag list as {name without the member suffix: value}, the way the engine reads it back: ints, floats, bools,
    names, strings, an enum value as its name, an engine vector as a tuple, an int array as a list, and a struct value
    as a dict of its own tags. An enum value is its short name, which it loads by as well as by the full one."""
    out = {}
    for t in tags:
        ty, v = t['type'], bytes(t['value'])
        if ty == 'IntProperty': x = struct.unpack('<i', v)[0]
        elif ty == 'FloatProperty': x = struct.unpack('<f', v)[0]
        elif ty == 'BoolProperty': x = bool(t['bool'])
        elif ty == 'NameProperty' or ty in ('ByteProperty', 'EnumProperty') and t['size'] == 8:
            n, num = struct.unpack('<ii', v); x = pkg.names[n] + ('_%d' % (num - 1) if num else '')
            if ty != 'NameProperty': x = x.split('::')[-1]   # an enum value loads by either name (UEnum::GetIndexByName)
        elif ty == 'StrProperty': x = v[4:-1].decode('latin-1')
        elif ty == 'StructProperty' and t['struct'] == 'Vector': x = struct.unpack('<3f', v)
        elif ty == 'StructProperty': x = tag_values(pkg, i, pkg.tags(i, t['at']))
        elif ty == 'ArrayProperty' and t['inner'] == 'IntProperty': x = list(struct.unpack_from('<%di' % struct.unpack_from('<i', v)[0], v, 4))
        else: x = ('?', ty)
        out[re.sub(r'_\d+_[0-9A-F]{32}$', '', t['name'])] = x
    return out


def uds_defaults(base):
    """The default instance a UserDefinedStruct package's Data stream holds, as tag_values reads it."""
    pkg = invariants.Package(base)
    st = pkg.struct(0)
    assert st.kind == 'UserDefinedStruct' and hasattr(st, 'defaults'), (base, st.kind)
    return tag_values(pkg, 0, st.defaults)


def uds_init_defaults():
    """Every member initializer of a UE_STRUCT is in its default instance, the Data stream the engine copies into each
    new value of the struct (UUserDefinedStruct::InitializeStruct). A member at its type's default may be left out, and
    a struct member may be left out or written without the members at its own struct's defaults: the load starts
    each member at its own default, a struct member at its struct's default instance."""
    here = os.path.dirname(asset('UdsInitTest'))
    gauge = uds_defaults(os.path.join(here, 'FUdsGauge'))
    assert gauge.get('Kills') == 9 and gauge.get('Time', 0.0) == 0.0 and set(gauge) <= {'Kills', 'Time'}, gauge
    tuned = uds_defaults(os.path.join(here, 'FUdsTuned'))
    want = {'Hp': 100, 'Rate': 0.25, 'bOn': True, 'Tag': 'Hot', 'Label': 'x', 'Pos': (1.0, 2.0, 3.0), 'Seq': [4, 5]}
    assert {k: tuned.get(k) for k in want} == want, tuned
    assert tuned['Dial'] == 'Low' and tuned.get('Zero', 0) == 0, tuned
    loaded = lambda v: {'Kills': v.get('Kills', 9), 'Time': v.get('Time', 0.0)}      # over FUdsGauge's own defaults
    assert loaded(tuned['Inner']) == {'Kills': 9, 'Time': 2.5} and loaded(tuned.get('Plain', {})) == {'Kills': 9, 'Time': 0.0}, tuned
    print('ok  UdsInitTest: every UE_STRUCT member initializer is in the struct\'s default instance, a struct member\'s too')
    # The class's members: a plain one takes the struct's defaults from InitializeValue, so its CDO tag may be left
    # out; a designated default changes the members it names, nested ones included, and leaves the rest at theirs.
    base = asset('UdsInitTest')
    pkg = invariants.Package(base)
    cdo = tag_values(pkg, pkg.find('Default__UdsInitTest_C'), pkg.tags(pkg.find('Default__UdsInitTest_C')))
    over = lambda d, v: {k: (over(d[k], v[k]) if isinstance(d.get(k), dict) and isinstance(v.get(k), dict) else v.get(k, d[k])) for k in d}
    full = over(dict(tuned, Zero=0, Inner=loaded(tuned['Inner']), Plain=loaded(tuned.get('Plain', {}))), {})
    assert over(full, cdo.get('Tuned', {})) == full, cdo.get('Tuned')
    braced = over(full, cdo['Braced'])
    assert braced == dict(full, Hp=7, Plain={'Kills': 9, 'Time': 0.5}), braced
    print('ok  UdsInitTest: a designated default of a UE_STRUCT member names what it changes; the rest keep the initializers')
    for name in ('UdsInitTest', 'FUdsTuned', 'FUdsGauge', 'EUdsDial'):
        keeps_invariants(os.path.join(here, name))
    print('ok  UdsInitTest: its struct, enum and class packages keep every invariants.py rule')


def enum_net_store():
    """A replicated UE_ENUM variable and a server RPC's UE_ENUM parameter, run as the server: the store wakes the object
    and holds the enumerator's value; the RPC called on the authority runs its body with the value passed. Every
    constant that reaches them is within the enum (enum_net_values: NetSerializeItem sends only the bits the enum's
    largest value needs)."""
    base = asset('EnumNetStore')
    vm = VM(base)
    vm.call('Shift')
    assert vm.self.vars.get('Gear') == 2, vm.self.vars
    assert [l[0] for l in vm.log if l[1] is vm.self][:2] == ['FlushNetDormancy', 'set'], vm.log
    vm = VM(base)
    vm.call('Ask')
    assert vm.self.vars.get('Gear') == 1 and vm.self.vars.get('Asked') == 1, vm.self.vars
    pkg = invariants.Package(base)
    stores = [n for i, st in invariants.functions(pkg) for n in invariants.statements(pkg, i)[0]
              if n.op == 0x0F and types_rules.const_value(pkg, i, n.kids[1]) is not None
              and (types_rules.field_prop(pkg, n.ops[0][2], n.ops[0][1][-1]) or (0, None))[1] is not None
              and types_rules.field_prop(pkg, n.ops[0][2], n.ops[0][1][-1])[1].flags & types_rules.CPF_Net]
    assert stores, 'no constant store into the replicated Gear for enum_net_values to judge'
    keeps_invariants(base)
    keeps_invariants(os.path.join(os.path.dirname(base), 'ENetGear'))
    print('ok  EnumNetStore: a replicated UE_ENUM variable and a server RPC\'s UE_ENUM parameter take the enumerators stored and passed')


def enum_refusals():
    """A UE_ENUM's closing _MAX takes the next value, so a uint8 enum's largest is 254 and an int32 / int64 enum's is
    one below the type's top; and a Blueprint enum is a uint8 (a byte variable), or an int32 / int64 one."""
    refused('EnumTop', '  int32 X;\n', 'is out of range', top='enum class EBig : uint8 { A, B = 255 };\nUE_ENUM(EBig);\n')
    refused('EnumTop', '  int32 X;\n', 'is out of range', top='enum class EWide : int32 { A = 2147483647 };\nUE_ENUM(EWide);\n')
    refused('EnumTop', '  int32 X;\n', 'is out of range', top='enum class EHuge : int64 { A = 9223372036854775807 };\nUE_ENUM(EHuge);\n')
    refused('EnumType', '  int32 X;\n', 'declare it', top='enum class EShort : int16 { A };\nUE_ENUM(EShort);\n')
    print('ok  a UE_ENUM with no room for its _MAX, or of another underlying type than uint8 / int32 / int64, is refused')


def enum_entries(base):
    """(entries [(FName, value)], CppForm) of the enum package at base, as UEnum::Serialize reads it."""
    pkg = invariants.Package(base)
    entries, form, end = types_rules.enum_body(pkg, 0)
    assert end == pkg.exports[0]['size'], (base, end, pkg.exports[0]['size'])
    return entries, form


def uds_local_init():
    base = asset('UdsLocalInit')
    for fn, parms, want in (('LocalHp', {}, 1109), ('BracedHp', {}, 120.0), ('LoopHp', {'N': 3}, 300), ('LoopHp', {'N': 0}, 0),
                            ('GaugeKills', {}, 9), ('TagAndSeq', {}, 12)):
        got = run(base, fn, **parms)[0]
        assert got == want, '%s(%s) = %r, want %r: a UE_STRUCT local starts at zero, not its defaults' % (fn, parms, got, want)
    keeps_invariants(base)


def enum_max_dup():
    """A declared <Enum>_MAX, the UE C++ idiom, is the sentinel itself: one entry of that name, the largest; any other
    value for it is refused."""
    base = asset('EnumMaxDup')
    entries, form = enum_entries(os.path.join(os.path.dirname(base), 'EMaxDupGear'))
    names = [n.lower() for n, _ in entries]
    assert len(set(names)) == len(names), 'an entry named twice: %s' % entries
    top = [(n, v) for n, v in entries if n.endswith('_MAX')]
    assert len(top) == 1 and all(v < top[0][1] for n, v in entries if n != top[0][0]), entries
    assert entries[:2] == [('EMaxDupGear::Low', 0), ('EMaxDupGear::High', 1)], entries
    keeps_invariants(os.path.join(os.path.dirname(base), 'EMaxDupGear'))
    keeps_invariants(base)
    refused('EnumMaxOff', '  int32 N = 0;\n', 'EMaxOff_MAX is the sentinel the engine adds',
            top='enum class EMaxOff : uint8 { A, B, EMaxOff_MAX = 7 };\nUE_ENUM(EMaxOff);\n')
    print('ok  EnumMaxDup: a declared <Enum>_MAX is the sentinel, one entry of its name; another value for it is refused')


def enum_names_across_mods():
    """No enumerator FName is cooked by two packages of the mods built here: UEnum::AddNamesToMasterList keeps the first
    of two in its one global map (Enum.cpp 83-94), so with both mods loaded, LookupEnumName answers for one of them."""
    owner = {}
    for b in invariants.packages([ROOT]):
        if os.sep + '_pending' + os.sep in os.path.normpath(b): continue
        pkg = invariants.Package(b)
        for i, e in enumerate(pkg.exports):
            if pkg.class_of(i + 1) == 'UserDefinedEnum':
                for n, _ in types_rules.enum_body(pkg, i)[0]: owner.setdefault(n, set()).add(pkg.package_name())
    both = {n: sorted(p) for n, p in owner.items() if len(p) > 1}
    assert not both, 'cooked by two packages: %s' % sorted(both.items())[:2]


uds_init_defaults()
enum_net_store()
enum_refusals()
uds_local_init()
print('ok  UdsLocalInit: a function with a UE_STRUCT local whose defaults are not zero is FUNC_HasDefaults, so the frame '
      'starts the local at the struct defaults')
enum_max_dup()
# An enumerator's FName is <Enum>::<Name>, kept in one global table where the first enum loaded wins
# (UEnum::AddNamesToMasterList): a mod enum named like the game's is refused, and no two test mods share one.
refused('EnumNativeClash', '  ClashNs::EDialogRestriction Mode = ClashNs::EDialogRestriction::SinglePlayerOnly;\n',
        'UE_ENUM(ClashNs::EDialogRestriction): /Script/FSD already has an enum EDialogRestriction',
        top='namespace ClashNs {\nenum class EDialogRestriction : uint8 { None, SinglePlayerOnly };\nUE_ENUM(EDialogRestriction);\n}\n')
enum_names_across_mods()
print('ok  enumerator names: a UE_ENUM named like a game enum is refused; no two test mods cook one name')


# ---- REPL: replication and RPCs (invariant_rules/replication.py)

import struct, tempfile
import invariants

NET_DIRECTIONS = 0x200000 | 0x1000000 | 0x4000        # FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast


def called(pkg, i):
    """(opcode, callee name) of every call in export i's script, nested ones included."""
    out = []
    for n in invariants.statements(pkg, i)[0]:
        if n.op not in (0x1B, 0x1C, 0x45, 0x46, 0x68): continue
        kind, v = n.ops[0][:2]
        out.append((n.op, v if kind == 'name' else pkg.obj(v)['name'] if v else None))
    return out


def compile_log(mod):
    """What the compiler printed for tests/pending/<mod>.cpp: pending_asset keeps only a refusal's reason, and a
    warning is what some of these gaps are closed by."""
    with tempfile.TemporaryDirectory() as tmp:
        return assetgen_compile([os.path.join(PENDING, mod + '.cpp'), UEAPI, tmp]).stdout


def refused_naming(mod, *names):
    """pending_asset(mod), or None when the compiler refused it naming one of `names`: for these gaps a refusal is
    one right answer. A refusal for anything else still fails the test."""
    try:
        return pending_asset(mod)
    except AssertionError as e:
        if str(e).startswith('refused: ') and any(re.search(r'\b%s\b' % re.escape(n), str(e)) for n in names): return None
        raise


def repl_conditions():
    """What the engine reads off ReplConditions: every ELifetimeCondition a replicated variable names cooks as its UE
    4.27 value (CoreNetTypes.h 12-25; COND_-prefixed the same), on a CPF_Net property; a RepNotify struct variable is
    CPF_Net | CPF_RepNotify naming its OnRep; NumReplicatedProperties counts the class's 16 CPF_Net properties; the
    RPCs carry one direction each and take their parameters as plain CPF_Parm, no return value. Run as the server
    runs it, SendPair writes the struct, runs OnRep_Pair right after, and appends to the replicated array."""
    base = asset('ReplConditions')
    pkg = invariants.Package(base)
    ci = pkg.find('ReplConditions_C')
    props = {p.name: p for p in pkg.struct(ci).props}
    got = [(props['C%d' % n].cond, props['C%d' % n].flags & 0x100000020, props['C%d' % n].notify) for n in range(14)]
    assert got == [(n, 0x20, 'None') for n in range(14)], got
    pair, pairs = props['Pair'], props['Pairs']
    assert (pair.flags & 0x100000020, pair.notify, pair.cond) == (0x100000020, 'OnRep_Pair', 0), (hex(pair.flags), pair.notify)
    assert pkg.path(pair.ref).endswith('/ReplConditions/FReplPair.FReplPair'), pkg.path(pair.ref)
    assert pairs.flags & 0x20 and pairs.subs[0].type == 'StructProperty' and pairs.subs[0].ref == pair.ref, pairs.subs
    assert not (props['Plain'].flags | props['Notified'].flags) & 0x20
    count = struct.unpack('<i', pkg.tag(ci, 'NumReplicatedProperties')['value'])[0]
    assert count == 16, count
    for fn, net, parms in (('SendPair', 0x2000C0, [('P', 'StructProperty')]), ('Tell', 0x1000040, [('N', 'IntProperty'), ('Why', 'StrProperty')]),
                           ('OnRep_Pair', 0, [])):
        st = pkg.struct(pkg.find(fn))
        assert st.function_flags & (0xC0 | NET_DIRECTIONS) == net, (fn, hex(st.function_flags))
        assert [(p.name, p.type) for p in st.props if p.flags & 0x80] == parms, (fn, st.props)
        assert not any(p.flags & 0x400 for p in st.props), fn
    keeps_invariants(base)
    names = dumpexp.load(os.path.join(os.path.dirname(base), 'FReplPair'))[3]
    a, b = (next(n for n in names if n.startswith(m + '_')) for m in ('A', 'B'))
    for A in (7, -3, 0):
        vm = VM(base, Notified=2, Pairs=[])
        P = {a: A, b: 1.5}
        vm.call('SendPair', P=P)
        assert (vm.self.vars['Pair'], vm.self.vars['Pairs'], vm.self.vars['Notified']) == (P, [P], 2 + A), vm.self.vars
        sets = [l[2] for l in vm.log if l[0] == 'set' and l[1] is vm.self]
        assert sets.index('Pair') < sets.index('Notified'), sets                 # the OnRep after the write
    vm = VM(base)
    vm.call('Tell', N=5, Why='x')
    assert vm.self.vars == dict(Plain=5), vm.self.vars
    print('ok  ReplConditions: each ELifetimeCondition by its UE 4.27 value, a RepNotify struct and its array, '
          'NumReplicatedProperties 16, one-way RPCs; SendPair writes, notifies, appends')


def repl_refusals():
    """What UE replicates wrongly or not at all, refused with the reason: a TMap / TSet variable (a nested one too) or
    RPC parameter sends nothing (PropertyMap.cpp 505-509, PropertySet.cpp 451-455); a RepNotify must be a
    no-parameter method of the class, the only kind the Kismet compiler keeps (KismetCompiler.cpp 2531-2545); a
    condition must be an ELifetimeCondition; an RPC needs a direction, returns nothing (RepLayout.cpp 6119 never
    sends a return value) and an override keeps its parent's net flags (Class.cpp 4189-4190). UE_SERVER with
    UE_CLIENT never compiles (clang: 'cold' and 'hot' attributes are not compatible); UE_MULTICAST with either is
    refused by rpc_one_way."""
    refused('ReplSetVar', '  UE_REPLICATED(TSet<int32>, Seen);\n', 'ReplSetVar::Seen: a TMap or TSet does not replicate')
    refused('ReplNestedSet', '  UE_REPLICATED(TArray<TSet<int32>>, Seen);\n', 'ReplNestedSet::Seen: a TMap or TSet does not replicate')
    refused('ReplMapVar', '  using FIntMap = TMap<int32, int32>;\n  UE_REPLICATED(FIntMap, M);\n', 'ReplMapVar::M: a TMap or TSet does not replicate')
    refused('RpcMapParm', '  UE_SERVER void S(TMap<int32, int32> M) {}\n', 'RpcMapParm::S: an RPC parameter cannot be a TMap or TSet')
    refused('RpcSetParm', '  UE_MULTICAST void S(TSet<FName> M) {}\n', 'RpcSetParm::S: an RPC parameter cannot be a TMap or TSet')
    refused('RepNotifyArg', '  UE_REPLICATED_USING(int32, N, OnRep_N);\n  void OnRep_N(int32 Old) {}\n',
            'its RepNotify OnRep_N must be a method of the class taking no parameters')
    refused('RepNotifyMissing', '  UE_REPLICATED_USING(int32, N, OnRep_Nope);\n', 'its RepNotify OnRep_Nope must be a method of the class')
    refused('ReplCondBad', '  UE_REPLICATED_IF(int32, A, Sometimes);\n', 'unknown replication condition Sometimes')
    refused('RpcReliableAlone', '  UE_RELIABLE void R() {}\n', 'UE_RELIABLE needs UE_SERVER, UE_CLIENT or UE_MULTICAST')
    refused('RpcReturns', '  UE_SERVER int32 S() { return 1; }\n', 'RpcReturns::S: an RPC returns void')
    refused('RpcOverride', '', "RpcKid::S: an override takes its parent's replication; drop the RPC marker",
            top='class RpcBase : public AActor { public: UE_SERVER void S() {} };\n'
                'class RpcKid : public RpcBase { public: UE_SERVER void S() {} };\n')
    refused('RpcServerClient', '  int32 N;\n  UE_SERVER UE_CLIENT void Both() { N = 1; }\n', 'FAILED: ')
    print('ok  replication refusals: TMap / TSet variables and RPC parameters, RepNotify with parameters or missing, '
          'unknown condition, RPC without direction / with a return / re-marked override, Server + Client')


def repl_never():
    base = asset('ReplNever')
    pkg = invariants.Package(base)
    got = {p.name: (p.cond, p.flags & 0x20) for p in pkg.struct(pkg.find('ReplNever_C')).props if p.flags & 0x20}
    assert got == {'Hidden': (15, 0x20), 'NoReplay': (13, 0x20), 'Shown': (0, 0x20)}, \
        'Hidden cooks condition %d, want COND_Never 15: %s' % (got.get('Hidden', (None,))[0], got)
    keeps_invariants(base)
    print('ok  ReplNever: UE_REPLICATED_IF(..., Never) is COND_Never 15 (CoreNetTypes.h 26), not the undefined 14')


def repl_notify_refusals():
    """A RepNotify returns void (RepLayout calls it with no room for a result), and is a function of the class: an
    inline method is none, so a client's RepLayout would never find it."""
    refused('RepNotifyRet', '  UE_REPLICATED_USING(int32, N, OnRep_N);\n  int32 OnRep_N() { return N; }\n', 'OnRep_N must return void')
    refused('ReplInlineNotify', '  UE_REPLICATED_USING(int32, Ammo, OnRep_Ammo);\n  int32 Seen;\n'
            '  inline void OnRep_Ammo() { Seen += 1; }\n  void Fire() { Ammo = 3; }\n', 'OnRep_Ammo is inline')
    print('ok  RepNotify refusals: one that returns a value, an inline one')


def rpc_one_way():
    """An RPC goes one way: the sender routes by one direction (AActor::GetFunctionCallspace), the receiver accepts by
    its own flags, so Multicast with Server or Client is refused (Server with Client never parses: clang refuses
    [[gnu::hot]] with [[gnu::cold]]). A static function's callspace comes from GetGlobalFunctionCallspace, which never
    answers Remote, so a static RPC is refused too."""
    refused('RpcTwoWays', '  int32 N;\n  UE_MULTICAST UE_SERVER void Also() { N = 2; }\n', 'RpcTwoWays::Also: an RPC goes one way')
    refused('RpcTwoWaysClient', '  int32 N;\n  UE_MULTICAST UE_CLIENT void Wide() { N = 3; }\n', 'RpcTwoWaysClient::Wide: an RPC goes one way')
    refused('RpcStatic', '  UE_SERVER static void S(int32 X) {}\n  void Call() { S(3); }\n', 'RpcStatic::S: a static function cannot be an RPC')
    print('ok  RPC refusals: Multicast with Server or Client, and an RPC marker on a static method')


repl_conditions()
repl_refusals()
repl_notify_refusals()
repl_never()
rpc_one_way()
# RepLayout sends a struct member by member and an array element by element (InitFromProperty_r, InitFromFunction):
# a TMap member sends nothing (FMapProperty::NetSerializeItem only logs), nor does an interface
# (FInterfaceProperty::NetSerializeItem writes nothing), at any depth.
REPL_BAG = 'struct FReplBag {\n  UE_STRUCT;\n  int32 Total;\n  TMap<int32, int32> Counts;\n};\n'
REPL_MARK = 'class IReplMarker {\npublic:\n  UE_INTERFACE;\n  void Mark();\n};\n'
refused('ReplHiddenMap', '  UE_REPLICATED(FReplBag, Bag);\n', 'ReplHiddenMap::Bag: a TMap or TSet in FReplBag does not replicate',
        top=REPL_BAG)
refused('ReplHiddenIface', '  UE_REPLICATED(TScriptInterface<IReplMarker>, Target);\n',
        'ReplHiddenIface::Target: an interface does not replicate', top=REPL_MARK)
refused('ReplHiddenRpc', '  int32 Got;\n  UE_SERVER void Send(FReplBag Sack) { Got = Sack.Total; }\n',
        'ReplHiddenRpc::Send: an RPC parameter cannot hold what does not replicate: Sack is or holds a TMap or TSet in FReplBag',
        top=REPL_BAG)
refused('ReplIfaceRpc', '  int32 N;\n  UE_SERVER void S(TScriptInterface<IReplMarker> Target) { N = 1; }\n',
        'ReplIfaceRpc::S: an RPC parameter cannot hold what does not replicate: Target is or holds an interface', top=REPL_MARK)
refused('ReplIfaceNest', '  UE_REPLICATED(FReplNest, Nest);\n', 'ReplIfaceNest::Nest: an interface in FReplNest does not replicate',
        top=REPL_MARK + 'struct FReplNest {\n  UE_STRUCT;\n  int32 N;\n  TArray<TScriptInterface<IReplMarker>> Marks;\n};\n')
print('ok  replication refusals: a TMap or an interface inside a replicated struct, as a replicated variable or an RPC '
      'parameter')
# A struct is sent whole, through the class variable holding it; a UObject lists no replicated variables and runs
# every RPC locally. A component replicates, so ReplComponentCtl compiles.
refused('StructRepl', '  UE_REPLICATED(FReplHp, Hp);\n', 'FReplHp::A: UE_REPLICATED on a struct member has no effect',
        top='struct FReplHp {\n  UE_STRUCT;\n  UE_REPLICATED(int32, A);\n  int32 B;\n};\n')
REPL_OBJ = '  UE_REPLICATED(int32, A);\n\npublic:\n  UE_SERVER void S() { A = 1; }\n};\n'
refused('ReplObjectHost', '', 'ReplObject: the replicated variable A does nothing',
        top='class ReplObject : public UObject {\n' + REPL_OBJ)
refused('ReplObjectRpc', '', 'ReplRpcObject: the RPC S does nothing',
        top='class ReplRpcObject : public UObject {\npublic:\n  UE_SERVER void S() {}\n};\n')
with tempfile.TemporaryDirectory() as _tmp:
    _src = os.path.join(_tmp, 'ReplComponentCtl.cpp')
    with open(_src, 'w', encoding='utf-8') as _f:
        _f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/ReplComponentCtl");\n'
                 'class ReplComponentCtl : public UActorComponent {\n' + REPL_OBJ)
    _proc = assetgen_compile([_src, UEAPI, _tmp])
    assert _proc.returncode == 0, _proc.stdout + _proc.stderr
print('ok  replication refusals: UE_REPLICATED on a struct member, replication on a UObject (a component compiles)')
# The engine routes a call by the called UFunction's flags; an inline method is none, so its marker goes nowhere.
for _mod, _mark in (('RpcInline', 'UE_SERVER'), ('AuthInline', 'UE_AUTHORITY_ONLY'), ('CosInline', 'UE_COSMETIC')):
    refused(_mod, '  int32 Pings;\n  %s inline void Ping() { Pings += 1; }\n  void Go() { Ping(); }\n' % _mark,
            '%s::Ping: an RPC, authority-only or cosmetic marker on an inline method does nothing' % _mod)
print('ok  a net, authority-only or cosmetic marker on an inline method is refused')


# ---- LATENT: latent calls, async actions, deferred spawn / construct warnings (invariant_rules/latent.py)

def latent_compile_log(src, game=None):
    """(exit code, what the compiler printed) for one source, compiled into a scratch folder: warnings are read off it."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        proc = assetgen_compile([src, UEAPI, tmp] + (['--game', game] if game else []))
    return proc.returncode, proc.stdout


def said(log, *patterns):
    """A 'warning:' or a refusal ('FAILED:') line of a compile log that matches every regex given, ignoring case."""
    lines = [l for l in log.splitlines() if re.search(r'\b(warning|FAILED):', l)]
    return any(all(re.search(p, l, re.I) for p in patterns) for l in lines)


def says(mod, what, *patterns):
    """A pending check: compiling tests/pending/<mod>.cpp warns (or refuses) on a line matching every pattern."""
    def check():
        out = latent_compile_log(os.path.join(PENDING, mod + '.cpp'))[1]
        assert said(out, *patterns), 'nothing said %s: %s' % (what, ' | '.join(out.strip().splitlines()))
    return check


def latent_refusals():
    """What a function that waits cannot be, each refused with its reason: a static (no object, so no frame to keep its
    locals in), one that returns a value or takes a non-const reference (its caller is gone when it resumes), one that
    passes its own FLatentActionInfo (the compiler writes the one the latent action manager resumes through), a class that
    declares its ubergraph's name (FindFunction would find that one, LatentActionManager.cpp 214-221), and a patched
    game class (whose ubergraph is the game's)."""
    umg = '#include "UeApi/UMG.h"\n'
    download = '  UAsyncTaskDownloadImage* T = UAsyncTaskDownloadImage::DownloadImage("u");'
    refused('WaitStatic', '  static void F() { UKismetSystemLibrary::Delay(1.0f); }\n', 'no object whose ubergraph frame')
    refused('WaitStaticAwait', '  static void F() {%s UE_AWAIT(T->OnSuccess); }\n' % download,
            'no object whose ubergraph frame', top=umg)
    refused('WaitValue', '  int32 F() { UKismetSystemLibrary::Delay(1.0f); return 1; }\n',
            'returns nothing and takes no non-const reference')
    refused('WaitRef', '  void F(int32& Out) { UKismetSystemLibrary::Delay(1.0f); Out = 1; }\n',
            'returns nothing and takes no non-const reference')
    refused('WaitAwaitValue', '  int32 F() {%s UE_AWAIT(T->OnSuccess); return 1; }\n' % download,
            'returns nothing and takes no non-const reference', top=umg)
    refused('WaitInfo', '  void F() { FLatentActionInfo I; UKismetSystemLibrary::Delay(this, 1.0f, I); }\n',
            'leave the FLatentActionInfo argument out')
    refused('WaitUberName', '  void ExecuteUbergraph_WaitUberName(int32 EntryPoint) {}\n'
            '  void F() { UKismetSystemLibrary::Delay(1.0f); }\n', "is the name of a class's ubergraph")
    # A UE_PATCH of AssetTest's AssetUser, AssetTest's output standing in for the game as EditTest has it.
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'WaitPatch.cpp')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('#include "UeApi/Types.h"\n#include "UeApi/FSD.h"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/WaitPatch");\n'
                    'class AssetUser : public AActor {\npublic:\n'
                    '  UE_CLASS("/Game/_ElytrasMods/AssetTest/AssetUser", "AssetUser_C");\n};\n'
                    'class UserWaits : public AssetUser {\n  UE_PATCH;\npublic:\n'
                    '  void ReceiveBeginPlay() { UKismetSystemLibrary::Delay(1.0f); }\n};\n')
        code, log = latent_compile_log(src, os.path.join(ROOT, 'AssetTest', 'FSD', 'Content'))
        assert code and 'a patched function that waits' in log, log
    print('ok  a function that waits: refused when static, returning, by reference, passing its own FLatentActionInfo, '
          'shadowing the ubergraph, or patched into a game class')


def latent_info_written():
    """Delay(Duration, Info) is the world-context-less overload with the latent info written out: the compiler supplies
    the info, so it refuses this as it refuses Delay(this, Duration, Info), though the argument count alone would not
    tell: lowered, it would be Delay(WorldContextObject = Duration, Duration = Info, LatentInfo = its own)."""
    refused('WaitInfoNoWco', '  void F() { FLatentActionInfo I; UKismetSystemLibrary::Delay(0.5f, I); }\n',
            'Delay: leave the FLatentActionInfo argument out')
    print('ok  WaitInfoNoWco: Delay(Duration, Info) is refused, the compiler supplies the FLatentActionInfo')


def latent_worlds():
    """A class that waits and has no world of its own (not an actor, component, widget, game instance or subsystem:
    Delay finds its world through GetWorld, which a plain UObject answers only through its Outer, Obj.cpp 846) is
    warned about, by method; one that has a world is not. The warning is advice: all three still wait and resume."""
    warned = lambda mod: sorted(re.findall(r'warning: (\S+) waits, and an object of this class finds its world only '
                                           r'through its Outer', LOGS[mod]))
    assert warned('LatentTest') == ['LatentJob::Run'], LOGS['LatentTest']
    assert warned('LatentWorlds') == ['ULatentWorldJob::Run'], LOGS['LatentWorlds']
    assert warned('AsyncTest') == [] and 'finds its world' not in LOGS['AsyncTest'], LOGS['AsyncTest']
    folder = os.path.dirname(asset('LatentWorlds'))
    for cls, done in (('ULatentWorldPart', 1), ('ULatentWorldJob', 2), ('LatentWorlds', 3)):
        vm = VM(os.path.join(folder, cls), {'Delay': latent_call})
        vm.call('Run', 0.5)
        [(fn, ctx, info, _)] = vm.latent
        assert ctx is vm.self and info[3] is vm.self and vm.log[-1][2][1] == 0.5 and 'Done' not in vm.self.vars, cls
        vm.fire()
        assert vm.self.vars['Done'] == done, (cls, vm.self.vars)
        keeps_invariants(os.path.join(folder, cls))
    print('ok  LatentWorlds: a waiting UObject is warned it finds its world only through its Outer; a component or '
          'an actor is not; all three resume')


def latent_repeats():
    """A waiting method called again before its action is done, as the latent action manager answers it: a Delay at
    the same (object, UUID) is ignored while one is pending (KismetSystemLibrary.cpp 2164-2174), so one resume, which
    reads the frame as the second call left it; a LoadAsset / LoadAssetClass always adds an action (2662, 2691), so
    each call resumes on its own. A compiler that gave each run of a call site its own UUID would resume twice."""
    lat = {n: latent_call for n in ('Delay', 'LoadAsset', 'LoadAssetClass')}
    vm = VM(asset('LatentTest'), lat, Stage=4, Log=[])
    vm.call('Wait', 1.0, 100)
    vm.call('Wait', 2.0, 200)                             # same call site, first Delay still pending
    assert vm.self.vars['Log'] == [100, 200] and len(vm.latent) == 1, (vm.self.vars, vm.latent)
    assert vm.log[-1][0] == 'Delay' and vm.log[-1][2][1] == 2.0, vm.log[-1]
    vm.fire(0)
    assert vm.self.vars['Log'] == [100, 200, 201] and not vm.latent, (vm.self.vars, vm.latent)   # the second call's Tag
    vm = VM(asset('LatentTest'), lat, Stage=9, Wanted='soft:Cls', Icon='soft:Icon')
    vm.call('Load')
    vm.call('Load')
    assert [a[0] for a in vm.latent] == ['LoadAssetClass'] * 2 and vm.latent[0][2][1] == vm.latent[1][2][1], vm.latent
    vm.fire(0, result='A')
    vm.fire(0, result='B')
    assert vm.self.vars['Got'] == 'B' and [a[0] for a in vm.latent] == ['LoadAsset'] * 2, (vm.self.vars, vm.latent)
    vm.fire(0, result=Obj('Texture2D'))
    assert vm.self.vars['Stage'] == 1, vm.self.vars
    vm.fire(0, result=None)
    assert vm.self.vars['Stage'] == 0 and not vm.latent, (vm.self.vars, vm.latent)
    assert sum(1 for l in vm.log if l[0] == 'LoadAsset') == 2, vm.log
    print('ok  LatentTest: a Delay called again while pending resumes once, with the latest frame; each LoadAsset resumes')


def await_fakes():
    """AsyncTest's download factory and Activate, recorded: (made tasks, [(task, dispatchers bound on it)])."""
    made, activated = [], []
    natives = {'DownloadImage': lambda vm, ctx, *a: made.append(Obj('AsyncTaskDownloadImage', args=a)) or made[-1],
               'Activate': lambda vm, ctx: activated.append((ctx, sorted(p for o, p, f, _ in vm.binds if o is ctx)))}
    return made, activated, natives


def await_paths():
    """UE_AWAIT on an async action activates it once, after its dispatchers are bound, on whichever path runs
    (K2Node_BaseAsyncTask.cpp 410-467): the else branch's await as well as the then branch's, and an await inside a
    loop on its first round only - a second Activate starts an action like AsyncLoadPrimaryAsset again. An await some
    paths reach with the action activated and some without is refused: keeping its Activate starts the action twice
    on one path, dropping it never on the other."""
    def once(activated, task, disp, what):
        """The task was activated exactly once, with the dispatcher awaited already bound (others may be too)."""
        mine = [bound for t, bound in activated if t is task]
        assert len(mine) == 1 and disp in mine[0], '%s activates the task %d times, want once after %s is bound%s' % (
            what, len(mine), disp, '' if not mine else ' (bound: %s)' % mine)

    made, activated, natives = await_fakes()
    for ok, disp in ((True, 'OnSuccess'), (False, 'OnFail')):
        vm = VM(asset('AwaitPaths'), natives, Rounds=0)
        vm.call('Either', 'u', ok)
        task = made[-1]
        once(activated, task, disp, 'Either(bOk=%s)' % ok)
        vm.broadcast(task, disp, 'tex' if ok else None)
        assert vm.self.vars == (dict(Rounds=0, Image='tex') if ok else dict(Rounds=-1)), vm.self.vars
    print("ok  AwaitPaths.Either: the else branch's UE_AWAIT activates the async action too")

    for fn in ('Twice', 'Until'):
        made, activated, natives = await_fakes()
        vm = VM(asset('AwaitPaths'), natives, Rounds=0)
        vm.call(fn, 'u')
        task = made[-1]
        vm.broadcast(task, 'OnSuccess', 't1')
        vm.broadcast(task, 'OnSuccess', 't2')
        assert vm.self.vars == dict(Rounds=2, Image='t2'), (fn, vm.self.vars)
        assert len(made) == 1, made
        once(activated, task, 'OnSuccess', fn)
    made, activated, natives = await_fakes()
    vm = VM(asset('AwaitPaths'), natives, Rounds=2)
    vm.call('Until', 'u')                                   # the first round breaks: nothing bound, nothing activated
    assert not activated and not vm.binds and vm.self.vars == dict(Rounds=2), (activated, vm.binds, vm.self.vars)
    print('ok  AwaitPaths.Twice / Until: a UE_AWAIT in a loop activates its async action once, on the first round')

    umg, task = '#include "UeApi/UMG.h"\n', '    UAsyncTaskDownloadImage* T = UAsyncTaskDownloadImage::DownloadImage(Url);\n'
    for mod, body in (('AwaitSkips', '    for (int32 I = 0; I < 3; ++I) { if (Stop) continue; UE_AWAIT(T->OnSuccess); }\n'),
                      ('AwaitMaybe', '    if (Stop) UE_AWAIT(T->OnSuccess);\n    UE_AWAIT(T->OnFail);\n')):
        refused(mod, '  bool Stop;\n  void F(FString Url) {\n%s%s  }\n' % (task, body),
                "some paths reach it with T's async action already activated and some without", top=umg)
    print('ok  AwaitSkips / AwaitMaybe: an await reached with its action activated on some paths only is refused')


def await_null_proxy():
    """An async factory that returns None: the editor's expansion tests the proxy with IsValid and skips the binds
    and Activate (K2Node_BaseAsyncTask.cpp 393-408), where binding through a None context logs an Accessed None
    script warning per bind and per Activate (ScriptCore.cpp 2904-2937). Nothing after the await runs either way.
    (DownloadImage itself never returns None; a factory such as CreateMoveToProxyObject does, with no pawn.) The same
    mod with a real task binds, activates and resumes, so the None case is the IsValid test and nothing else."""
    made, activated, natives = await_fakes()
    vm = VM(asset('AwaitNullProxy'), natives)
    vm.call('Download', 'u')
    once_ok = len(activated) == 1 and activated[0][0] is made[-1] and 'OnSuccess' in activated[0][1]
    vm.broadcast(made[-1], 'OnSuccess', 'tex')
    assert once_ok and vm.self.vars == dict(Image='tex') and not vm.accessed_none, (activated, vm.self.vars)
    made, activated, natives = await_fakes()
    natives['DownloadImage'] = lambda vm, ctx, *a: None
    vm = VM(asset('AwaitNullProxy'), natives)
    vm.call('Download', '')
    assert not activated and 'Image' not in vm.self.vars and not vm.binds, (activated, vm.self.vars, vm.binds)
    assert not vm.accessed_none, 'the None proxy is bound / activated through a warning context: Accessed None ' \
                                 'at %s' % vm.accessed_none
    print('ok  AwaitNullProxy.Download: a None async proxy skips its binds and Activate, as IsValid gates them')


def proxy_frame_held():
    """A callback proxy whose factory leaves it to the persistent frame to keep (CreateProxyObjectForPlayMontage sets
    RF_StrongRefOnFrame and roots it nowhere else, PlayMontageCallbackProxy.cpp 15-23; the anim instance's delegates
    reach it weakly, 56-63) must be stored where the garbage collector sees it: a member, or a local of the ubergraph,
    whose frame the class reports (BlueprintGeneratedClass.cpp 1683-1713). ProxyLocalHeld.Play binds its proxy in a
    plain function, whose local dies with the call: the compiler stores it into a transient member too, so after Play
    returns the object still references it and a GC leaves it for Done. (The suite's AsyncTest.Play is the same case;
    latent_proxy_frame_held checks it there.)"""
    import invariants
    base = asset('ProxyLocalHeld')
    pkg = invariants.Package(base)
    local = lambda n: n.op == 0x00 and ('.'.join(n.ops[0][1]), n.ops[0][2])

    def holder(i, dest):
        """Where a Let's destination keeps the proxy: a member, the ubergraph's frame, or a plain local - unless
        that local is copied on into a member or the frame in the same function."""
        if dest.op == 0x01: return 'a member'
        if dest.op != 0x00: return 'op %02x' % dest.op
        owner = dest.ops[0][2]
        st = pkg.struct(owner - 1) if owner > 0 else None
        if st and st.function_flags & 0x8000: return 'the frame'
        for n in invariants.statements(pkg, i)[0]:
            if (n.op in (0x0F, 0x5F) and n.kids[0].op == 0x01 and local(n.kids[1]) == local(dest)) or \
                    (n.op == 0x64 and local(n.kids[0]) == local(dest)):
                return 'a member' if n.op != 0x64 else 'the frame'
        return 'a local of ' + pkg.path(owner)
    factory = lambda n: n.op in (0x1B, 0x1C, 0x45, 0x46, 0x68) and n.ops[0][0] == 'obj' \
        and (pkg.path(n.ops[0][1]) or '').endswith(':CreateProxyObjectForPlayMontage')
    held = []
    for i, st in invariants.functions(pkg):
        for n in invariants.statements(pkg, i)[0]:
            if n.op in (0x0F, 0x5F) and factory(n.kids[1]): held.append(holder(i, n.kids[0]))
            elif n.op == 0x64 and factory(n.kids[0]): held.append('the frame')
    assert len(held) == 1, held
    assert held[0] in ('a member', 'the frame'), 'the montage proxy is kept in %s' % held[0]
    made = []
    vm = VM(base, {'CreateProxyObjectForPlayMontage': lambda vm, ctx, *a: made.append(Obj('PlayMontageCallbackProxy')) or made[-1]},
            Mesh='mesh', Montage='montage', Last='None')
    vm.call('Play')
    assert any(v is made[-1] for v in vm.self.vars.values()), 'nothing of the object holds the proxy: %s' % vm.self.vars
    vm.broadcast(made[-1], 'OnCompleted', 'End')
    assert vm.self.vars['Last'] == 'End', vm.self.vars
    keeps_invariants(base)
    print('ok  ProxyLocalHeld.Play: a callback proxy bound in a plain function is kept in a member, where the GC sees it')


def deferred_left():
    """A deferred spawn never finished (the actor never runs its construction script or BeginPlay, Actor.cpp
    3185-3251), and a deferred component add never finished (never attached or registered, ActorConstruction.cpp
    1165-1212) or begun with a bManualAttachment the finish then overrides (the add's is not read when deferred,
    1157-1160): C++ compiles each, so the compiler says so at the function. A finish at another transform is not
    asked about: FinishSpawning recomposes it on purpose (Actor.cpp 3212-3232). The patterns start at a word, so the
    function names NoFinish / CompNoFinish do not match them themselves. The control: UberDeferGuard's Begin stores
    its deferred spawn in a member and Finish finishes it, which is not warned about."""
    log = latent_compile_log(os.path.join(TESTS, 'DeferredLeft.cpp'))[1]
    for fn, what in (('NoFinish', r'\bfinish'), ('CompNoFinish', r'\bfinish'), ('CompManual', r'\battach')):
        assert said(log, r'DeferredLeft::%s\b' % fn, what), \
            'nothing said at %s: %s' % (fn, ' | '.join(log.strip().splitlines()))
    held = latent_compile_log(os.path.join(TESTS, 'UberDeferGuard.cpp'))[1]
    assert not said(held, r'\bdeferred\b'), 'a deferred spawn kept in a member warned: ' + held
    print('ok  DeferredLeft: an unfinished deferred spawn or component add, or a finish with another attachment, warns; '
          'one kept in a member does not')


def spawn_abstract():
    """SpawnActor of an abstract class returns None (LevelActor.cpp 333-347); SpawnObject with no Outer returns None
    (GameplayStatics.cpp 606-627). Each call is warned about at the function that makes it; the abstract one saying so
    (the word, not the class name, which has it too). SpawnObject of an abstract class is refused: it makes one quietly
    in Shipping and asserts in Development (UObjectGlobals.cpp 2362), which uber_spawn_class_operands holds every
    package to, and the editor's Construct Object node refuses the class (K2Node_GenericCreateObject.cpp 13-64)."""
    log = LOGS['SpawnAbstract']
    for fn, what in (('SpawnShape', r'\babstract\b'), ('MakeOuterless', r'\bouter\b|\bNone\b')):
        assert said(log, r'SpawnAbstract::%s\b' % fn, what), 'nothing said at %s: %s' % (fn, ' | '.join(log.strip().splitlines()))
    refused('SpawnAbstractSpec', '  void MakeSpec() { NewObject<UAbstractSpec>(this); }\n',
            'MakeSpec: UAbstractSpec is an abstract class',
            OBJECTS + 'class UAbstractSpec : public UObject {\npublic:\n  virtual int32 N() = 0;\n};\n')
    print('ok  SpawnAbstract: spawning an abstract actor class and constructing with no Outer each warn at the '
          'function; constructing an abstract object class is refused')


def spawn_abstract_component():
    """AddComponentByType of an abstract component class ends in NewObject too (AActor::AddComponentByClass,
    ActorConstruction.cpp 1140-1163), whose allocation asserts in a Development game (UObjectGlobals.cpp 2362), so it is
    refused as SpawnObject's is: a class with a `= 0` method, and a UE_FINAL_AS base, cooked Abstract, whose message
    names the leaf to make instead."""
    refused('SpawnAbstractComp', '  void MakeComp() { AddComponentByType<UAbstractComp>(this); }\n',
            'MakeComp: UAbstractComp is an abstract class',
            OBJECTS + 'class UAbstractComp : public UActorComponent {\npublic:\n  virtual int32 N() = 0;\n};\n')
    refused('SpawnAbstractLeaf', '  void MakeBase() { AddComponentByType<UFaCompBase>(this); }\n',
            'MakeBase: UFaCompBase is an abstract class (UE_FINAL_AS UFaComp\'s base)',
            OBJECTS + 'class UFaCompBase : public UActorComponent {\npublic:\n  int32 Seen;\n};\n'
                      'UE_FINAL_AS(UFaCompBase, UFaComp);\n')


spawn_abstract_component()
print('ok  SpawnAbstract: AddComponentByType of an abstract component class is refused, a UE_FINAL_AS base\'s naming the leaf')


def wait_hold():
    """A local that holds an object across a wait is a weak reference in the persistent frame unless the object has
    RF_StrongRefOnFrame (UObjectGlobals.cpp 3460-3483): WaitHold.F's widget, made before the Delay and read after it,
    can be collected in between. The compiler knows which locals outlive a wait, so it says so. Its controls are not
    warned about: a widget read out of a member, one made after the wait, a SpawnObject result, an actor."""
    log = LOGS['WaitHold']
    assert said(log, r'WaitHold::F\b', r'\bW\b'), 'nothing said about W outliving the wait: ' + log
    held = re.findall(r'warning: WaitHold::(\w+) keeps (\w+)', log)
    assert held == [('F', 'W')], held
    print('ok  WaitHold.F: an object local read after a wait warns that the frame does not keep it; its controls do not')


latent_refusals()
latent_info_written()
latent_worlds()
latent_repeats()
await_paths()
await_null_proxy()
proxy_frame_held()
deferred_left()
spawn_abstract()
wait_hold()


# ---- EDITS: invariants on the packages UE_PATCH / UE_ASSET_EDITS produce from the game (invariant_rules/edits.py)
# sweep() never reaches those, so each check here runs invariants.py's rules on them itself. A game package's own findings
# are not the edit's: for those only what the edit adds counts. The mods are inline, as edits()'s are: an S38 mod
# compiles only with --game, which build() gives EditTest alone, so it cannot sit in tests/ or tests/pending.

import tempfile
import invariants
from invariant_rules import edits as edits_rules


EDIT_HEAD = '#include "UeApi/Types.h"\n#include "UeApi/Engine.h"\n#include "UeApi/FSD.h"\n'
COOKED_RULES = {'cooked_instancing_flags', 'cooked_instancing_scopes', 'cooked_template_tags_listed', 'edited_template_changes_listed'}


def pinned_elsewhere(f):
    """A finding a pending test below pins, which the behaviour checks leave to it. None now: EditAddTick, which pinned
    COMP's receive_tick_sets_can_ever_tick, passes (a ReceiveTick a patch adds turns the class's tick on)."""
    return False


def compile_edit(tmp, mod, src, game):
    """Compiles an S38 mod (its whole source) against `game`, the folder /Game is in, into tmp; returns the process and
    the Content folder the edited packages are written to, each at its own /Game path."""
    path = os.path.join(tmp, mod + '.cpp')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(src)
    out = os.path.join(tmp, 'FSD', 'Content', '_ElytrasMods', mod)
    os.makedirs(out)
    proc = assetgen_compile([path, UEAPI, out, '--game', game])
    return proc, os.path.join(tmp, 'FSD', 'Content')


def edit_findings(edited, game_root, only=None, pinned=lambda f: False):
    """invariants.py's findings on an edited package that its vanilla copy (the same path under game_root) does not
    have, the known ones and the pinned ones left out. The vanilla copy is in GAME_CONTENT meanwhile, so rules comparing
    against it see it."""
    vanilla = os.path.join(game_root, *edited.replace(os.sep, '/').split('/FSD/Content/', 1)[1].split('/'))
    saved = list(invariants.GAME_CONTENT)
    if saved: invariants.GAME_CONTENT[:] = [game_root] + [g for g in saved if g != game_root]   # no --game: /Game imports go unchecked
    try:
        new = set(invariants.check(invariants.Package(edited), only)) - set(invariants.check(invariants.Package(vanilla), only))
    finally:
        invariants.GAME_CONTENT[:] = saved
    return sorted(f for f in new if f[0] not in KNOWN_RULES and not pinned(f))


def keeps_edit_invariants(edited, game_root, pinned=pinned_elsewhere):
    found = edit_findings(edited, game_root, pinned=pinned)
    assert not found, '%s: the edit breaks %s' % (os.path.basename(edited), '; '.join('%s %s: %s' % f for f in found[:3]))


def keeps_invariants_but(base, game_root, pinned=pinned_elsewhere):
    """keeps_invariants, a pending test's pinned findings left to it; game_root, the folder the edited packages came
    from, is searched first for /Game imports."""
    saved = list(invariants.GAME_CONTENT)
    if saved: invariants.GAME_CONTENT[:] = [game_root] + [g for g in saved if g != game_root]   # no --game: /Game imports go unchecked
    try:
        found = [f for f in invariants.check(invariants.Package(base)) if f[0] not in KNOWN_RULES and not pinned(f)]
    finally:
        invariants.GAME_CONTENT[:] = saved
    assert not found, '%s breaks %s' % (os.path.basename(base), '; '.join('%s %s: %s' % f for f in found[:3]))


def function_facts(base, name):
    """What the engine reads off a class's function `name`: whether the class lists it in Children and in FuncMap
    under its own name, its SuperStruct as the export header and the payload give it, its flags."""
    pkg = invariants.Package(base)
    c = next(i for i, st in invariants.classes(pkg))
    cls = pkg.struct(c)
    f = next(i for i, e in enumerate(pkg.exports) if e['name'] == name and e['outer'] == c + 1)
    st = pkg.struct(f)
    return dict(child=f + 1 in cls.children, funcmap=dict(cls.func_map).get(name) == f + 1,
                header_super=pkg.path(pkg.exports[f]['super']), super=pkg.path(st.super), flags=st.function_flags)


def cdo_ends_at_serial_size(base):
    """The class default object's tags end at its SerialSize: SerializeDefaultObject writes the tags and nothing after
    them for a class without sparse data (Class.cpp:4674-4697)."""
    pkg = invariants.Package(base)
    c = next(i for i, st in invariants.classes(pkg))
    cdo = pkg.struct(c).cdo - 1
    return pkg.tags(cdo).end == len(pkg.blob(cdo))


FUNC_NETFUNCFLAGS = 0x012040C0      # Net | NetReliable | NetServer | NetClient | NetMulticast (Script.h:157)

COMP_DECL = ('class CompTest : public AActor {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/CompTest/CompTest", "CompTest_C");\n'
             '  int32 Ticks;\n  class UPointLightComponent* Lamp;\n'
             '  static constexpr const char* Lamp__UeScsNode = "00000000000000000000000000000000";\n'
             '  void ReceiveBeginPlay();\n};\n')
RPC_KEEP = ('class ReplTest : public AActor {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/ReplTest/ReplTest", "ReplTest_C");\n'
            '  int32 Local;\n  UE_CLIENT void ClientPing(int32 Seq);\n};\n'
            'class Tweaks : public ReplTest {\n  UE_PATCH;\n  void ClientPing(int32 Seq) { ReplTest::ClientPing(Seq); Local = Local + 1; }\n};\n')


def suite_edit_cases():
    """The edits edits() and path_edits() make of the suite's own stand-ins for game packages, as (mod, the folder /Game
    is in, the edited package's path under _ElytrasMods, source after the includes)."""
    comp, repl, types, assets = (os.path.join(ROOT, m, 'FSD', 'Content') for m in ('CompTest', 'ReplTest', 'TypesTest', 'AssetTest'))
    return (
        ('FnLocal', comp, 'CompTest/CompTest', COMP_DECL + 'class Tweaks : public CompTest {\n  UE_PATCH;\n'
         '  void ReceiveBeginPlay() { int32 Step = 5; Ticks = Ticks + Step; }\n};\n'),
        ('FnKeep', comp, 'CompTest/CompTest', COMP_DECL + 'class Tweaks : public CompTest {\n  UE_PATCH;\n'
         '  void ReceiveBeginPlay() { CompTest::ReceiveBeginPlay(); Ticks = Ticks + 5; }\n};\n'),
        ('FnAdd', comp, 'CompTest/CompTest', COMP_DECL + 'class Tweaks : public CompTest {\n  UE_PATCH;\n'
         '  void ReceiveBeginPlay() { Ticks = Twice(Ticks) + 5; }\n  int32 Twice(int32 X) { return X * 2; }\n'
         '  void ReceiveTick(float DeltaSeconds) { Ticks = Ticks + 1; }\n};\n'),
        ('LampDefaults', comp, 'CompTest/CompTest', COMP_DECL + 'class Tweaks : public CompTest {\n  UE_PATCH;\n'
         '  UE_DEFAULTS { Lamp->Intensity = 5000.0f; Lamp->AttenuationRadius = 900.0f; Ticks = 3; }\n};\n'),
        ('GlobalFn', comp, 'CompTest/CompTest', COMP_DECL + 'int32 Step = 5;\nclass Tweaks : public CompTest {\n  UE_PATCH;\n'
         '  void ReceiveBeginPlay() { Ticks = Ticks + Step; Step = Step + 1; }\n};\n'),
        ('RpcKeep', repl, 'ReplTest/ReplTest', RPC_KEEP),
        ('TypesPaths', types, 'TypesTest/TypesTest',
         'class TypesTest : public AActor {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/TypesTest/TypesTest", "TypesTest_C");\n'
         '  TArray<FVector> Points;\n  TArray<FFloatInterval> Spans;\n};\n'
         'class Tweaks : public TypesTest {\n  UE_PATCH;\n'
         '  UE_DEFAULTS { Points[1].Y = 50.0f; Spans[0].Max = 9.0f; Spans[1] = FFloatInterval{3.0f, 4.0f}; }\n};\n'),
        ('MoodPatch', assets, 'AssetTest/UMoodDef',
         'UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Exploder, "/Game/Enemies/Spider/Exploder/ED_Spider_Exploder");\n'
         'UE_ASSET_AT(UEnemyDescriptor, ED_AssetTest, "/Game/_ElytrasMods/AssetTest/ED_AssetTest");\n'
         'UE_ASSET_EDITS {\n  ED_AssetTest.VeteranClasses[0] = &ED_Spider_Exploder;\n  ED_AssetTest.IdealSpawnSize = 9;\n}\n'
         'class UMoodDef : public UPrimaryDataAsset {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/AssetTest/UMoodDef", "UMoodDef_C");\n'
         '  float Health;\n  int32 Count;\n  FName Tag;\n};\n'
         'class MoodTweaks : public UMoodDef {\n  UE_PATCH;\n  UE_DEFAULTS { Health = 42.0f; Count = 0; Tag = "tweaked"; }\n};\n'))


def edit_invariants_suite():
    """CLS-G11 on the suite's own stand-ins for game packages: every edit edits() and path_edits() make - defaults, a
    component's template, a function replaced (with a local of its own, and around its game body, kept), added (a
    helper and an override of a native event), an RPC's body kept, a global, paths into values, a data asset - keeps
    invariants.py's rules (a pending test's pinned findings left to it), and
    what it adds to a class is listed as the engine looks functions up: in Children and FuncMap under its own name, the
    kept body overriding nothing and routed nowhere, the override naming the native function it overrides."""
    for mod, game, path, body in suite_edit_cases():
        with tempfile.TemporaryDirectory() as tmp:
            proc, content = compile_edit(tmp, mod, EDIT_HEAD + 'UE_MOD_PACKAGE("/Game/_ElytrasMods/%s");\n%s' % (mod, body), game)
            assert proc.returncode == 0, mod + ':\n' + proc.stdout + proc.stderr
            own = os.path.join(content, '_ElytrasMods', mod)
            written = [os.path.join(dp, f[:-len('.uasset')]) for dp, _, fs in os.walk(content) for f in fs if f.endswith('.uasset')]
            edited = [w for w in written if not w.startswith(own + os.sep)]
            assert os.path.join(content, '_ElytrasMods', *path.split('/')) in edited, (mod, edited)
            for b in written:
                keeps_invariants_but(b, game)           # the rules that hold on every package: the edited ones, a global's class
            for b in edited:
                keeps_edit_invariants(b, game)          # and nothing new against the package it edits
                if any(True for _ in invariants.classes(invariants.Package(b))):
                    assert cdo_ends_at_serial_size(b), (mod, b, 'the edited CDO does not end at its SerialSize')
            b = os.path.join(content, '_ElytrasMods', *path.split('/'))
            if mod == 'FnKeep':
                kept = function_facts(b, 'ReceiveBeginPlay__Vanilla')
                assert kept['child'] and kept['funcmap'] and kept['header_super'] is None and kept['super'] is None \
                    and not kept['flags'] & FUNC_NETFUNCFLAGS, kept
                was = function_facts(os.path.join(game, '_ElytrasMods', 'CompTest', 'CompTest'), 'ReceiveBeginPlay')
                now = function_facts(b, 'ReceiveBeginPlay')
                assert now == was, (was, now)           # the replaced function: same listing, super and flags
            if mod == 'FnAdd':
                tick, twice = function_facts(b, 'ReceiveTick'), function_facts(b, 'Twice')
                assert tick['child'] and tick['funcmap'] and tick['header_super'] == tick['super'] == '/Script/Engine.Actor:ReceiveTick', tick
                assert twice['child'] and twice['funcmap'] and twice['header_super'] is twice['super'] is None, twice
            if mod == 'RpcKeep':
                kept, rpc = function_facts(b, 'ClientPing__Vanilla'), function_facts(b, 'ClientPing')
                assert kept['child'] and kept['funcmap'] and kept['super'] is None and not kept['flags'] & (FUNC_NETFUNCFLAGS | 0x80000000), kept
                assert rpc['flags'] & FUNC_NETFUNCFLAGS == 0x01000040 and rpc['child'] and rpc['funcmap'], rpc
    print('ok  EditTest: every UE_PATCH / UE_ASSET_EDITS edit of the suite\'s packages keeps invariants.py\'s rules; a kept game '
          'body (a plain function, an RPC\'s without its net flags) and an added function are in Children and FuncMap, '
          'overriding nothing or the native event they name; each edited CDO ends at its SerialSize')


SUPER_DECL = ('class SuperBase : public AActor {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/SuperTest/SuperBase", "SuperBase_C");\n'
              '  int32 Count;\n  int32 Bump(int32 By);\n  int32 Twice(int32 By);\n};\n'
              'class SuperTest : public SuperBase {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/SuperTest/SuperTest", "SuperTest_C");\n'
              '  int32 Bump(int32 By);\n  int32 Thrice(int32 By);\n};\n')
TYPES_DECL = ('enum class EPreloadTier : uint8 { Low, High };\nUE_ENUM_IN(EPreloadTier, "/Game/_ElytrasMods/PreloadTypes");\n'
              'struct FPreloadInner {\n  UE_STRUCT_IN("/Game/_ElytrasMods/PreloadTypes");\n  int32 A;\n};\n'
              'struct FPreloadOuter {\n  UE_STRUCT_IN("/Game/_ElytrasMods/PreloadTypes");\n  FPreloadInner Inner;\n  EPreloadTier Tier;\n};\n'
              'class PreloadTypes : public AActor {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/PreloadTypes/PreloadTypes", "PreloadTypes_C");\n'
              '  int32 Sum(FPreloadOuter O);\n};\n')


def edit_deps_completed():
    """S38: a function a patch writes into a game package has the preload dependencies FPackage::Save completes for a
    function of AssetGen's own package (SavePackage.cpp 3962-4140): no edl_* finding the vanilla package lacks.
    FnOverrideBp adds to SuperTest_C an override of SuperBase_C's Twice, which SuperTest_C did not override: the loader
    fetches its SuperStruct, a function in the parent Blueprint's package, with bCheckSerialized while it serializes the
    override (edl_super_serialized). FnTypeLocal replaces PreloadTypes_C's Sum with a body holding a TArray<EPreloadTier>
    local, a type the game's Sum never named: the function links against the enum while it is serialized, so the enum
    is serialized first (edl_property_types) and created before the payload naming it is read (edl_payload_created)."""
    cases = (('FnOverrideBp', 'SuperTest', 'SuperTest/SuperTest', SUPER_DECL + 'class Tweaks : public SuperTest {\n  UE_PATCH;\n'
              'public:\n  int32 Twice(int32 By) { return By * 3; }\n};\n'),
             ('FnTypeLocal', 'PreloadTypes', 'PreloadTypes/PreloadTypes', TYPES_DECL + 'class Tweaks : public PreloadTypes {\n'
              '  UE_PATCH;\npublic:\n  int32 Sum(FPreloadOuter O) {\n    TArray<EPreloadTier> Tiers;\n'
              '    Tiers.Add(EPreloadTier::High);\n    return Tiers.Num();\n  }\n};\n'))
    found = []
    for mod, stand_in, path, body in cases:
        game = os.path.join(ROOT, stand_in, 'FSD', 'Content')
        with tempfile.TemporaryDirectory() as tmp:
            proc, content = compile_edit(tmp, mod, EDIT_HEAD + 'UE_MOD_PACKAGE("/Game/_ElytrasMods/%s");\n%s' % (mod, body), game)
            assert proc.returncode == 0, mod + ':\n' + proc.stdout + proc.stderr
            b = os.path.join(content, '_ElytrasMods', *path.split('/'))
            found += ['%s %s %s: %s' % ((mod,) + f) for f in edit_findings(b, game, set(EDL_RULES))]
    assert not found, '%d findings, e.g. %s' % (len(found), '; '.join(found[:2]))


def edit_whole_containers():
    """S38: a patch's whole TSet / TMap is what the edited default object loads. Its tag is a delta against the
    archetype, the parent Blueprint's CDO: the loader copies that value in, takes out the elements the tag lists as
    removed, then adds the rest (PropertySet.cpp 285-358, PropertyMap.cpp 316-400), as for a mod class's own
    (prop_set_delta). MapPatch sets PropSetDelta_C's Ids and Score over PropSetBase_C's {1, 2} and {a: 1, c: 3}: to
    {7} and {a: 9} (every element dropped or changed), then to {1, 7} and {a: 1, b: 2} (some kept as they were). A set
    or map inside a struct written as tags loads the same way, each member over the archetype's struct's (Class.cpp
    2775): by a member path (Held.Ids, and Deep.In.Score, where the CDO has no Deep tag of its own) and inside a whole
    struct (Held, Deep). Written as additions only, the CDO would load the union. What a case does not assign keeps
    what the unpatched CDO loads."""
    game = os.path.join(ROOT, 'PropSetDelta', 'FSD', 'Content')
    decl = ('struct FPropSetHeld {\n  UE_STRUCT_IN("/Game/_ElytrasMods/PropSetDelta");\n  TSet<int32> Ids;\n'
            '  TMap<FName, int32> Score;\n  int32 N;\n};\n'
            'struct FPropSetDeep {\n  UE_STRUCT_IN("/Game/_ElytrasMods/PropSetDelta");\n  FPropSetHeld In;\n  int32 M;\n};\n'
            'class PropSetBase : public AActor {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/PropSetDelta/PropSetBase", "PropSetBase_C");\n'
            '  TSet<int32> Ids;\n  TMap<FName, int32> Score;\n  FPropSetHeld Held;\n  FPropSetDeep Deep;\n};\n'
            'class PropSetDelta : public PropSetBase {\npublic:\n  UE_CLASS("/Game/_ElytrasMods/PropSetDelta/PropSetDelta", "PropSetDelta_C");\n};\n')
    paths = (('Ids',), ('Score',), ('Held', 'Ids'), ('Held', 'Score'), ('Deep', 'In', 'Ids'), ('Deep', 'In', 'Score'))
    parent = invariants.Package(os.path.join(game, '_ElytrasMods', 'PropSetDelta', 'PropSetBase'))
    pc = parent.find('Default__PropSetBase_C')

    def loads(child):
        cc = child.find('Default__PropSetDelta_C')
        kinds = {p: 'set' if p[-1] == 'Ids' else 'map' for p in paths}
        return {p: loaded_container(child, cc, p, kinds[p], loaded_container(parent, pc, p, kinds[p], set() if kinds[p] == 'set' else {}))
                for p in paths}
    vanilla = loads(invariants.Package(os.path.join(game, '_ElytrasMods', 'PropSetDelta', 'PropSetDelta')))
    for body, assigned in (('Ids = {7};\n    Score = {{"a", 9}};', {('Ids',): {7}, ('Score',): {'a': 9}}),
                           ('Ids = {1, 7};\n    Score = {{"a", 1}, {"b", 2}};', {('Ids',): {1, 7}, ('Score',): {'a': 1, 'b': 2}}),
                           ('Held.Ids = {7};\n    Deep.In.Score = {{"a", 9}};', {('Held', 'Ids'): {7}, ('Deep', 'In', 'Score'): {'a': 9}}),
                           ('Held = {{1, 7}, {{"a", 1}, {"b", 2}}, 3};\n    Deep = {{{7}, {{"c", 3}}, 3}, 4};',
                            {('Held', 'Ids'): {1, 7}, ('Held', 'Score'): {'a': 1, 'b': 2}, ('Deep', 'In', 'Ids'): {7},
                             ('Deep', 'In', 'Score'): {'c': 3}})):
        with tempfile.TemporaryDirectory() as tmp:
            proc, content = compile_edit(tmp, 'MapPatch', EDIT_HEAD + 'UE_MOD_PACKAGE("/Game/_ElytrasMods/MapPatch");\n' + decl
                                         + 'class Tweaks : public PropSetDelta {\n  UE_PATCH;\n'
                                         '  UE_DEFAULTS {\n    ' + body + '\n  }\n};\n', game)
            assert proc.returncode == 0, proc.stdout + proc.stderr
            got = loads(invariants.Package(os.path.join(content, '_ElytrasMods', 'PropSetDelta', 'PropSetDelta')))
        want = {**vanilla, **assigned}
        assert got == want, 'the patched CDO loads %s; the patch says %s, so it should load %s' % (got, body, want)


GRUNT = '#include "UeApi/Game/ENE_Spider_Grunt_Normal_C.h"\n'
GRUNT_PKGS = ['Enemies/Spider/Grunt/ED_Spider_Grunt', 'Enemies/Spider/Grunt/ENE_Spider_Grunt_Normal']
GRUNT_KEEP = ('class GruntCount : public ENE_Spider_Grunt_Normal_C {\n  UE_PATCH;\n'
              '  void GetEnemySpawnedCount(int& SpawnCount) {\n'
              '    ENE_Spider_Grunt_Normal_C::GetEnemySpawnedCount(SpawnCount);\n    SpawnCount = SpawnCount + 41;\n  }\n};\n')


def game_edit_cases():
    """game_edits()'s edits of the game's own packages, split by kind, as (mod, the UeApi include, the edited packages'
    paths under /Game, source after the includes)."""
    return (
        ('GruntDefaults', GRUNT, GRUNT_PKGS,
         'UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");\n'
         'UE_ASSET_EDIT(ED_Spider_Grunt) {.SpawnSpread = 800.0f, .IdealSpawnSize = 12};\n'
         'class GruntTweaks : public ENE_Spider_Grunt_Normal_C {\n  UE_PATCH;\n'
         '  UE_DEFAULTS {\n    CustomTimeDilation = 0.5f;\n    HealthComponent->MaxHealth = 180.0f;\n'
         '    MeleeAttack->CenterOnTarget = true;\n    enemy->mixerName = "Grunty";\n  }\n};\n'),
        ('GruntPathsInv', GRUNT, GRUNT_PKGS,
         'UE_ASSET_AT(UAnimMontage, ANIM_Spider_Grunt_Attack_I, "/Game/Enemies/Spider/Animation/ANIM_Spider_Grunt_Attack_I");\n'
         'UE_ASSET_AT(UParticleSystem, P_SpiderGrunt_Armor_Debris, "/Game/Enemies/Spider/Particles/P_SpiderGrunt_Armor_Debris");\n'
         'class GruntPaths : public ENE_Spider_Grunt_Normal_C {\n  UE_PATCH;\n  UE_DEFAULTS {\n'
         '    PrimaryActorTick.bCanEverTick = true;\n    MeleeAttack->Montages[0] = &ANIM_Spider_Grunt_Attack_I;\n'
         '    SimpleArmorDamage->ArmorBreakEffects.DissolveParticles[0] = &P_SpiderGrunt_Armor_Debris;\n  }\n};\n'
         'UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");\n'
         'UE_ASSET_EDITS { ED_Spider_Grunt.SpawnRarityModifiers[1].Rarity = 2.0f; }\n'),
        ('GruntReplace', GRUNT, GRUNT_PKGS[1:],
         'class GruntCount : public ENE_Spider_Grunt_Normal_C {\n  UE_PATCH;\n'
         '  void GetEnemySpawnedCount(int& SpawnCount) { SpawnCount = 42; }\n};\n'),
        ('GruntKeep', GRUNT, GRUNT_PKGS[1:], GRUNT_KEEP))


def edit_invariants_game():
    """CLS-G11 on the game (--game): the edits game_edits() makes to ED_Spider_Grunt and the grunt Blueprint - its CDO,
    a native subobject, its own and an inherited component's templates, a function replaced with and without its game
    body kept, paths into four values - add no finding of invariants.py's to what the game's own packages have; the
    kept body is listed in Children and FuncMap, overriding nothing; each edited CDO ends at its SerialSize."""
    if not GAME or not os.path.exists(os.path.join(UEAPI, 'Game', 'ENE_Spider_Grunt_Normal_C.h')):
        print('--  S38 edits keep the invariants on the game\'s packages: skipped (needs --game and UeApi/Game)')
        return
    for mod, header, rels, body in game_edit_cases():
        with tempfile.TemporaryDirectory() as tmp:
            proc, content = compile_edit(tmp, mod, EDIT_HEAD + header + 'UE_MOD_PACKAGE("/Game/_ElytrasMods/%s");\n%s' % (mod, body), GAME)
            assert proc.returncode == 0, mod + ':\n' + proc.stdout + proc.stderr
            for rel in rels:
                keeps_edit_invariants(os.path.join(content, *rel.split('/')), GAME)
            b = os.path.join(content, 'Enemies', 'Spider', 'Grunt', 'ENE_Spider_Grunt_Normal')
            assert cdo_ends_at_serial_size(b), (mod, 'the edited CDO does not end at its SerialSize')
            if mod == 'GruntKeep':
                kept = function_facts(b, 'GetEnemySpawnedCount__Vanilla')
                assert kept['child'] and kept['funcmap'] and kept['super'] is None and kept['header_super'] is None, kept
                was = function_facts(os.path.join(GAME, *GRUNT_PKGS[1].split('/')), 'GetEnemySpawnedCount')
                assert function_facts(b, 'GetEnemySpawnedCount') == was
    print('ok  S38 on the game: the grunt\'s default, component, path and asset edits and a function replaced with and '
          'without its game body add no invariants.py finding to the game\'s packages; the kept body is listed, '
          'overriding nothing; each edited CDO ends at its SerialSize')


def kept_body_locals():
    """A kept game body (<Fn>__Vanilla, a byte copy of the replaced function) reads its parameters through its own
    properties. The copy keeps the original's bytecode, whose EX_LocalVariable / EX_LocalOutVariable operands must be
    re-owned: left naming the replaced function's properties (the operand's FFieldPath owner is resolved as written,
    FieldPath.cpp:256-270), an out parameter is never found - the copy's frame records each out parameter under the
    copy's own property (ScriptCore.cpp:851-906, the list null-terminated), and execLocalOutVariable walks it for the
    replaced function's, off its end (2170-2185, checkSlow only): the Parent:: call crashes the game. GruntKeep
    (GetEnemySpawnedCount(int& SpawnCount), --game) and RpcKeep (ClientPing(int32 Seq)) keep bodies that read a
    parameter; local_operands must find nothing on either."""
    cases = [('RpcKeep', os.path.join(ROOT, 'ReplTest', 'FSD', 'Content'), '_ElytrasMods/ReplTest/ReplTest', RPC_KEEP)]
    if GAME and os.path.exists(os.path.join(UEAPI, 'Game', 'ENE_Spider_Grunt_Normal_C.h')):
        cases.insert(0, ('GruntKeep', GAME, GRUNT_PKGS[1], GRUNT + GRUNT_KEEP))
    found = []
    for mod, game, rel, body in cases:
        with tempfile.TemporaryDirectory() as tmp:
            proc, content = compile_edit(tmp, mod, EDIT_HEAD + 'UE_MOD_PACKAGE("/Game/_ElytrasMods/%s");\n%s' % (mod, body), game)
            assert proc.returncode == 0, mod + ':\n' + proc.stdout + proc.stderr
            b = os.path.join(content, *rel.split('/'))
            assert any(e['name'].endswith('__Vanilla') for e in invariants.Package(b).exports), (mod, 'no kept body')
            found += ['%s %s: %s' % (mod, f[1], f[2]) for f in edit_findings(b, game, {'local_operands'})]
    assert not found, '; '.join(found)


def added_tick_can_tick():
    """A ReceiveTick a patch adds to a Blueprint that cannot tick turns ticking on, as the editor's compiler does for a
    ReceiveTick event (KismetCompiler.cpp:4738-4800: the CDO's PrimaryActorTick.bCanEverTick) and as AssetGen does for
    a class of its own (Blueprint.cpp:336-352). The runtime registers an actor's tick only when bCanEverTick is set
    (Actor.cpp:914-925), and AActor leaves it off (Actor.cpp:99): CompTest, a direct AActor child whose default object
    sets no PrimaryActorTick, would never run the added ReceiveTick. Passes when the edited CDO sets
    PrimaryActorTick.bCanEverTick, or the patch is refused naming ReceiveTick."""
    from dumptags import tags as read_tags
    game = os.path.join(ROOT, 'CompTest', 'FSD', 'Content')
    with tempfile.TemporaryDirectory() as tmp:
        proc, content = compile_edit(tmp, 'EditAddTick', EDIT_HEAD + 'UE_MOD_PACKAGE("/Game/_ElytrasMods/EditAddTick");\n' + COMP_DECL
                                     + 'class Tweaks : public CompTest {\n  UE_PATCH;\n'
                                     '  void ReceiveTick(float DeltaSeconds) { Ticks = Ticks + 1; }\n};\n', game)
        if proc.returncode:
            assert 'ReceiveTick' in proc.stdout, 'refused, but not for ReceiveTick: ' + proc.stdout
            return
        b = os.path.join(content, '_ElytrasMods', 'CompTest', 'CompTest')
        pkg = invariants.Package(b)
        c = next(i for i, st in invariants.classes(pkg))
        assert pkg.path(pkg.struct(c).super) == '/Script/Engine.Actor' and function_facts(b, 'ReceiveTick')['funcmap']
        cdo = pkg.struct(c).cdo - 1
        before = invariants.Package(os.path.join(game, '_ElytrasMods', 'CompTest', 'CompTest'))
        cb = next(i for i, st in invariants.classes(before))
        assert not before.tag(before.struct(cb).cdo - 1, 'PrimaryActorTick'), 'the stand-in could tick before the patch'
        t = pkg.tag(cdo, 'PrimaryActorTick')
        lines = []
        if t: read_tags(t['value'], 0, len(t['value']), pkg.names, 0, lines)
        assert any(l.startswith('bCanEverTick [0] BoolProperty') and 'value=1' in l for l in lines), \
            'the patch adds ReceiveTick, and the CDO sets no PrimaryActorTick.bCanEverTick: %s' % (lines or 'no PrimaryActorTick tag')


GLOW = '#include "UeApi/Game/PRJ_NormalBlasterShot_C.h"\n'
BLASTER = 'WeaponsNTools/ChargeBlaster/PRJ_NormalBlasterShot'


def component_record(base, template):
    """(kind, bHasValidCookedData, the ChangedPropertyList's root names) of the SCS node or override record whose template
    is the export `template`, and that template's tags as {name: value bytes}."""
    pkg = invariants.Package(base)
    data = edits_rules.component_data
    for kind, i, c, tmpl, key, valid, lst in data(pkg):
        if tmpl > 0 and pkg.exports[tmpl - 1]['name'] == template:
            cls = pkg.exports[tmpl - 1]['cls']
            return kind, valid, {n for n, _, s in lst if s == cls}, {t['name']: t['value'] for t in pkg.tags(tmpl - 1)}
    return None


def edit_listed_component():
    """BPGC-G25, the half that works: an edit of a property a component's cooked instancing data already lists reaches
    spawned components, since the fast path copies every listed property from the template. PRJ_NormalBlasterShot's
    PointLight node has valid cooked data listing Intensity; the patch rewrites the template's Intensity tag, keeps the
    data valid and listing it, and the cooked-data rules find nothing new against the game's package."""
    if not GAME or not os.path.exists(os.path.join(UEAPI, 'Game', 'PRJ_NormalBlasterShot_C.h')):
        print('--  S38 edit of a listed component property: skipped (needs --game and UeApi/Game)')
        return
    import struct
    with tempfile.TemporaryDirectory() as tmp:
        proc, content = compile_edit(tmp, 'BlasterGlow', EDIT_HEAD + GLOW + 'UE_MOD_PACKAGE("/Game/_ElytrasMods/BlasterGlow");\n'
                                     'class BlasterGlow : public PRJ_NormalBlasterShot_C {\n  UE_PATCH;\n'
                                     '  UE_DEFAULTS { PointLight->Intensity = 5000.0f; }\n};\n', GAME)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        b = os.path.join(content, *BLASTER.split('/'))
        kind, valid, listed, tags = component_record(b, 'PointLight_GEN_VARIABLE')
        assert kind == 'scs' and valid and 'Intensity' in listed and struct.unpack('<f', tags['Intensity'])[0] == 5000.0, (kind, valid, listed)
        assert component_record(os.path.join(GAME, *BLASTER.split('/')), 'PointLight_GEN_VARIABLE')[:3] == (kind, valid, listed)
        keeps_edit_invariants(b, GAME)
    print('ok  S38: a patch of a component property its cooked instancing data lists keeps the data valid and listing it')


PUMPKIN = 'Art/Environments/Holiday_Halloween/BP_PumpkinFace_Item'


def edit_bound_names():
    """BPGC-G28: a patch replacing functions a timeline calls by name keeps every name the timeline and the class's
    component bindings bind resolving, to the same export. BP_PumpkinFace_Item's Timeline_0 binds Timeline_0__UpdateFunc
    and Timeline_0__FinishedFunc, its ComponentDelegateBinding two BndEvt__ events. The patch replaces both timeline
    functions (the update one around its game body, kept as __Vanilla): timeline_names_resolve and
    dynamic_binding_names_resolve still pass, every FuncMap entry the game had names the export it named, and the bound
    functions still take no parameter."""
    if not GAME or not os.path.exists(os.path.join(UEAPI, 'Game', 'BP_PumpkinFace_Item_C.h')):
        print('--  S38 patch of timeline functions: skipped (needs --game and UeApi/Game)')
        return
    with tempfile.TemporaryDirectory() as tmp:
        proc, content = compile_edit(tmp, 'PumpkinTimeline', EDIT_HEAD + '#include "UeApi/Game/BP_PumpkinFace_Item_C.h"\n'
                                     'UE_MOD_PACKAGE("/Game/_ElytrasMods/PumpkinTimeline");\n'
                                     'class PumpkinTimeline : public BP_PumpkinFace_Item_C {\n  UE_PATCH;\n'
                                     '  void Timeline_0__UpdateFunc() { BP_PumpkinFace_Item_C::Timeline_0__UpdateFunc(); }\n'
                                     '  void Timeline_0__FinishedFunc() { }\n};\n', GAME)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        b, a = os.path.join(content, *PUMPKIN.split('/')), os.path.join(GAME, *PUMPKIN.split('/'))
        names = {'timeline_names_resolve', 'dynamic_binding_names_resolve'}
        assert not invariants.check(invariants.Package(a), names), 'the game\'s own package breaks them'
        assert not invariants.check(invariants.Package(b), names), invariants.check(invariants.Package(b), names)
        keeps_edit_invariants(b, GAME)
        for fn in ('Timeline_0__UpdateFunc', 'Timeline_0__FinishedFunc'):
            assert function_facts(b, fn) == function_facts(a, fn), fn
        assert function_facts(b, 'Timeline_0__UpdateFunc__Vanilla')['funcmap']
        pa, pb = invariants.Package(a), invariants.Package(b)
        fm = lambda p: dict(p.struct(next(i for i, st in invariants.classes(p))).func_map)
        assert {k: v for k, v in fm(pb).items() if k in fm(pa)} == fm(pa), 'a name the game bound moved'
        assert not [p for p in pb.struct(fm(pb)['Timeline_0__UpdateFunc'] - 1).props if p.flags & 0x80], 'the update function takes parameters'
    print('ok  S38: a patch of the functions a timeline binds by name keeps each bound name resolving to its export, parameterless')


def edit_cooked_unlisted(mod, header, rel, member, template, body):
    """BPGC-G25: a patch of a component property that the component's valid cooked instancing data does not list. A
    spawned actor builds that component on the fast path (NewObject of its class, then only the listed properties
    copied from the template), so the edit must list the property, clear bHasValidCookedData, or be refused - never
    ship a template edit the fast path drops. Passes when the edit is refused naming the member, or is written (the
    tag holds the new value) with no finding of the cooked-data rules against the game's package."""
    def test():
        with tempfile.TemporaryDirectory() as tmp:
            proc, content = compile_edit(tmp, mod, EDIT_HEAD + '#include "UeApi/Game/%s"\nUE_MOD_PACKAGE("/Game/_ElytrasMods/%s");\n%s'
                                         % (header, mod, body), GAME)
            if proc.returncode:
                assert member in proc.stdout, 'refused, but not for the member: ' + proc.stdout
                return
            b = os.path.join(content, *rel.split('/'))
            got = component_record(b, template)
            assert got and member in got[3], (got, 'the edit is not in the template')
            found = edit_findings(b, GAME, COOKED_RULES)
            assert not found, '%s: %s' % found[0][::2]
    return test


def edit_cooked_unlisted_cases():
    if not GAME or not os.path.exists(os.path.join(UEAPI, 'Game', 'PRJ_NormalBlasterShot_C.h')):
        print('--  S38 edits of components with cooked instancing data: skipped (needs --game and UeApi/Game)')
        return
    edit_cooked_unlisted('EditCookedScs', 'PRJ_NormalBlasterShot_C.h', BLASTER, 'SourceRadius', 'PointLight_GEN_VARIABLE',
                         'class EditCookedScs : public PRJ_NormalBlasterShot_C {\n  UE_PATCH;\n'
                         '  UE_DEFAULTS { PointLight->SourceRadius = 12.0f; }\n};\n')()
    print('ok  EditCookedScs: a patch of an SCS template property its valid cooked data does not list leaves '
          'no stale cooked data')
    edit_cooked_unlisted('EditCookedIch', 'PRJ_PatrolBotLaser_Flying_C.h',
                         'Enemies/RivalTech/PatrolBot/Projectiles/PRJ_PatrolBotLaser_Flying', 'bReceivesDecals',
                         'Body_GEN_VARIABLE',
                         'class EditCookedIch : public PRJ_PatrolBotLaser_Flying_C {\n  UE_PATCH;\n'
                         '  UE_DEFAULTS { Body->bReceivesDecals = false; }\n};\n')()
    print('ok  EditCookedIch: a patch of an override record\'s template property its valid cooked data does not list '
          'leaves no stale cooked data')


if not globals().get('EDITS_EXPLORE'):     # set by the dev loop's exploration driver, which reuses the cases above
    edit_invariants_suite()
    edit_invariants_game()
    kept_body_locals()
    print('ok  S38: a kept game body (<Fn>__Vanilla) reads its parameters, an out parameter included, through its own '
          'properties, not the replaced function\'s')
    added_tick_can_tick()
    print('ok  EditAddTick: a ReceiveTick a patch adds to a Blueprint that cannot tick sets PrimaryActorTick.bCanEverTick '
          'on its CDO')
    edit_listed_component()
    edit_bound_names()
    edit_cooked_unlisted_cases()
    edit_deps_completed()
    print('ok  EditDeps: a patch\'s added override and replaced function get the preload dependencies the cook '
          'completes: the super serialized first, a new local\'s type serialized and created first')
    edit_whole_containers()
    print('ok  MapPatch: a patch\'s whole TSet / TMap loads as written, a property or one inside a struct (by a member '
          'path or in a whole struct): the archetype\'s elements it drops are listed as removed')


PREFETCH.finish()
print('ok  %d known gaps, each a failing test of something AssetGen does not do yet' % len(GAPS))
assert not FIXED, 'these pass now, move them in with the others: ' + ', '.join(FIXED)
