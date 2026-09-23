# AssetGen

A compiler from C++ to cooked Unreal Engine 4.27 Blueprint assets. You write a mod as ordinary C++
against the game's reflected API; AssetGen turns it into a `BlueprintGeneratedClass` package
(`.uasset` + `.uexp`, Kismet bytecode included) that the game loads from a `_P.pak`. You don't need
the editor, and you don't ship a DLL.

The only target so far is Deep Rock Galactic (UE 4.27, cooked, unversioned, tagged properties).

## Requirements

| What | Why |
| --- | --- |
| Visual Studio 2022 (v143, x64) | builds `assetgen.exe` |
| [vcpkg](https://github.com/microsoft/vcpkg) with `vcpkg integrate install` | `nlohmann-json`, restored from `vcpkg.json` on first build |
| LLVM `clang++` on `PATH` | AssetGen parses your source with clang's JSON AST dump |
| Python 3 + `pyyaml` | `tools/bpbuild.py`, the multi-mod build driver |
| UE 4.27 `UnrealPak.exe` (optional) | packs the result; set `UNREALPAK=<path>` if it isn't at the default Epic install path |
| The game SDK headers (`UeApi/`) | what your mod `#include`s. Get them from the companion SDK repo, or generate your own with `tools/genueapi.py` |

## Build

Open `AssetGen.sln`, build **Release | x64**, and you get `x64\Release\assetgen.exe`.

## Use

One source file:

```
assetgen compile <Mod.cpp> <UeApi dir> <out dir> --api <UeApi dir>
```

The out dir has to exist already. A mod declares its package and derives from a game class:

```cpp
#include "UeApi/Types.h"
#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_MyMods/Hello");

class Hello : public AActor {
  int32 Count = 0;
  void ReceiveBeginPlay() { Count += 1; }
};
```

Several mods, only rebuilding what changed, packed into paks:

```
python tools/bpbuild.py <mods dir> <UeApi dir> <path\to\assetgen.exe> [--force] [--no-pak]
```

`<mods dir>` holds a `mods.yaml`:

```yaml
mods:
  - name: Hello                # -> out/Hello_P.pak
    sources: [Hello.cpp]       # .cpp files get compiled; other listed files only count for staleness
    needs: [OtherMod]          # optional: build these first
    embed: true                # optional: also pack the needed mods' assets into this pak
```

## Generating the SDK yourself

1. Dump the game with the [Dumper-7 fork](https://github.com/Elytras/Dumper-7) with `FullAssetPaths=1`
   in `Dumper-7.ini`. Upstream Dumper-7 doesn't write the package paths genueapi reads.
2. `python tools/genueapi.py <dump>\SDK\SDK <UeApi dir>`
3. Copy the hand-written `UeMeta.h` and `Types.h` from the SDK repo into `<UeApi dir>`.

## Reporting bugs

Use the **Compiler bug** issue form. It asks for everything needed to reproduce the bug: the AssetGen
commit, the SDK version, the minimal source, the full output and, for anything that compiled, the
generated `.uasset`/`.uexp`. An issue that can't be reproduced can't be fixed.

## Other tools

`tools/` also holds the inspection scripts used while developing the writer: `dumpexp.py` (export
table / serialized bytes), `walkscript.py` (a function's bytecode, expression by expression),
`dumptags.py`, `dumpstruct.py`, `dumpedl.py`, `dumpar.py` (AssetRegistry.bin), and `runscript.py`
(an offline Kismet interpreter).
