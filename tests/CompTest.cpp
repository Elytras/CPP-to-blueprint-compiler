#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompTest");

/*
Components, and the per-component defaults that go with them. Root is declared first, so it is the
actor's root and Mesh/Lamp attach to it. The UE_DEFAULTS values land on each component's own
archetype, delta'd against that component's CDO - which is why bVisible = false survives even
though it is "zero": a class variable would drop it, an archetype must not.
*/
class CompTest : public AActor {
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UStaticMeshComponent, Mesh);
  UE_COMPONENT(UPointLightComponent, Lamp);

  int32 Ticks = 0;

  UE_DEFAULTS {
    Mesh->bVisible = false;
    Lamp->Intensity = 1500.0f;
    /* The engine puts the root at the spawn transform and never applies its own transform, so
       AssetGen moves it onto the components attached to it: Mesh ends up at (10, 0, 0) scaled
       (2, 2, 3), its roll turned by the root's yaw to (0, 90, 90); Lamp's offset grows with the
       root's scale to (10, 0, 150), turns with its yaw to (0, 10, 150), and moves to (10, 10, 150). */
    Root->RelativeLocation = FVector(10.0f, 0.0f, 0.0f);
    Root->RelativeRotation = FRotator(0.0f, 90.0f, 0.0f);
    Root->RelativeScale3D = FVector(2.0f, 2.0f, 3.0f);
    Lamp->RelativeLocation = FVector(5.0f, 0.0f, 50.0f);
    Mesh->RelativeRotation = FRotator(0.0f, 0.0f, 90.0f);
    /* A constructor's argument lands on the member its parameter is named after, so the stub's
       parameter names are the truth: FColor takes R, G, B, A like the engine's C++ constructor,
       though its members are B, G, R, A in memory. A struct with a native Serialize, like FColor
       and FVector, writes raw bytes rather than nested tags. */
    Lamp->LightColor = FColor(255, 128, 0);     // orange; A defaults to 255
  }

public:
  void ReceiveBeginPlay() { Ticks = Ticks + 1; }
  // The same constructor in a body: the struct constant's members still go by parameter name.
  FColor Orange() { return FColor(255, 128, 0); }
};
