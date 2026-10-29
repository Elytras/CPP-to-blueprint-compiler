"""The replicated surface of a cooked mod, and a check that it only grows.

    python netsurface.py <mod's FSD/Content> [--write <baseline.json>] [--check <baseline.json>]

A host and a client running two versions of a mod still play together only while what the engine replicates matches
on both: a replicated class's path and bReplicates, its replicated properties (name and type), its RPCs (name, the
Server / Client / Multicast and Reliable flags, the parameters), the replicated components it constructs, and every
mod struct those carry (fields and their cooked names). Values, defaults, function bodies and everything not
replicated are free to change.

--write saves the surface as the baseline, the released version's. --check compares the build with it: a class or
struct the baseline holds must be there and identical (a replicated property, RPC or component added to an existing
class breaks a client of the old version too); a new class or struct is fine. bpbuild runs --check for a mod whose
mods.yaml entry names a `net_contract`, and packs no pak that fails it."""
import json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import invariants

CPF_NET, CPF_PARM, CPF_RETURN = 0x20, 0x80, 0x400
FUNC_NET = 0x40
RPC_FLAGS = {0x80: 'Reliable', 0x4000: 'Multicast', 0x200000: 'Server', 0x1000000: 'Client'}
KIND = {'classes': 'class', 'structs': 'struct'}
CLASSES = ('BlueprintGeneratedClass', 'WidgetBlueprintGeneratedClass', 'AnimBlueprintGeneratedClass')


def type_of(p, prop, structs):
    """A property's type as text, `Struct</Game/X/FStats.FStats>`; a mod struct it names goes into `structs`."""
    t = prop.type[:-len('Property')]
    refs = [p.path(r) for r in (prop.ref, prop.meta, prop.enum) if r]
    if prop.type == 'StructProperty' and prop.ref:
        structs.setdefault(refs[0], p.resolve(prop.ref))
    subs = [type_of(p, s, structs) for s in prop.subs]
    inner = refs + subs
    return t + ('<%s>' % ','.join(inner) if inner else '')


def replicates(p, i):
    """The class's bReplicates from its CDO, else its parent's (a mod class's), else False."""
    st = p.struct(i)
    if st.cdo > 0:
        tag = p.tag(st.cdo - 1, 'bReplicates')
        if tag: return bool(tag['bool'])
    parent = p.resolve(st.super) if st.super else None
    if parent and parent[0].struct(parent[1]) and parent[0].struct(parent[1]).kind in CLASSES:
        return replicates(*parent)
    return False


def class_surface(p, i, structs):
    st = p.struct(i)
    me = i + 1
    props = {q.name: type_of(p, q, structs) for q in st.props if q.flags & CPF_NET}
    rpcs, comps = {}, {}
    for k, e in enumerate(p.exports):
        if e['outer'] != me: continue
        kind = p.class_of(k + 1)
        if kind in p.FUNCTION_CLASSES:
            f = p.struct(k)
            if not f.function_flags & FUNC_NET: continue
            rpcs[e['name']] = dict(
                flags=sorted(n for bit, n in RPC_FLAGS.items() if f.function_flags & bit),
                params=[[q.name, type_of(p, q, structs)] for q in f.props if q.flags & CPF_PARM and not q.flags & CPF_RETURN])
        elif e['name'].endswith('_GEN_VARIABLE'):
            tag = p.tag(k, 'bReplicates')
            if tag and tag['bool']:
                comps[e['name'][:-len('_GEN_VARIABLE')]] = p.path(e['cls'])
    rep = replicates(p, i)
    if not (rep or props or rpcs or comps): return None
    return dict(super=p.path(st.super), replicates=rep, properties=props, rpcs=rpcs, components=comps)


def struct_fields(where, structs):
    """A mod struct's fields, by their cooked names; None for one this build cannot read (an engine struct)."""
    if not where: return None
    p, k = where
    st = p.struct(k)
    return {q.name: type_of(p, q, structs) for q in st.props} if st else None


def surface(content):
    classes, structs = {}, {}
    for root, _dirs, files in os.walk(content):
        for f in files:
            if not f.endswith('.uasset'): continue
            p = invariants.load(os.path.join(root, f)[:-len('.uasset')])
            for i, e in enumerate(p.exports):
                if p.class_of(i + 1) in CLASSES and e['outer'] == 0:
                    s = class_surface(p, i, structs)
                    if s: classes[p.path(i + 1)] = s
    described = {}
    while len(described) < len(structs):        # a struct's fields can name more structs
        for path in [s for s in structs if s not in described]:
            described[path] = struct_fields(structs[path], structs)
    return dict(classes=classes, structs={k: v for k, v in described.items() if v is not None})


def check(baseline, now):
    """What the build changed or removed of the baseline's surface; additions of new classes and structs are free."""
    problems = []
    for part in ('classes', 'structs'):
        for path, was in baseline[part].items():
            if path not in now[part]:
                problems.append('%s %s: gone from the replicated surface' % (KIND[part], path))
                continue
            got = now[part][path]
            for key in sorted(set(was) | set(got)) if part == 'structs' else ('super', 'replicates', 'properties', 'rpcs', 'components'):
                a, b = was.get(key), got.get(key)
                if a != b:
                    problems.append('%s %s: %s was %s, now %s' % (KIND[part], path, key, json.dumps(a, sort_keys=True), json.dumps(b, sort_keys=True)))
    return problems


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith('--'): sys.exit(__doc__)
    now = surface(args[0])
    if '--write' in args:
        out = args[args.index('--write') + 1]
        with open(out, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(now, f, indent=1, sort_keys=True)
            f.write('\n')
        print('netsurface: %d classes, %d structs -> %s' % (len(now['classes']), len(now['structs']), out))
    if '--check' in args:
        problems = check(json.load(open(args[args.index('--check') + 1], encoding='utf-8')), now)
        for line in problems: print('netsurface: ' + line)
        if problems:
            print('netsurface: the replicated surface changed; a client of the released version would disconnect '
                  '(README "The network contract"). Revert it, or write a new baseline for a breaking release.')
            return 1
        print('netsurface: ok, %d classes and %d structs as released' % (len(now['classes']), len(now['structs'])))
    if '--write' not in args and '--check' not in args:
        print(json.dumps(now, indent=1, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
