#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/OverrideTest");

/*
The two things a subclass can restate that its own initializers cannot: an inherited component's
defaults, and an inherited plain property. Which one a UE_DEFAULTS statement means is decided by who
declares the member it names, so neither needs a Super:: spelling. The component override becomes a
UInheritableComponentHandler record keyed on the parent's SCS node GUID; the plain property becomes a
tag on this class's CDO, where re-declaring the name would instead shadow it.
*/
class BaseProp : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UPointLightComponent, Lamp);

  UE_DEFAULTS { Lamp->Intensity = 1000.0f; }
};

class DimProp : public BaseProp {
  UE_DEFAULTS {
    Lamp->Intensity = 250.0f;
    InitialLifeSpan = 3.0f;
  }
};

/*
The native side of the same idea, and a different mechanism for it: ACharacter's components are
default subobjects, not SCS nodes, so no handler is involved. Each one is overridden by an export
named after it under this class's CDO, which points back at it by name.
*/
class Walker : public ACharacter {
  UE_DEFAULTS {
    CapsuleComponent->CapsuleRadius = 55.0f;
    Mesh->bVisible = false;
  }
};
