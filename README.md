# AssetGen

A compiler from C++ to cooked Unreal Engine 4.27 Blueprint assets. You write a mod as ordinary C++
against the game's reflected API; AssetGen turns it into a `BlueprintGeneratedClass` package
(`.uasset` + `.uexp`, Kismet bytecode included) that the game loads from a `_P.pak`. You don't need
the editor, and you don't ship a DLL.

The only target so far is Deep Rock Galactic (UE 4.27, cooked, unversioned, tagged properties).

## Requirements

AssetGen builds and runs on Windows and on Linux; both write byte-identical assets.

| What | Windows | Linux |
| --- | --- | --- |
| C++17 compiler | Visual Studio 2022 (v143, x64) | GCC or clang (tested with GCC 13 and clang 18), CMake 3.16+ |
| `nlohmann-json` | [vcpkg](https://github.com/microsoft/vcpkg) with `vcpkg integrate install`, restored from `vcpkg.json` on first build | the distro package (`nlohmann-json3-dev`, `nlohmann-json`), else CMake downloads it |
| LLVM `clang++` on `PATH` | yes | yes. AssetGen parses your source with clang's JSON AST dump |
| Python 3 + `pyyaml` | yes | yes. For `tools/bpbuild.py`, the multi-mod build driver |
| UE 4.27 `UnrealPak` (optional) | `UnrealPak.exe`, found at the default Epic install path or `UNREALPAK=<path>` | `UnrealPak` on `PATH`, or `UNREALPAK=<path>` (see [Packing on Linux](#packing-on-linux)) |
| The game SDK headers (`UeApi/`) | yes | yes. What your mod `#include`s. Get them from [DRG-Blueprint-Cpp-SDK](https://github.com/Elytras/DRG-Blueprint-Cpp-SDK), or generate your own with `tools/genueapi.py` |

## Build

**Windows:** open `AssetGen.sln`, build **Release | x64**, and you get `x64\Release\assetgen.exe`.

**Linux:**

```sh
sudo apt install build-essential cmake clang python3-yaml nlohmann-json3-dev   # Debian / Ubuntu
cmake -B build
cmake --build build -j
```

and you get `build/assetgen`. On Arch the packages are `base-devel cmake clang python-yaml nlohmann-json`,
on Fedora `gcc-c++ cmake clang python3-pyyaml json-devel`.

On Linux, clang still parses your mod for the game's target (`x86_64-pc-windows-msvc`), so `sizeof`,
`long` and `wchar_t` mean what they mean in the game. That target has no C++ standard library on Linux,
so `<initializer_list>`, which the SDK needs, is the only standard header a mod can include there. Use
clang builtins such as `__is_same(A, B)` instead of `<type_traits>`.

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

A variable outside any class, such as `int32 Total = 0;` at namespace scope, is shared by every class that uses
it. The compiler keeps it in the default object of a class it generates for that variable.

Several mods, only rebuilding what changed, packed into paks:

```
python tools/bpbuild.py <mods dir> <UeApi dir> <path to assetgen> [--force] [--no-pak]
```

For example `python tools\bpbuild.py mods ..\DrgCppSdk\UeApi x64\Release\assetgen.exe` on Windows, or
`python3 tools/bpbuild.py mods ../DrgCppSdk/UeApi build/assetgen` on Linux.

`<mods dir>` holds a `mods.yaml`:

```yaml
mods:
  - name: Hello                # -> out/Hello_P.pak
    sources: [Hello.cpp]       # .cpp files get compiled; other listed files only count for staleness
    needs: [OtherMod]          # optional: build these first
    embed: true                # optional: also pack the needed mods' assets into this pak
```

### Packing on Linux

A native Linux UnrealPak (from a source-built UE 4.27) goes on `PATH` or in `UNREALPAK`. Otherwise run the
Windows `UnrealPak.exe` under Wine, straight from a UE 4.27 install (a dual-boot Windows drive
works, read-only is fine) or a copy of its `Engine` folder. Save a wrapper script:

```sh
#!/bin/sh
exec wine "$HOME/UE_4.27/Engine/Binaries/Win64/UnrealPak.exe" "$@"
```

`chmod +x` it and point `UNREALPAK` at it:

```sh
UNREALPAK=~/bin/unrealpak-wine python3 tools/bpbuild.py mods ../DrgCppSdk/UeApi build/assetgen
```

With `--no-pak` (or no UnrealPak at all) bpbuild still compiles every mod and leaves the assets under
`<mods dir>/build/<mod>/FSD/Content` for you to pack yourself, with the files under
`../../../FSD/Content/...` in the pak.

Under Proton the game reads paks from
`steamapps/common/Deep Rock Galactic/FSD/Content/Paks/`, the same folder as on Windows.

## Generating the SDK yourself

1. Dump the game with the [Dumper-7 fork](https://github.com/Elytras/Dumper-7) with `FullAssetPaths=1`
   in `Dumper-7.ini`. Upstream Dumper-7 doesn't write the package paths genueapi reads. (soon will)
2. `python tools/genueapi.py <dump>/SDK/SDK <UeApi dir>`. This step runs on Linux too, from the dump's folder
   on the Windows drive or a copy of it.
3. Copy the hand-written `UeMeta.h` and `Types.h` from [the SDK repo](https://github.com/Elytras/DRG-Blueprint-Cpp-SDK/tree/main/UeApi) into `<UeApi dir>`.
4. Optionally, `UeAssets/`, every game asset named by its path (`&UeAssets::USoundWave::Game::Audio::...::Name`):
   extract `FSD/AssetRegistry.bin` from the game's pak
   (`UnrealPak <pak> -Extract <dir> -Filter=*AssetRegistry.bin`), then
   `python tools/genueassets.py <dir>/FSD/AssetRegistry.bin <UeApi dir> <UeAssets dir>`. Put `UeAssets/` beside
   `UeApi/` so a mod can `#include "UeAssets/USoundWave.h"`. `UeAssets::USoundWave::All` is every one of them,
   as soft pointers.

## Tests

```
python tools/test_bytecode.py [--assetgen <assetgen binary>] [--ueapi <UeApi dir>]
```

compiles every mod in `tests/` and checks what a mod can observe: return values and side effects (run offline
through `tools/runscript.py` / `tools/runvm.py`), and what the engine sees (exports, flags, property types,
defaults, the registry). It never checks the exact bytecode, so an optimization that keeps the behaviour passes.
CI runs it on Linux and Windows against the [SDK repo](https://github.com/Elytras/DRG-Blueprint-Cpp-SDK) on
every push and pull request.

## Reporting bugs

Use the **Compiler bug** issue form. It asks for everything needed to reproduce the bug: the AssetGen
commit, the SDK version, the minimal source, the full output and, for anything that compiled, the
generated `.uasset`/`.uexp`. An issue that can't be reproduced can't be fixed.

## Other tools

`tools/` also holds the inspection scripts used while developing the writer: `dumpexp.py` (export
table / serialized bytes), `walkscript.py` (a function's bytecode, expression by expression),
`dumptags.py`, `dumpstruct.py`, `dumpedl.py`, `dumpar.py` (an AssetRegistry.bin, assetgen's or the game's), and
`runscript.py` (an offline Kismet interpreter).

## License

GPL-3.0, see `LICENSE`.
