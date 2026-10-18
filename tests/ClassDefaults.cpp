/*
ClassDefaults.cpp - a class as a default. `X::StaticClass()` is the value of a TSubclassOf or UClass* member wherever a
default is written: an initializer, a container's braces, a UE_STRUCT's default instance, a struct value, a subclass's
UE_DEFAULTS and an asset's braces. The cook writes it as it writes a class picked in Class Defaults, an object reference
to the class (its import, or the mod's own class's export), so it loads as that class.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"
#include "UeApi/Game/ENE_Spider_Grunt_Normal_C.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ClassDefaults");

class CdOther : public AActor {}; // another of the mod's classes, in a package of its own

struct FCdSpawn {
  UE_STRUCT;
  TSubclassOf<AActor> Kind = APlayerCharacter::StaticClass();
  int32 Count = 2;
};

class UCdDef : public UPrimaryDataAsset {
public:
  TSubclassOf<AActor> SpawnClass;
  UClass *Any;
};

class ClassDefaults : public AActor {
public:
  TSubclassOf<AActor> Kind = AActor::StaticClass();                     // an engine class
  TSubclassOf<APawn> Pawn = APlayerCharacter::StaticClass();            // a game class below the one the slot takes
  TSubclassOf<AActor> Grunt = ENE_Spider_Grunt_Normal_C::StaticClass(); // a game Blueprint
  TSubclassOf<AActor> Mine = CdOther::StaticClass();                    // another of the mod's classes
  UClass *Self = ClassDefaults::StaticClass();                          // this class
  UClass *Any = UObject::StaticClass();                                 // a class in /Script/CoreUObject
  TSubclassOf<AActor> Empty = nullptr;
  TArray<TSubclassOf<AActor>> Kinds = {AActor::StaticClass(), CdOther::StaticClass()};
  TMap<FName, TSubclassOf<AActor>> ByName = {{"spider", ENE_Spider_Grunt_Normal_C::StaticClass()}};
  FCdSpawn Spawn;
  FCdSpawn Twin = {.Kind = CdOther::StaticClass()};
};

class ClassDefaultsKid : public ClassDefaults {
public:
  UE_DEFAULTS {
    Kind = ClassDefaultsKid::StaticClass(); // the subclass's own
    Pawn = nullptr;                         // a null over the parent's class is a value too
  }
};

UCdDef CD_Asset = {.SpawnClass = CdOther::StaticClass(), .Any = AActor::StaticClass()};
