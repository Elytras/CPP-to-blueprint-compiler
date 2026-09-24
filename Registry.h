#pragma once
/*
Writer for a cooked AssetRegistry.bin (UE 4.27 FAssetRegistryState::Save, version FixedTags).
Cooked packages carry no registry data of their own; the engine appends this file per content plugin.
*/
#include <string>
#include <vector>

#include "SharedLib/core/Types.h"

namespace Uasset
{
/* ObjectPath and PackagePath are derived from these exactly as FAssetData's constructor does. */
struct FRegistryAsset
{
    std::string PackageName;
    std::string AssetName;
    std::string AssetClass;
    uint32 PackageFlags = 0x80000000;        // PKG_FilterEditorOnly
};

bool SaveAssetRegistry(const std::vector<FRegistryAsset>& Assets, const std::string& OutPath,
                       std::string* Err);

/* The rows of a registry SaveAssetRegistry wrote. Anything with tags or dependencies (an editor-cooked one) is refused
   rather than half-read. */
bool LoadAssetRegistry(const std::string& Path, std::vector<FRegistryAsset>& Out, std::string* Err);

/* Path's rows, with every package Assets names replaced by Assets' rows, written back to Path (a missing Path starts
   empty). A pak has one registry, which each compile into it, and each dependency it embeds, adds to. */
bool MergeAssetRegistry(const std::vector<FRegistryAsset>& Assets, const std::string& Path, std::string* Err);

}   // namespace Uasset
