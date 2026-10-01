#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"
#include "UeApi/Game/ENE_Spider_Grunt_Normal_C.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PreloadGameParent");

/*
A class whose parent is one of the game's Blueprints, restating none of its default subobjects. The CDO its class makes
while it is serialized copies each default subobject from the parent CDO's of the same name (UObjectGlobals.cpp
3822-3859), so the cook orders every one the parent's package exports before the class (BlueprintGeneratedClass.cpp
1437-1446, SavePackage.cpp 4013-4040): the grunt's CDO exports nine, ASpiderEnemy's (StatusEffects, Temperature,
Affliction, SceneComponent, Mesh, PathfinderMovement, HealthComponent, PawnStats, HitReactions). One copied before the
parent's export is serialized keeps the native constructor's values instead of the grunt's.
*/
class PreloadGameParent : public ENE_Spider_Grunt_Normal_C {
public:
  int32 Marker = 7;
};
