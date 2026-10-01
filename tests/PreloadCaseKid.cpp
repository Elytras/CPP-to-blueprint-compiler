#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"
#include "UeApi/Game/ENE_Spider_Grunt_Normal_C.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PreloadCaseKid");

/*
A child of a game Blueprint restating a default subobject whose name the object dump and the parent's package spell in
different case: AEnemyDeepPathfinderCharacter's member `temperature` fills the subobject the grunt's package exports as
Temperature, and the dump, read off a running game whose name table met the property first, spells it temperature. FName
compares without case, so both are one object, which the cook imports once (SavePackage.cpp 3288-3339 builds the import
map from one mark per object): once as the override's archetype, once as a parent subobject the class is serialized
after.
*/
class PreloadCaseKid : public ENE_Spider_Grunt_Normal_C {
public:
  UE_DEFAULTS { temperature->TemperatureChangeScale = 2.0f; }
};
