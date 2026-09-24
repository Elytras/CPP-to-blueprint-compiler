/*
GlobalTest.cpp - namespace-scope variables, and UE_ASSET_ALL.

A Blueprint has no globals. Each namespace-scope variable a function uses is kept in the default object of a class
the compiler generates for it, so every class of the mod reads and writes the same value. A `const` / `constexpr`
one is not stored at all: its uses are the value.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/GlobalTest");

int32   Counter  = 5;
FString Greeting = "hi";
namespace Tally {
int32 Hits; // no initializer: zero, as C++ zero-initializes a global
}

/* All: every UE_ASSET_AT under Picks, nested namespaces too, whose class is UEnemyDescriptor or derives from it. The
   texture is not one. */
namespace Picks {
UE_ASSET_AT(UEnemyDescriptor, Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");
namespace Boom {
UE_ASSET_AT(UEnemyDescriptor, Exploder, "/Game/Enemies/Spider/Exploder/ED_Spider_Exploder");
}
UE_ASSET_AT(UTexture2D, Web, "/Game/LevelElements/RoomObjects/Hazards/StickySpiderWeb/T_StickySpiderWeb_Corner");
UE_ASSET_ALL(UEnemyDescriptor);
} // namespace Picks

class GlobalTest : public AActor {
public:
  TSoftObjectPtr<UTexture2D> WebIcon = "/Game/LevelElements/RoomObjects/Hazards/StickySpiderWeb/T_StickySpiderWeb_Corner";

  int32 Bump(int32 By) {
    Counter += By;
    Tally::Hits += 1;
    return Counter;
  }
  int32   Take() { return Counter++; } // the value before the store
  FString Greet() { return Greeting + "!"; }
  int32   PickCount() { return Picks::All.Num(); }
  /* A soft pointer to a subclass passes as one to its parent. */
  TSoftObjectPtr<UObject> FirstPick() { return Picks::All[0]; }
};

/* Another class of the mod: the same objects, so it sees what GlobalTest wrote. */
class GlobalPeer : public AActor {
public:
  int32 Read() { return Counter * 100 + Tally::Hits; }
};
