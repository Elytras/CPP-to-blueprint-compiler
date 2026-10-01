#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SubobjectChain");

/*
A default-subobject override's archetype is the subobject of the same name under its outer's archetype - for an
override under this class's CDO, the parent CDO's (UObjectArchetype.cpp GetArchetypeFromRequiredInfo). The loader
constructs the export on that template and takes every default the export does not write from it. So the base's
override stands on Default__Character's CollisionCylinder, and the kid's on the base's own override: the kid keeps
what the base set and changes only the radius.
*/
class SubobjectChainBase : public ACharacter {
  UE_DEFAULTS {
    CapsuleComponent->CapsuleRadius = 55.0f;
    CapsuleComponent->CapsuleHalfHeight = 120.0f;
  }
};

class SubobjectChain : public SubobjectChainBase {
  UE_DEFAULTS { CapsuleComponent->CapsuleRadius = 70.0f; }
};

/*
A class that leaves the capsule alone, below one that restates it: the tip's override stands on the middle class's
CollisionCylinder, which the middle class exports with no tags so that the tip's import finds it.
*/
class SubobjectChainMid : public ACharacter {
public:
  int32 Marker = 3;
};

class SubobjectChainTip : public SubobjectChainMid {
  UE_DEFAULTS { CapsuleComponent->CapsuleHalfHeight = 30.0f; }
};
