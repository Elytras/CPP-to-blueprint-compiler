#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PreloadIch");

/*
A subclass restating an inherited component's default: an InheritableComponentHandler record whose template
overrides the parent's. The class names the handler, and the loader must have serialized it before the class, which
finds component archetypes through it.
*/
class PreloadLampBase : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UPointLightComponent, Lamp);

  UE_DEFAULTS { Lamp->Intensity = 1000.0f; }
};

class PreloadIch : public PreloadLampBase {
  UE_DEFAULTS { Lamp->Intensity = 250.0f; }
};
