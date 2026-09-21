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
#include <map>
#include <string>
#include <utility>
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
    std::vector<FApiFunction> Functions;
    std::vector<FPropertyDef> Variables;
    std::map<std::string, std::string> Categories;   // function or variable -> its editor category, "A|B"
};

/*
Writes <OutDir>/<AssetName>.uasset. Source is the cooked package the class was built into: the
parameter defs carry FIndexes into its import table, and this resolves them to imports of its own.
A parameter whose type lives in the mod's own package (a UE_STRUCT, a UE_ENUM) has no editor-side
asset yet, so its function is skipped with a line on stdout rather than emitted half-typed.
*/
bool WriteApiAsset(const FApiClass& Class, const FPackage& Source, const std::string& OutDir, std::string* Err);

/*
The editor-side half of a mod's UE_STRUCT: an uncooked UserDefinedStruct carrying the same
GUID-suffixed compiled member names as the cooked asset, plus the UserDefinedStructEditorData the
editor recompiles from. VarGuid in each StructVariableDescription is the 32-hex tail of the member
name, so the editor's GetGuidFromName resolves the member to the same GUID our bytecode references.
*/
struct FApiStruct
{
    std::string PackageName;            // /Game/_ElytrasMods/StructTest/FStats  (same path the cooked asset uses)
    std::string StructName;             // FStats (the .uasset)
    std::vector<FPropertyDef> Members;  // GUID-suffixed compiled names, exactly as the cooked layout
    uint32 Guid[4] = { 0, 0, 0, 0 };    // the struct Guid tag, matching the cooked FinishStruct
};

bool WriteApiStructAsset(const FApiStruct& Struct, const FPackage& Source, const std::string& OutDir, std::string* Err);

/* The editor-side half of a mod's UE_ENUM: an uncooked UserDefinedEnum. Enumerators are stored as
   "<Enum>::<Entry>" (the C++ names, not the editor's NewEnumeratorN), ending in the "<Enum>_MAX"
   sentinel - so a mod's own bytecode, which uses ordinals, and a consumer's editor pin agree. */
struct FApiEnum
{
    std::string PackageName;                            // /Game/_ElytrasMods/StructTest/EMood
    std::string EnumName;                               // EMood (the .uasset)
    std::vector<std::pair<std::string, int64>> Entries; // enumerator name -> value, in declaration order
};

bool WriteApiEnumAsset(const FApiEnum& Enum, const std::string& OutDir, std::string* Err);

}   // namespace Uasset
