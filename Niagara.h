#pragma once
/*
Niagara systems: the one piece of a cooked UNiagaraSystem that is not a tag, worked out from the tags a
UE_ASSET_DEFAULTS block lowered (Cpp.cpp, FinishAssetDefaults' caller), as the editor works it out.

UNiagaraSystem::Serialize writes, after the system's tags, EmitterCompiledData: a count, then per emitter handle one
FNiagaraEmitterCompiledData as tags (NiagaraSystem.cpp 562-593). It is no UPROPERTY, so a block cannot say it; the editor
builds it from the compiled scripts (UNiagaraSystem::InitEmitterCompiledData, NiagaraSystem.cpp 2578-2646):
  - SpawnAttributes: the system scripts' FNiagaraSpawnInfo attributes named "<emitter>.".
  - The emitter's alias variables: Emitter.SpawnInterval and its kind, "Emitter." replaced by the emitter's name.
  - DataSetCompiledData: the particle scripts' attributes (the update script's, then the spawn script's not already
    there; a GPU emitter's GPU compute script's), each attribute's register layout from its type's members
    (FNiagaraTypeLayoutInfo::GenerateLayoutInfo), the handle's IdName as the data set's name, the sim target.
The cook leaves a disabled emitter out; its compiled data, built while it was there, keeps its name, sim target and
whether it needs persistent IDs, so a block states those on a disabled emitter it keeps.
*/
#include "Script.h"

#include <functional>
#include <string>
#include <vector>

struct FNiagaraObject
{
    std::string Class;                      // the engine class's name, "NiagaraEmitter"
    std::string Name;
    int32 Outer = -1;                       // index into the list; -1 is the asset
    std::vector<Uasset::FPropertyDef> Tags; // an FDefaultValue::Instanced id is an index into the list + 1
};

/* Compiled gets one tag list per emitter handle, or none when the system scripts were never compiled, as the editor
   leaves it. Then a disabled emitter and what it holds are removed from Objects, as the cook leaves them out
   (UNiagaraEmitter::NeedsLoadForTargetPlatform), every Instanced id and Outer renumbered and a reference to one nulled;
   Kept[i] is the index the i-th remaining object had. TypeName names the object an FIndex is (a type definition's
   ClassStructOrEnum); NiagaraStruct imports a /Script/Niagara struct. False with Err on what the editor could not
   build either. */
bool CookNiagara(std::vector<Uasset::FPropertyDef>& SystemTags, std::vector<FNiagaraObject>& Objects, std::vector<int32>& Kept,
                 const std::function<std::string(Uasset::FIndex)>& TypeName,
                 const std::function<Uasset::FIndex(const std::string&)>& NiagaraStruct,
                 std::vector<std::vector<Uasset::FPropertyDef>>& Compiled, std::string* Err);
