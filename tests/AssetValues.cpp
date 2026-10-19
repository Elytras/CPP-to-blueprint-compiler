/*
AssetValues.cpp - S38: a game asset's values used when the mod is built. The suite compiles it with --game at
AssetTest's output, so the "game" here is AssetTest: its UMoodDef assets and class, declared below the way a UeApi
header declares a game Blueprint, and its ED_AssetTest. `MD_Big.Health` is the value MD_Big's package holds, a
constant in the code and in a default; one the asset does not set is its archetype's, the class's default object.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetValues");

class UMoodDef : public UPrimaryDataAsset {
public:
  UE_CLASS("/Game/_ElytrasMods/AssetTest/UMoodDef", "UMoodDef_C");
  float         Health;
  int32         Count;
  FString       Title;
  FName         Tag;
  bool          bBig;
  TArray<int32> Waves;
};

UE_ASSET_AT(UMoodDef, MD_Plain, "/Game/_ElytrasMods/AssetTest/MD_Plain");
UE_ASSET_AT(UMoodDef, MD_Calm, "/Game/_ElytrasMods/AssetTest/MD_Calm");
UE_ASSET_AT(UMoodDef, MD_Big, "/Game/_ElytrasMods/AssetTest/MD_Big");
UE_ASSET_AT(UEnemyDescriptor, ED_AssetTest, "/Game/_ElytrasMods/AssetTest/ED_AssetTest");

class AssetValues : public AActor {
public:
  float   Twice = MD_Plain.Health * 2; // the class's 100, through the asset's archetype
  FString Title = MD_Big.Title;
  FName   Label = MD_Big.Tag;
  int32   Size  = ED_AssetTest.IdealSpawnSize;

  float BigHealth() { return MD_Big.Health * 2; }     // MD_Big's own -500.5
  float PlainHealth() { return MD_Plain.Health; }     // none of its own: UMoodDef_C's default object's
  int32 CalmCount() { return MD_Calm.Count + 1; }     // an explicit zero
  int32 BigWave() { return MD_Big.Waves[2]; }         // an element
  bool  PlainBig() { return MD_Plain.bBig; }          // the class's own false, which its default object leaves out
  float Spread() { return ED_AssetTest.SpawnSpread; } // a native class's asset
};
