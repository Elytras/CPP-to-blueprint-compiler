/*
NsTest.cpp - a namespace is the asset's folder.

A mod's class, struct, interface or enum in `namespace A::B` is cooked in <UE_MOD_PACKAGE>/A/B. A namespace that starts
at Game is a /Game path of its own, `Game::A::X` being /Game/A/X, where genueapi puts a game Blueprint's class. What
names one - a parent, a call, a property's type, the registry - names that path.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/NsTest");

namespace Weapons {
enum class EKind : uint8 { Plain, Heavy };
UE_ENUM(EKind);

struct FAmmo {
  UE_STRUCT;
  int32 Count;
  EKind Kind;
};

class ITrigger {
  UE_INTERFACE;
  int32 Pull(int32 Times);
};

class Rifle : public AActor, public ITrigger {
public:
  int32 Shots;
  int32 Pull(int32 Times) {
    Shots += Times;
    return Shots;
  }
  int32 Load(FAmmo Ammo) { return Ammo.Kind == EKind::Heavy ? Ammo.Count * 2 : Ammo.Count; }
};
} // namespace Weapons

namespace Game::NsTestAbs {
/* Outside the mod's folder, and a child of a class in another one. */
class Pistol : public Weapons::Rifle {
public:
  int32 Pull(int32 Times) { return Weapons::Rifle::Pull(Times) + 100; }
};
} // namespace Game::NsTestAbs

class NsTest : public AActor {
public:
  Weapons::Rifle          *Gun;
  Game::NsTestAbs::Pistol *Side;
  Weapons::FAmmo           Ammo;
  int32                    Fire() { return Gun->Load(Ammo) + Side->Pull(1); }
};
