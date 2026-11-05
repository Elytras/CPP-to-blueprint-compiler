#pragma once
/*
Cascade particle systems: the work DRG's cooker does on a UParticleSystem, done on the tags a UE_ASSET_DEFAULTS block
lowered (Cpp.cpp, FinishAssetDefaults' caller), before they are written.

  - Each FRawDistributionFloat / FRawDistributionVector whose Distribution is a Constant or Uniform object of the block
    is baked into its lookup table (FRawDistribution::Initialize), MinValue / MaxValue filled, Distribution null, and
    the object left out of the package, as UDistribution*::NeedsLoadForClient leaves a baked one out of a cooked one.
  - A raw distribution a module leaves unset gets the module's starting value, baked: Cascade's editor gives a new
    module its distributions in InitializeDefaults, which never runs for an object loaded from a package, so unset
    it reads as 0 in game (an unset BurstScale turns off every burst).
  - Each module's LODValidity is derived from the LOD levels that name it, each LOD level's Level from its place.
  - What the engine would assert on or patch at load is refused.
*/
#include "Script.h"

#include <string>
#include <utility>
#include <vector>


struct FCascadeObject
{
    std::string Class;                  // the engine class's name, "ParticleModuleSpawn"
    std::string Name;
    int32 Outer = -1;                   // index into the list; -1 is the asset
    std::vector<Uasset::FPropertyDef> Tags;     // an FDefaultValue::Instanced id is an index into the list + 1
    /* The class's FRawDistribution members by their tag names, each with whether it is a vector one. */
    std::vector<std::pair<std::string, bool>> RawDists;
};

/* Rewrites SystemTags and Objects in place. The objects left out (baked distributions) are removed from Objects and
   every Instanced id and Outer renumbered; Kept[i] is the index the i-th remaining object had. Warnings are
   "<where>: ..." lines; false with Err on a refusal. */
bool CookCascade(std::vector<Uasset::FPropertyDef>& SystemTags, std::vector<FCascadeObject>& Objects, std::vector<int32>& Kept,
                 std::vector<std::string>& Warnings, std::string* Err);
