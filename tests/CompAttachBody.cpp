#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompAttachBody");

/*
SetupAttachment in a function attaches at once and keeps the relative transform, as the engine's own leads to when the
component registers (AttachToComponent with KeepRelative, no welding: SceneComponent.cpp 667-683). On a component
already registered, as an actor's components are once it is constructed, the engine's own call does nothing but fail
an ensure (1750), so the compiler warns that it attached anyway.
*/
class CompAttachBody : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UPointLightComponent, Lamp);
  UE_COMPONENT(USceneComponent, Pivot);

  void ReceiveBeginPlay() { Pivot->SetupAttachment(Lamp); }
};
