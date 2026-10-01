#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompAttachRoot");

/*
SetupAttachment(RootComponent) puts a component under the actor's root, whichever component that is, as a constructor's
call does. Below a parent that gives the actor a root, ACharacter's capsule or a Blueprint parent's root, its node is a
root node naming no parent, which ExecuteScriptOnActor attaches to that root (SimpleConstructionScript.cpp 686). With
none to inherit, the root is the first of the class's own scene components left alone, Base though Glow is declared
first; with none of those, the DefaultSceneRoot node, which keeps Glow as its child, as the editor saves a component
added under it. Glow sits 30 above the root in each, and RootOwn's Base hands its own 50 on to it.
*/
class RootChar : public ACharacter {
public:
  UE_COMPONENT(UPointLightComponent, Glow);

  UE_DEFAULTS {
    Glow->SetupAttachment(RootComponent);
    Glow->RelativeLocation = FVector(0.0f, 0.0f, 30.0f);
  }
};

class RootBase : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
};

class RootKid : public RootBase {
public:
  UE_COMPONENT(UPointLightComponent, Glow);

  UE_DEFAULTS {
    Glow->SetupAttachment(RootComponent);
    Glow->RelativeLocation = FVector(0.0f, 0.0f, 30.0f);
  }
};

class RootOwn : public AActor {
public:
  UE_COMPONENT(UPointLightComponent, Glow);
  UE_COMPONENT(USceneComponent, Base);

  UE_DEFAULTS {
    Glow->SetupAttachment(RootComponent);
    Glow->RelativeLocation = FVector(0.0f, 0.0f, 30.0f);
    Base->RelativeLocation = FVector(0.0f, 0.0f, 50.0f);
  }
};

class CompAttachRoot : public AActor {
public:
  UE_COMPONENT(UPointLightComponent, Glow);

  UE_DEFAULTS {
    Glow->SetupAttachment(RootComponent);
    Glow->RelativeLocation = FVector(0.0f, 0.0f, 30.0f);
  }
};
