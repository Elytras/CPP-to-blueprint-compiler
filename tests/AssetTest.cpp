/*
AssetTest.cpp - objects declared statically, and references to them.

Declaring: a namespace-scope variable of a UE class with a braced initializer cooks as an instance of that
class in its own package, /Game/_ElytrasMods/AssetTest/<variable name>. Only the members the braces name
are written, the rest keep the class defaults. The class is a mod class or a native one; its members must
be public, or it is not an aggregate. UE_ASSET_AT names an asset some other package already holds.

Referencing: `&MD_Big` is that object, wherever an object is expected - a member default (what picking the
asset in the editor's details panel does), an element of a TArray / TSet / TMap default, another asset's
member, or an expression in a function body.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetTest");

enum class EMood : uint8 { Calm, Angry };
UE_ENUM(EMood);

class UMoodDef : public UPrimaryDataAsset {
public:
  float          Health = 100;
  int32          Count  = 3;
  FString        Title  = "Base";
  FName          Tag;
  EMood          Mood = EMood::Angry;
  bool           bBig = false;
  UMoodDef      *Next = nullptr;
  TArray<int32>  Waves;
};

UMoodDef MD_Plain = {};
UMoodDef MD_Calm  = {.Count = 0, .Mood = EMood::Calm}; // an explicit zero is written, not dropped
UMoodDef MD_Big   = {.Health = -500.5f, .Title = "Big", .Tag = "big", .bBig = true, .Next = &MD_Calm, .Waves = {3, 5, 8}};

/* A native class works the same way, and so does pointing at the game's own assets. */
UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");
UEnemyDescriptor ED_AssetTest = {.VeteranClasses = {&ED_Spider_Grunt}, .SpawnSpread = 250.0f, .IdealSpawnSize = 4,
                                 .CanBeUsedForConstantPressure = true};

class AssetUser : public AActor {
public:
  UMoodDef                   *Picked = &MD_Big;
  TArray<UEnemyDescriptor *>  Enemies = {&ED_Spider_Grunt, &ED_AssetTest};
  TSet<FName>                 Tags = {"big", "calm"};
  TMap<FName, UMoodDef *>     ByName = {{"big", &MD_Big}, {"calm", &MD_Calm}};
  TMap<int32, float>          Scale = {{1, 0.5f}, {2, -2.0f}};

  void ReceiveBeginPlay() {
    UMoodDef *Def = &MD_Calm;
    Say("Picked " + Picked->Title + ", calm count " + UKismetStringLibrary::Conv_IntToString(Def->Count));
  }

  inline void Say(FString Msg) { UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Msg); }
};
