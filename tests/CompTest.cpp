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
    /* A struct value is one argument per member, in DECLARATION order - which is also the order
       genueapi gives the stub constructor, so the header's parameter names are the truth (FColor
       is B, G, R, A). A struct with a native Serialize, like these two, writes raw bytes rather
       than nested tags. */
    Root->RelativeScale3D = FVector(2.0f, 2.0f, 3.0f);
    Lamp->LightColor = FColor(255, 128, 0, 255);
  }

public:
  void ReceiveBeginPlay() { Ticks = Ticks + 1; }
};
