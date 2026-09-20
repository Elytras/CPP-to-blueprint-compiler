#pragma once
/*
The editor-side half of a mod: an uncooked Blueprint that carries the cooked class's callable
signatures and nothing else. Another modder opens it in the UE4.27 editor, calls the functions
from their own Blueprint, and cooks; at runtime the real pak supplies the bodies.

Stubs, not implementations - and deliberately so. The editor recompiles every Blueprint it loads,
so a hand-written BlueprintGeneratedClass would be thrown away and rebuilt from the graphs. The
graphs are therefore the payload: one function graph per callable, an entry node holding the
inputs and a result node holding the outputs, with no body between them.
*/
#include <string>
#include <vector>

#include "Package.h"
#include "Script.h"

namespace Uasset
{
struct FApiFunction
{
    std::string Name;
    std::vector<FPropertyDef> Params;
    uint32 Flags = 0;
};

struct FApiClass
{
    std::string PackageName;                 // /Game/_ElytrasMods/ReadProperty
    std::string AssetName;                   // ReadProperty (the .uasset)
    std::string ParentPackage, ParentClass;  // /Script/Engine, BlueprintFunctionLibrary
    bool bIsActor = false;
    std::vector<FApiFunction> Functions;
    std::vector<FPropertyDef> Variables;
};

/*
Writes <OutDir>/<AssetName>.uasset. Source is the cooked package the class was built into: the
parameter defs carry FIndexes into its import table, and this resolves them to imports of its own.
A parameter whose type lives in the mod's own package (a UE_STRUCT, a UE_ENUM) has no editor-side
asset yet, so its function is skipped with a line on stdout rather than emitted half-typed.
*/
bool WriteApiAsset(const FApiClass& Class, const FPackage& Source, const std::string& OutDir, std::string* Err);

}   // namespace Uasset
