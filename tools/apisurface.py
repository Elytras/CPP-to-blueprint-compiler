"""What a mod's published APIs offer, per API, and a check that a change to it bumped that API's version.

    python apisurface.py <mods dir> <mod> [--write] [--check]

A mod with an `api_contract` in mods.yaml (ElytrasModManager: API.md "Versions") publishes APIs, each a set of package
prefixes (its `manifest` `apis`, the longest prefix wins, the `frozen` packages left out) with a Major.Minor in a header
(`api_contract.versions`, `static constexpr int32 <Api>Major = N;`). An API's surface is what another mod may use of
its packages, read off the cooked build: each class's parent and interfaces, its functions but the private ones
(parameters, return and flags), its variables but the private ones (the API stub's variables, when the build made one),
each struct's fields and each enum's entries.

The baseline (`api_contract.baseline`) is the surface and the versions of the last release. --check compares the build
with it, per API: anything of the baseline gone or changed (a struct changed at all) needs a higher Major; only
additions (a class, a function or variable, an enum entry at the end) a higher Minor (or Major); nothing changed, nothing.
bpbuild runs --check for such a mod and packs no pak that fails it. --write records the build as the release."""
import json, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import invariants
from netsurface import type_of, CLASSES

FUNC_PRIVATE = 0x40000
CPF_PARM, CPF_OUT, CPF_RETURN = 0x80, 0x100, 0x400
FUNC_FLAGS = {0x2000: 'Static', 0x80000: 'Protected', 0x10000000: 'Pure', 0x40000000: 'Const',
              0x800: 'Event', 0x40: 'Net', 0x100000: 'Delegate'}


def stub_variables(stubs, package):
    """The variables the API stub of package declares (its Blueprint's NewVariables, lower case), or None without one:
    a private C++ field is cooked on the class but left out of the stub, the only place its access shows."""
    base = os.path.join(stubs, *package.replace('/Game/', '').split('/')) if stubs else None
    if not base or not os.path.isfile(base + '.uasset'):
        return None
    p = invariants.load(base)
    i = next(k for k, _e in enumerate(p.exports) if p.class_of(k + 1) in ('Blueprint', 'WidgetBlueprint', 'AnimBlueprint'))
    tag, out = p.tag(i, 'NewVariables'), set()
    if not tag:
        return out
    b = p.blob(i)
    o = tag['at'] + 4 + 49          # the count, then the inner StructProperty tag (UE4)
    for _ in range(struct.unpack_from('<i', b, tag['at'])[0]):
        element = p.tags(i, o)
        name = next(t for t in element if t['name'] == 'VarName')
        out.add(p.names[struct.unpack_from('<i', name['value'])[0]].lower())
        o = element.end
    return out


def class_surface(p, i, stubs):
    st, me, junk = p.struct(i), i + 1, {}
    public = stub_variables(stubs, p.package_name())
    props = {q.name: type_of(p, q, junk) for q in st.props
             if q.name != 'UberGraphFrame' and (public is None or q.name.lower() in public)}
    functions = {}
    # A function library's protected functions are out of its API stub (and callable by no mod) as private ones are:
    hidden = FUNC_PRIVATE | (0x80000 if p.path(st.super) == '/Script/Engine.BlueprintFunctionLibrary' else 0)
    for k, e in enumerate(p.exports):
        if e['outer'] != me or p.class_of(k + 1) not in p.FUNCTION_CLASSES or e['name'].startswith('ExecuteUbergraph'):
            continue
        f = p.struct(k)
        if f.function_flags & hidden:
            continue
        functions[e['name']] = dict(
            flags=sorted(n for bit, n in FUNC_FLAGS.items() if f.function_flags & bit),
            params=[[q.name, type_of(p, q, junk)] + (['out'] if q.flags & CPF_OUT else [])
                    for q in f.props if q.flags & CPF_PARM and not q.flags & CPF_RETURN],
            returns=[type_of(p, q, junk) for q in f.props if q.flags & CPF_RETURN])
    return dict(super=p.path(st.super), interfaces=sorted(p.path(c) for c, _o, _k in st.interfaces),
                functions=functions, properties=props)


def enum_entries(p, i):
    """A UserDefinedEnum's entries in order, as name=value: after its tags, the lazy guid bool and the pair array."""
    b, o = p.blob(i), p.tags(i).end + 4
    n = struct.unpack_from('<i', b, o)[0]
    out = []
    for k in range(n):
        index, number, value = struct.unpack_from('<iiq', b, o + 4 + 16 * k)
        name = p.names[index] + ('_%d' % (number - 1) if number else '')
        if not name.endswith('_MAX'):     # the engine's own last entry: it moves with every append
            out.append('%s=%d' % (name, value))
    return out


def surface(content, stubs):
    """Every class, struct and enum the cooked build holds, by package path."""
    out = dict(classes={}, structs={}, enums={})
    for root, _dirs, files in os.walk(content):
        for f in files:
            if not f.endswith('.uasset'):
                continue
            p = invariants.load(os.path.join(root, f)[:-len('.uasset')])
            for i, e in enumerate(p.exports):
                kind = p.class_of(i + 1)
                if e['outer'] != 0:
                    continue
                if kind in CLASSES:
                    out['classes'][p.path(i + 1)] = class_surface(p, i, stubs)
                elif kind == 'UserDefinedStruct':
                    out['structs'][p.path(i + 1)] = {q.name: type_of(p, q, {}) for q in p.struct(i).props}
                elif kind == 'UserDefinedEnum':
                    out['enums'][p.path(i + 1)] = enum_entries(p, i)
    return out


def by_api(whole, spec):
    """The surface split by API: a path goes to the API of its package's longest prefix; frozen packages to none."""
    prefixes = sorted(((p.rstrip('/'), api) for api, ps in spec['apis'].items() for p in ps), key=lambda x: -len(x[0]))
    frozen = set(spec.get('frozen') or [])
    out = {api: dict(classes={}, structs={}, enums={}) for api in spec['apis']}
    for part, items in whole.items():
        for path, value in items.items():
            package = path.split('.')[0]
            api = next((a for pre, a in prefixes if package == pre or package.startswith(pre + '/')), None)
            if api and package not in frozen:
                out[api][part][path] = value
    return out


def versions(header, apis):
    text = open(header, encoding='utf-8-sig').read()
    get = lambda name: int(re.search(r'\b%s\s*=\s*(\d+)\s*;' % name, text).group(1))
    return {api: [get(api + 'Major'), get(api + 'Minor')] for api in apis}


def changes(was, now):
    """(breaking, added) between two surfaces of one API, as lines."""
    breaking, added = [], []
    for path, a in was['classes'].items():
        b = now['classes'].get(path)
        if b is None:
            breaking.append('class %s gone' % path)
            continue
        for key in ('super', 'interfaces'):
            if a[key] != b[key]:
                breaking.append('class %s: %s was %s, now %s' % (path, key, a[key], b[key]))
        for key in ('functions', 'properties'):
            for name, value in a[key].items():
                if name not in b[key]:
                    breaking.append('%s.%s gone' % (path, name))
                elif b[key][name] != value:
                    breaking.append('%s.%s was %s, now %s' % (path, name, json.dumps(value), json.dumps(b[key][name])))
            added += ['%s.%s new' % (path, n) for n in b[key] if n not in a[key]]
    for path, a in was['structs'].items():
        if now['structs'].get(path) != a:
            breaking.append('struct %s %s' % (path, 'gone' if path not in now['structs'] else 'laid out anew'))
    for path, a in was['enums'].items():
        b = now['enums'].get(path)
        if b is None or b[:len(a)] != a:
            breaking.append('enum %s %s' % (path, 'gone' if b is None else 'entries changed'))
        elif len(b) > len(a):
            added.append('enum %s: %s' % (path, ', '.join(b[len(a):])))
    for part in ('classes', 'structs', 'enums'):
        added += ['%s %s new' % (part[:-1] if part != 'classes' else 'class', p) for p in now[part] if p not in was[part]]
    return breaking, added


def check(baseline, now, numbers):
    """What stops the build, per API: a change whose version was not bumped past the release's."""
    problems = []
    for api, surf in now.items():
        old = baseline.get(api)
        if old is None:
            continue                    # an API new since the release: nothing to break
        (major, minor), (was_major, was_minor) = numbers[api], old['version']
        breaking, added = changes(old['surface'], surf)
        if (major, minor) < (was_major, was_minor):
            problems.append('%s %d.%d is older than the released %d.%d' % (api, major, minor, was_major, was_minor))
        elif breaking and major <= was_major:
            problems += ['%s: %s' % (api, line) for line in breaking[:20]]
            problems.append('%s: %d change(s) a mod built on %d.%d may use: bump %sMajor' % (api, len(breaking), was_major, was_minor, api))
        elif added and not breaking and (major, minor) == (was_major, was_minor):
            problems += ['%s: %s' % (api, line) for line in added[:20]]
            problems.append('%s: %d addition(s) since %d.%d: bump %sMinor' % (api, len(added), was_major, was_minor, api))
    return problems


def run(bp, mod, stage_content, write=False):
    """bpbuild's call, and the CLI's: True when the build may be packed."""
    contract, spec = mod['api_contract'], mod['manifest']
    baseline_path = os.path.join(bp, contract['baseline'])
    stubs = os.path.join(bp, mod['api_dir']) if mod.get('api_dir') else None
    now = by_api(surface(stage_content, stubs), spec)
    numbers = versions(os.path.join(bp, contract['versions']), spec['apis'])
    if write:
        record = {api: dict(version=numbers[api], surface=now[api]) for api in now}
        with open(baseline_path, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(record, f, indent=1, sort_keys=True)
            f.write('\n')
        print('%-16s apisurface: released %s -> %s' % (mod['name'], ', '.join('%s %d.%d' % (a, *numbers[a]) for a in now), contract['baseline']))
        return True
    if not os.path.isfile(baseline_path):
        print('%-16s apisurface: no release recorded at %s; nothing to compare' % (mod['name'], contract['baseline']))
        return True
    problems = check(json.load(open(baseline_path, encoding='utf-8')), now, numbers)
    for line in problems:
        print('%-16s apisurface: %s' % (mod['name'], line))
    if problems:
        print('%-16s apisurface: an API changed without its version (API.md "Versions"); bump it in %s, '
              'and record a release with `apisurface.py <mods dir> %s --write`' % (mod['name'], contract['versions'], mod['name']))
    return not problems


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 2:
        sys.exit(__doc__)
    import yaml
    bp, name = args
    mod = next(m for m in yaml.safe_load(open(os.path.join(bp, 'mods.yaml'), encoding='utf-8-sig'))['mods'] if m['name'] == name)
    content = os.path.join(bp, 'build', name, 'FSD', 'Content')
    return 0 if run(bp, mod, content, write='--write' in sys.argv) else 1


if __name__ == '__main__':
    sys.exit(main())
