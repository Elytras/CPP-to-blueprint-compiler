#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PreloadDso");

/*
ACharacter's capsule is a default subobject its constructor makes: each class restates its default through an export
under its CDO. The loader checks that export's TemplateIndex is set and fetches it serialized (AsyncLoading.cpp 2955,
3191). Its archetype is the subobject of the same name under the parent CDO: Default__Character:CollisionCylinder for
PreloadDsoBase, PreloadDsoBase's own export for PreloadDso - which the child class is serialized after, since the CDO
it makes then builds its capsule from that archetype (BlueprintGeneratedClass.cpp 1437-1446). PreloadDsoKid restates
nothing, so it exports no capsule: its CDO's capsule is only ever the copy of PreloadDsoBase's (UObjectGlobals.cpp
3844-3859), and its class is serialized after that export all the same.
*/
class PreloadDsoBase : public ACharacter {
  UE_DEFAULTS {
    CapsuleComponent->CapsuleRadius = 40.0f;
    CapsuleComponent->CapsuleHalfHeight = 120.0f;
  }
};

class PreloadDso : public PreloadDsoBase {
  UE_DEFAULTS { CapsuleComponent->CapsuleRadius = 50.0f; }
};

class PreloadDsoKid : public PreloadDsoBase {
public:
  int32 Marker = 7;
};
