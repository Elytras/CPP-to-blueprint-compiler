#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompTwoSet");

/*
Two defaults on one inherited component: they are one override of that component, so the class carries one
InheritableComponentHandler record for it and one template holding both values. A second record with the same key
would be shadowed by the first (FindRecord takes the first match), losing whatever only it holds.

TwoSetBase is also the parent a rebuild must not orphan: its nodes' guids depend on the class and the component's name
only, so a parent recompiled with a component added before Lamp, and a method more, still matches the record this
class ships with.
*/
class TwoSetBase : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UPointLightComponent, Lamp);

  UE_DEFAULTS { Lamp->Intensity = 1000.0f; }

  int32 Probe() { return 1; }
};

class CompTwoSet : public TwoSetBase {
  UE_DEFAULTS {
    Lamp->Intensity = 300.0f;
    Lamp->bVisible = false;
  }
};
