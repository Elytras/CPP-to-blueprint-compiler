"""bpclass - browse and manage the Blueprint classes of BpMods in a terminal UI.

  bpclass.py                               the UI: folders and their classes; n new, r rename, m move, d delete, o open
  bpclass.py new <dir> <Class> [: Base]    create one class without the UI (a VS External Tool passes "$(ItemDir).")

Every change rewrites the VS project's file list (bpbuild's write_vs_filters), so Visual Studio reloads the project
with the files under the filter of their folder.
"""
import fnmatch
import os
import re
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import yaml
from bpbuild import GENERATED, MOD_PACKAGE, write_vs_filters

BP = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "BpMods"))
CLASS = re.compile(r'^[ \t]*class\s+(\w+)(?:\s+final)?\s*:\s*(?:public\s+)?(\w+)', re.M)
API_CLASS = re.compile(r'^class\s+(\w+)\b[^;\n]*$', re.M)
INCLUDE = re.compile(r'(#include\s+")([^"]+)(")')
IDENT = re.compile(r'[A-Za-z_]\w*')


@dataclass
class ClassInfo:
    name: str
    base: str
    file: str
    line: int


# --- files -----------------------------------------------------------------------------------------------------------

def read(path):
    """The text and whether it had a BOM; bytes, not text mode, so a rewrite keeps the file's line endings."""
    raw = open(path, "rb").read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    return raw[3 if bom else 0:].decode("utf-8", "surrogateescape"), bom


def write(path, text, bom=False):
    open(path, "wb").write((b"\xef\xbb\xbf" if bom else b"") + text.encode("utf-8", "surrogateescape"))


def rel(path):
    return os.path.relpath(path, BP)


def walk():
    """(folder, its .h/.cpp files) for every folder of BpMods, parents first, the build output and dumps left out."""
    for root, dirs, files in os.walk(BP):
        dirs[:] = sorted((d for d in dirs if root != BP or d not in ("build", "out") + GENERATED), key=str.lower)
        yield root, [os.path.join(root, f) for f in sorted(files, key=str.lower) if f.endswith((".h", ".cpp"))]


def all_sources():
    return [f for _, files in walk() for f in files]


def scan():
    """Folder -> the classes its sources declare, in file order. Empty folders are kept so they can take a class."""
    found = {}
    for root, files in walk():
        found[root] = []
        for path in files:
            text = read(path)[0]
            for m in CLASS.finditer(text):
                found[root].append(ClassInfo(m.group(1), m.group(2), path, text.count("\n", 0, m.start()) + 1))
    return found


def api_classes():
    """Every class the dumped UeApi headers define -> the header to include for it."""
    api, out = os.path.join(BP, "UeApi"), {}
    for root, _, files in os.walk(api):
        for f in files:
            if f.endswith(".h"):
                header = os.path.relpath(os.path.join(root, f), api).replace("\\", "/")
                for m in API_CLASS.finditer(read(os.path.join(root, f))[0]):
                    out.setdefault(m.group(1), header)
    return out


def package(folder):
    """The folder's UE_MOD_PACKAGE, from the first of its .cpp files that declares one."""
    for path in sorted(os.listdir(folder)):
        if path.endswith(".cpp"):
            m = MOD_PACKAGE.search(read(os.path.join(folder, path))[0])
            if m:
                return m.group(1)
    return None


def mods_of(cpp):
    """The mods in mods.yaml whose sources take this .cpp (it need not exist yet)."""
    mods = (yaml.safe_load(read(os.path.join(BP, "mods.yaml"))[0]) or {}).get("mods") or []
    target = os.path.normcase(os.path.abspath(cpp))
    return [m["name"] for m in mods for s in m.get("sources") or []
            if fnmatch.fnmatch(target, os.path.normcase(os.path.abspath(os.path.join(BP, s))))]


def folder_mods(folder):
    return sorted({m for f in os.listdir(folder) if f.endswith(".cpp") for m in mods_of(os.path.join(folder, f))})


def own_files(c):
    """The class's X.h / X.cpp. A class that shares a file with others can't be moved or deleted file-wise."""
    folder, stem = os.path.dirname(c.file), os.path.splitext(os.path.basename(c.file))[0]
    if stem != c.name:
        raise ValueError("%s is declared in %s among other code; give it its own %s.h first" % (c.name, rel(c.file), c.name))
    return [p for p in (os.path.join(folder, c.name + e) for e in (".h", ".cpp")) if os.path.exists(p)]


def check_new_name(name, found):
    if not IDENT.fullmatch(name):
        raise ValueError("%r is not a C++ identifier" % name)
    clash = next((c for cs in found.values() for c in cs if c.name == name), None)
    if clash:
        raise ValueError("%s already exists in %s" % (name, rel(clash.file)))


def unpicked_note(cpp):
    return [] if mods_of(cpp) else ["no mod in mods.yaml compiles %s - add it to a mod's sources" % rel(cpp)]


# --- operations: each returns notes for the user, or raises ValueError ----------------------------------------------

def create(folder, name, base, found, api):
    folder = os.path.abspath(folder)
    if not os.path.normcase(folder).startswith(os.path.normcase(BP)):
        raise ValueError("%s is not inside %s" % (folder, BP))
    check_new_name(name, found)
    h, cpp = (os.path.join(folder, name + e) for e in (".h", ".cpp"))
    for f in (h, cpp):
        if os.path.exists(f):
            raise ValueError("%s already exists" % rel(f))
    # A base from BpMods is included by its header's path; one from the dump by its UeApi header.
    local = next((c.file for cs in found.values() for c in cs if c.name == base and c.file.endswith(".h")), None)
    include = os.path.relpath(local, folder).replace("\\", "/") if local else api.get(base, "Engine.h")
    pkg = package(folder)
    body = '    UE_CLASS_IN("%s");\n' % pkg if pkg else ""
    write(h, '#pragma once\n#include "%s"\n\nclass %s : public %s\n{\npublic:\n%s};\n' % (include, name, base, body))
    write(cpp, '#include "%s.h"\n' % name)
    return ["created %s and %s.cpp" % (rel(h), name)] + unpicked_note(cpp)


def rename(c, new, found):
    """Every whole-word use in BpMods, Name_C included (the UE_CLASS name), then the files and mods.yaml."""
    check_new_name(new, found)
    files = own_files(c) if os.path.splitext(os.path.basename(c.file))[0] == c.name else []
    word = re.compile(r'\b%s(_C)?\b' % re.escape(c.name))
    changed = 0
    for path in all_sources():
        text, bom = read(path)
        out = word.sub(lambda m: new + (m.group(1) or ""), text)
        if out != text:
            write(path, out, bom)
            changed += 1
    for p in files:
        os.rename(p, os.path.join(os.path.dirname(p), new + os.path.splitext(p)[1]))
    manifest = os.path.join(BP, "mods.yaml")
    text, bom = read(manifest)
    out = re.sub(r'\b%s\.cpp\b' % re.escape(c.name), new + ".cpp", text)
    if out != text:
        write(manifest, out, bom)
    return ["renamed %s -> %s in %d files" % (c.name, new, changed),
            "other paks that reference the old %s asset will not find it" % c.name]


def move(c, dest, found):
    """Moves X.h / X.cpp, points UE_CLASS at the new folder's package, and fixes every relative include."""
    files, src = own_files(c), os.path.dirname(c.file)
    if os.path.normcase(dest) == os.path.normcase(src):
        raise ValueError("%s is already in %s" % (c.name, rel(dest)))
    moved = {os.path.normcase(p): os.path.join(dest, os.path.basename(p)) for p in files}
    for p in moved.values():
        if os.path.exists(p):
            raise ValueError("%s already exists" % rel(p))

    def retarget(from_dir, to_dir):
        def fix(m):
            target = os.path.normpath(os.path.join(from_dir, m.group(2)))
            if not os.path.exists(target) and os.path.normcase(target) not in moved:
                return m.group(0)  # not a relative include (UeApi, SDK): the include path still finds it
            target = moved.get(os.path.normcase(target), target)
            return m.group(1) + os.path.relpath(target, to_dir).replace("\\", "/") + m.group(3)
        return fix

    pkg, notes = package(dest), []
    for p in files:
        text, bom = read(p)
        text = INCLUDE.sub(retarget(src, dest), text)
        if pkg:
            text = re.sub(r'(UE_CLASS_IN\s*\(\s*")[^"]*"', r'\g<1>%s"' % pkg, text)
            text = re.sub(r'(UE_CLASS\s*\(\s*")[^"]*/%s"' % re.escape(c.name), r'\g<1>%s/%s"' % (pkg, c.name), text)
        write(moved[os.path.normcase(p)], text, bom)
        os.remove(p)
    if not pkg:
        notes.append("%s declares no UE_MOD_PACKAGE; check %s's UE_CLASS by hand" % (rel(dest), c.name))
    fixed = 0
    for path in all_sources():
        if os.path.normcase(path) in {os.path.normcase(p) for p in moved.values()}:
            continue
        text, bom = read(path)
        here = os.path.dirname(path)
        # An includer resolves against where the files were, so check the old location, not the new.
        def fix(m):
            target = os.path.normcase(os.path.normpath(os.path.join(here, m.group(2))))
            return m.group(1) + os.path.relpath(moved[target], here).replace("\\", "/") + m.group(3) if target in moved else m.group(0)
        out = INCLUDE.sub(fix, text)
        if out != text:
            write(path, out, bom)
            fixed += 1
    cpp = os.path.join(dest, c.name + ".cpp")
    return (["moved %s to %s, fixed includes in %d files" % (c.name, rel(dest), fixed)] + notes
            + (unpicked_note(cpp) if os.path.exists(cpp) else []))


def delete(c, found):
    files = own_files(c)
    for p in files:
        os.remove(p)
    word = re.compile(r'\b%s\b' % re.escape(c.name))
    users = [rel(p) for p in all_sources() if word.search(read(p)[0])]
    return ["deleted " + ", ".join(rel(p) for p in files)] + (["still used in " + ", ".join(users)] if users else [])


# --- the UI ----------------------------------------------------------------------------------------------------------

def tui():
    from textual.app import App
    from textual.containers import Horizontal, Vertical
    from textual.screen import ModalScreen
    from textual.suggester import SuggestFromList
    from textual.widgets import Footer, Header, Input, Label, OptionList, Static, Tree

    class Form(ModalScreen):
        """Asks for a few values in turn; Enter moves on, Enter on the last returns them all, Esc cancels."""
        BINDINGS = [("escape", "dismiss(None)", "Cancel")]

        def __init__(self, prompt, fields):  # fields: (placeholder, initial value, suggestions or None)
            super().__init__()
            self.prompt, self.fields = prompt, fields

        def compose(self):
            with Vertical(id="dialog"):
                yield Label(self.prompt)
                for placeholder, value, suggest in self.fields:
                    yield Input(value=value, placeholder=placeholder,
                                suggester=SuggestFromList(suggest, case_sensitive=False) if suggest else None)

        def on_input_submitted(self, event):
            inputs = list(self.query(Input))
            i = inputs.index(event.input)
            if i + 1 < len(inputs):
                inputs[i + 1].focus()
            else:
                self.dismiss([x.value.strip() for x in inputs])

    class Confirm(ModalScreen):
        BINDINGS = [("y", "dismiss(True)", "Yes"), ("n,escape", "dismiss(False)", "No")]

        def __init__(self, text):
            super().__init__()
            self.text = text

        def compose(self):
            with Vertical(id="dialog"):
                yield Label(self.text + "\n\n[b]y[/b] yes    [b]n[/b] no")

    class Pick(ModalScreen):
        BINDINGS = [("escape", "dismiss(None)", "Cancel")]

        def __init__(self, prompt, options):
            super().__init__()
            self.prompt, self.options = prompt, options

        def compose(self):
            with Vertical(id="dialog"):
                yield Label(self.prompt)
                yield OptionList(*self.options)

        def on_option_list_option_selected(self, event):
            self.dismiss(self.options[event.option_index])

    class BpClassApp(App):
        TITLE = "bpclass"
        CSS = """
        #tree { width: 50%; }
        #info { padding: 1 2; }
        Form, Confirm, Pick { align: center middle; }
        #dialog { width: 80; height: auto; max-height: 80%; padding: 1 2; border: thick $accent; background: $surface; }
        """
        BINDINGS = [("n", "new", "New"), ("r", "rename", "Rename"), ("m", "move", "Move"), ("d", "delete", "Delete"),
                    ("o", "open", "Open"), ("f5", "rescan", "Rescan"), ("q", "quit", "Quit")]

        def compose(self):
            yield Header()
            with Horizontal():
                yield Tree("BpMods", data=BP, id="tree")
                yield Static(id="info")
            yield Footer()

        def on_mount(self):
            self.api = api_classes()
            self.rescan()

        def rescan(self):
            self.found = scan()
            tree = self.query_one(Tree)
            tree.clear()
            tree.root.expand()
            nodes = {BP: tree.root}
            for folder, classes in self.found.items():  # walk order: a parent before its children
                if folder != BP:
                    nodes[folder] = nodes[os.path.dirname(folder)].add(os.path.basename(folder), data=folder, expand=True)
                for c in classes:
                    nodes[folder].add_leaf("%s [dim]: %s[/dim]" % (c.name, c.base), data=c)

        def on_tree_node_highlighted(self, event):
            d = event.node.data
            folder = os.path.dirname(d.file) if isinstance(d, ClassInfo) else d
            head = "[b]%s[/b] : %s\n%s:%d\n\n" % (d.name, d.base, rel(d.file), d.line) if isinstance(d, ClassInfo) \
                else "[b]%s[/b]  %d classes\n\n" % (rel(d), len(self.found.get(d, [])))
            self.query_one("#info", Static).update(
                head + "package  %s\nmods     %s" % (package(folder) or "-", ", ".join(folder_mods(folder)) or "-"))

        def selected(self):
            node = self.query_one(Tree).cursor_node
            return node.data if node else BP

        def selected_folder(self):
            d = self.selected()
            return os.path.dirname(d.file) if isinstance(d, ClassInfo) else d

        def selected_class(self):
            d = self.selected()
            if not isinstance(d, ClassInfo):
                self.notify("select a class first", severity="warning")
            return d if isinstance(d, ClassInfo) else None

        def apply(self, op, *args):
            try:
                notes = op(*args)
            except (ValueError, OSError) as e:
                self.notify(str(e), severity="error", timeout=10)
                return
            write_vs_filters(BP)
            self.rescan()
            for n in notes:
                self.notify(n, timeout=10)

        def action_new(self):
            folder = self.selected_folder()
            bases = sorted({c.name for cs in self.found.values() for c in cs} | set(self.api))
            self.push_screen(Form("New class in %s" % rel(folder), [("Class name", "", None), ("Base (UObject)", "", bases)]),
                             lambda v: v and v[0] and self.apply(create, folder, v[0], v[1] or "UObject", self.found, self.api))

        def action_rename(self):
            c = self.selected_class()
            if c:
                self.push_screen(Form("Rename %s" % c.name, [("New name", c.name, None)]),
                                 lambda v: v and v[0] != c.name and self.apply(rename, c, v[0], self.found))

        def action_move(self):
            c = self.selected_class()
            if c:
                targets = {rel(f): f for f in self.found if os.path.normcase(f) != os.path.normcase(os.path.dirname(c.file))}
                self.push_screen(Pick("Move %s to" % c.name, list(targets)),
                                 lambda v: v and self.apply(move, c, targets[v], self.found))

        def action_delete(self):
            c = self.selected_class()
            if c:
                self.push_screen(Confirm("Delete %s (%s)?" % (c.name, rel(c.file))),
                                 lambda yes: yes and self.apply(delete, c, self.found))

        def action_open(self):
            d = self.selected()
            os.startfile(d.file if isinstance(d, ClassInfo) else d)

        def action_rescan(self):
            self.rescan()

    BpClassApp().run()


def main():
    args = [a for a in sys.argv[1:] if a != ":"]
    if not args:
        return tui()
    if args[0] != "new" or len(args) < 3:
        sys.exit(__doc__.strip())
    try:
        notes = create(args[1], args[2], args[3] if len(args) > 3 else "UObject", scan(), api_classes())
    except ValueError as e:
        sys.exit(str(e))
    write_vs_filters(BP)
    print("\n".join(notes))


if __name__ == "__main__":
    main()
