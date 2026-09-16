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

}   // namespace Uasset
