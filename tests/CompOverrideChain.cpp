#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompOverrideChain");

/*
Three levels of one component's defaults. In C++ each constructor runs after its parent's, so the grandchild's Lamp
has ChainMid's Intensity 250 and its own bVisible false. In the cooked classes that is the archetype chain: the
grandchild's override template must be archetyped on ChainMid's override (the nearest one, which GetArchetype finds by
name), so the values it does not restate come from there and not from ChainBase's template.
*/
class ChainBase : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UPointLightComponent, Lamp);

  UE_DEFAULTS { Lamp->Intensity = 1000.0f; }
};

class ChainMid : public ChainBase {
  UE_DEFAULTS { Lamp->Intensity = 250.0f; }
};

class CompOverrideChain : public ChainMid {
  UE_DEFAULTS { Lamp->bVisible = false; }
};
