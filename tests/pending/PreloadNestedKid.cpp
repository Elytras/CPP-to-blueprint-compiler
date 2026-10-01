#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"
#include "UeApi/Game/WPN_Pickaxe_C.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PreloadNestedKid");

/*
A child of a game Blueprint whose default object exports subobjects nested under its default subobjects: WPN_Pickaxe's
Damage and SpecialDamage each hold an instanced BreakIceBonus_0, an archetype object under the component. The CDO this
class makes while it is serialized gets a copy of each, built from the parent's, so the cook orders every object under
the parent CDO that is a default subobject or an archetype, at any depth, before the class (SavePackage.cpp 4013-4040:
GetObjectsWithOuter includes nested objects). The pickaxes the game derives from it list both.
*/
class PreloadNestedKid : public WPN_Pickaxe_C {
public:
  int32 Swings = 0;
};
