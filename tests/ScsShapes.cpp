#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ScsShapes");

/*
The construction-script shapes AssetGen writes, three classes deep. ScsShapes declares a component that is
not a scene component before its first scene one, so the root is the first SCENE component (Pivot) and not
the first declared; every later scene component attaches to Pivot; the movement components attach to
nothing. ScsShapesKid adds a scene component of its own, which attaches to the root its parent's
construction script made, and overrides one inherited component's defaults. ScsShapesGrandkid overrides a
component of its grandparent that its parent left alone, and one its parent declared.
*/
class ScsShapes : public AActor {
public:
  UE_COMPONENT(UProjectileMovementComponent, Mover);
  UE_COMPONENT(USceneComponent, Pivot);
  UE_COMPONENT(UStaticMeshComponent, Arm);
  UE_COMPONENT(UPointLightComponent, Glow);
  UE_COMPONENT(URotatingMovementComponent, Spinner);

  UE_DEFAULTS {
    Mover->InitialSpeed = 1200.0f;
    Glow->Intensity = 1000.0f;
  }
};

class ScsShapesKid : public ScsShapes {
public:
  UE_COMPONENT(UAudioComponent, Hum);

  UE_DEFAULTS {
    Glow->Intensity = 250.0f;
    Hum->VolumeMultiplier = 0.75f;
  }
};

class ScsShapesGrandkid : public ScsShapesKid {
  UE_DEFAULTS {
    Arm->bVisible = false;
    Hum->PitchMultiplier = 2.0f;
  }
};
