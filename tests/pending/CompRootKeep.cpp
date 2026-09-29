#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompRootKeep");

/*
The actor's root is the first scene component of the class that runs its construction first: a mod parent's, or the
native parent's (ACharacter's capsule). A subclass's own scene components then attach to that root
(USimpleConstructionScript::ExecuteScriptOnActor passes the actor's RootComponent as their parent), and an attached
component keeps its relative transform. So Pivot sits 100 above the inherited root and Glow 10 in front of Pivot: each
keeps the RelativeLocation written here, and none is "the actor's root" to warn about.
*/
class RigBase : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UStaticMeshComponent, Body);
};

class CompRootKeep : public RigBase {
public:
  UE_COMPONENT(USceneComponent, Pivot);
  UE_COMPONENT(UPointLightComponent, Glow);

  UE_DEFAULTS {
    Pivot->RelativeLocation = FVector(0.0f, 0.0f, 100.0f);
    Glow->RelativeLocation = FVector(10.0f, 0.0f, 0.0f);
  }
};

class RigChar : public ACharacter {
public:
  UE_COMPONENT(USceneComponent, Pivot);
  UE_COMPONENT(UPointLightComponent, Glow);

  UE_DEFAULTS {
    Pivot->RelativeLocation = FVector(0.0f, 0.0f, 100.0f);
    Glow->RelativeLocation = FVector(10.0f, 0.0f, 0.0f);
  }
};

/* A first own scene component that draws something: attached below RigBase's root, its offset applies. */
class RigSpot : public RigBase {
public:
  UE_COMPONENT(UPointLightComponent, Spot);

  UE_DEFAULTS { Spot->RelativeLocation = FVector(0.0f, 0.0f, 50.0f); }
};
