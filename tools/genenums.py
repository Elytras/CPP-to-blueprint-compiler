#!/usr/bin/env python3
"""usage: genenums.py <path to UE_4.27/Engine/Source/Runtime>"""
import re, io, os, sys

if len(sys.argv) < 2:
    sys.exit(__doc__.strip().splitlines()[-1])

UE = sys.argv[1].replace("\\", "/").rstrip("/")
SCRIPT = UE + "/CoreUObject/Public/UObject/Script.h"
MACROS = UE + "/CoreUObject/Public/UObject/ObjectMacros.h"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "UeEnums.h")

def grab(path, enum_name):
    """(name, value, comment) triples from one enum block."""
    text = io.open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"enum\s+" + enum_name + r"\s*(?::\s*\w+\s*)?\{", text)
    if not m:
        sys.exit("enum not found: " + enum_name)
    depth, i = 1, m.end()
    while depth:
        if text[i] == '{': depth += 1
        elif text[i] == '}': depth -= 1
        i += 1
    body = text[m.end():i-1]
    out = []
    for line in body.splitlines():
        mm = re.match(r"\s*([A-Za-z_]\w*)\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uUlL]*\s*,?\s*(?://+/?<?\s*(.*))?$", line)
        if mm:
            out.append((mm.group(1), int(mm.group(2), 0), (mm.group(3) or "").strip()))
    return out

def emit(f, cpp_name, underlying, rows, doc):
    f.write("/* %s */\n" % doc)
    f.write("enum %s : %s\n{\n" % (cpp_name, underlying))
    width = max(len(r[0]) for r in rows)
    for name, val, comment in rows:
        lit = {"uint64": "0x%016X", "uint32": "0x%08X", "uint8": "0x%02X", "uint16": "0x%02X"}[underlying] % val
        line = "    %-*s = %s," % (width, name, lit)
        f.write(line + (("   // " + comment) if comment else "") + "\n")
    f.write("};\n\n")

expr = grab(SCRIPT, "EExprToken")
cast = grab(SCRIPT, "ECastToken")
func = grab(SCRIPT, "EFunctionFlags")
prop = grab(MACROS, "EPropertyFlags")
klass = grab(MACROS, "EClassFlags")
obj = grab(MACROS, "EObjectFlags")

f = io.open(OUT, "w", encoding="utf-8-sig", newline="\n")
f.write("""#pragma once
/*
UeEnums.h — UE 4.27's serialization enums, generated from the engine headers.

These are the vocabularies a package writer has to speak exactly: Kismet opcodes, and the
flag words stored on functions, properties, classes and export rows. They are generated
rather than hand-copied (tools/genenums.py) because a single mistyped bit produces an
asset that loads and then misbehaves, which is far worse than one that fails outright.

Emitting an opcode is a separate question from naming it: the full EExprToken set is here so
the assembler can *name* anything it meets, while ExprName() plus a TODO marker tells you
which ones it cannot yet write.
*/
#include "SharedLib/core/Types.h"

namespace Uasset
{
""")
emit(f, "EExprToken", "uint16", expr, "Kismet bytecode opcodes (FBlueprintBytecode / Script.h).")
emit(f, "ECastToken", "uint16", cast, "Operands of EX_PrimitiveCast.")
emit(f, "EFunctionFlags", "uint32", func, "UFunction::FunctionFlags.")
emit(f, "EPropertyFlags", "uint64", prop, "FProperty::PropertyFlags.")
emit(f, "EClassFlags", "uint32", klass, "UClass::ClassFlags.")
emit(f, "EObjectFlags", "uint32", obj, "Export-row object flags.")

f.write("/* Every opcode's spelling, for diagnostics and the assembler's unimplemented path. */\n")
f.write("inline const char* ExprName(uint8 Op)\n{\n    switch (Op)\n    {\n")
seen = set()
for name, val, _ in expr:
    if val in seen: continue
    seen.add(val)
    f.write('    case 0x%02X: return "%s";\n' % (val, name))
f.write('    default: return "EX_<unknown>";\n    }\n}\n\n}   // namespace Uasset\n')
f.close()
print("EExprToken %d, ECastToken %d, EFunctionFlags %d, EPropertyFlags %d, EClassFlags %d, EObjectFlags %d"
      % (len(expr), len(cast), len(func), len(prop), len(klass), len(obj)))
