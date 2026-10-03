// Assets of another mod's classes (AssetOtherShared.h, cooked by AssetOtherOwner.cpp). Each loads over its class's CDO,
// in the owner's package, whose value of a TSet / TMap the shared header says; so the braces must list the CDO's
// elements they drop as removed, as for a class of the mod's own (test_bytecode.py asset_other_class). A class of
// this mod under one of them starts from the header's value too. A game class's CDO holds a value no header says:
// there the elements are left to load, with a warning naming the member.
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

#include "AssetOtherShared.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetOtherUser");

class UAouLocal : public UAosDef {
public:
  UE_DEFAULTS {
    M = {{"c", 4}};   // C++: this CDO's M is {c: 4}, over UAosDef's {a: 1, c: 3}
  }
};

class UAouEnemy : public UEnemyDescriptor {
public:
  UE_DEFAULTS {
    BannedMissionTypes = {};    // over UEnemyDescriptor's C++ default: warned
  }
};

// C++: S {3}, M {a: 5}.
UAosDef AS_AouOver = {.S = {3}, .M = {{"a", 5}}};
// C++: S {5, 6}, over the kid's UE_DEFAULTS {2, 5}.
UAosKid AS_AouKid = {{.S = {5, 6}}};
// C++: S {7}, M {a: 2}.
UAouLocal AS_AouLocal = {{.S = {7}, .M = {{"a", 2}}}};
// C++: S {3}.
UAosNamed AS_AouNamed = {.S = {3}};
// C++: BannedMissionTypes empty; the game's CDO's value is unknown here: warned.
UEnemyDescriptor AS_AouNative = {.BannedMissionTypes = {}};
// C++: S {3}; UAosHand's CDO's value is unknown here (no initializer): warned.
AosHand::UAosHand AS_AouHand = {.S = {3}};
