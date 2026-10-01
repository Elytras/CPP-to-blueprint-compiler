/*
EditTest.cpp - S38: assets another package already holds, edited in place (UE_ASSET_EDIT), and a Blueprint's class
defaults (UE_PATCH). The suite compiles it with --game at AssetTest's output, so the "game" here is AssetTest: its
ED_AssetTest, and its UMoodDef class, declared below the way a UeApi header declares a game Blueprint. Each edited
package is written to its own path, beside this mod's (none of its own here).
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/EditTest");

UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");
UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Exploder, "/Game/Enemies/Spider/Exploder/ED_Spider_Exploder");
UE_ASSET_AT(UEnemyDescriptor, ED_AssetTest, "/Game/_ElytrasMods/AssetTest/ED_AssetTest");

/* Replaced in place: VeteranClasses, SpawnSpread, and the bool, now false. Added: EnemySignificance. The array's new
   element is an object the package did not import yet. */
UE_ASSET_EDIT(ED_AssetTest) {.VeteranClasses = {&ED_Spider_Grunt, &ED_Spider_Exploder},
                             .EnemySignificance = EEnemySignificance::Critical, .SpawnSpread = 800.0f,
                             .CanBeUsedForConstantPressure = false};

class UMoodDef : public UPrimaryDataAsset {
public:
  UE_CLASS("/Game/_ElytrasMods/AssetTest/UMoodDef", "UMoodDef_C");
  float   Health;
  int32   Count;
  FString Title;
  FName   Tag;
};

class MoodTweaks : public UMoodDef {
  UE_PATCH;
  UE_DEFAULTS {
    Health = 42.0f; // replaces the class's own 100
    Count  = 0;     // a zero replaces a value too
    Tag    = "tweaked"; // a tag the default object did not have
  }
};
