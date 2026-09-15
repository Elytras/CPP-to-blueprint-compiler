#pragma once
/*
Registry.h — a writer for AssetRegistry.bin, the cooked FAssetRegistryState.

A cooked .uasset carries no asset-registry data of its own: SavePackage writes ObjectCount = 0
into the in-package section whenever PKG_FilterEditorOnly is set, and on the read side
AssetDataGatherer calls ReadAssetRegistryDataIfCookedPackage FIRST, which synthesizes an
FAssetData from the export table and never looks at that section. So the only way a generated
class is in the registry without something calling ScanPathsSynchronous for it is a baked
AssetRegistry.bin — which the engine appends, per content plugin, at registry startup
(AssetRegistry.cpp: `ContentPlugin->GetBaseDir() / "AssetRegistry.bin"`).

The format is UE 4.27's FAssetRegistryState::Save at version FixedTags: a version guid, an FName
batch, a fixed tag store, then the asset rows. Generated mods carry no tags, so the store is
written empty and every row points at the empty map — which is the whole reason this is a few
hundred bytes of writer rather than a port of FStoreBuilder.
*/
#include <string>
#include <vector>

#include "SharedLib/core/Types.h"

namespace Uasset
{
/*
One registry row: the asset export of one generated package.

`PackageName` is the /Game path of the package, `AssetName` the export the package marks
bIsAsset (a generated class, so "Foo_C"), and `AssetClass` that export's class name. The
remaining FAssetData fields the loader reads — ObjectPath and PackagePath — are derived from
these exactly as FAssetData's own constructor derives them.
*/
struct FRegistryAsset
{
    std::string PackageName;                 // "/Game/_Mods/CppTest/InitMod_Hello"
    std::string AssetName;                   // "InitMod_Hello_C"
    std::string AssetClass;                  // "BlueprintGeneratedClass"
    uint32 PackageFlags = 0x80000000;        // PKG_FilterEditorOnly, as the package writer sets
};

/* Writes OutPath (an AssetRegistry.bin). Returns false and fills Err on a write failure. */
bool SaveAssetRegistry(const std::vector<FRegistryAsset>& Assets, const std::string& OutPath,
                       std::string* Err);

}   // namespace Uasset
