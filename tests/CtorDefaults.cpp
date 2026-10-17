#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CtorDefaults");

/*
A default constructor is read as the class's defaults, as in-class initializers and UE_DEFAULTS are: the engine never
runs it, a Blueprint object being a copy of its class's default object. A member initializer or an assignment to one of
the class's own variables is that variable's initializer, the last one winning; any other statement joins UE_DEFAULTS,
after its own. test_bytecode.py compiles these classes written the other way too, and checks both cook the same.
*/
struct FCtorStats {
  UE_STRUCT;
  int32 HP = 1;
  float Speed;
  FCtorStats() : Speed(2.5f) { HP = 50; }
};

class CtorDefaults : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UPointLightComponent, Lamp);
  int32 Charges = 3;
  int32 Plain;
  float Range = 100.0f;
  FString Label;
  TSet<int32> Marks;
  FCtorStats Stats;

  CtorDefaults() : Plain(7), Label("lit"), Marks({1, 2, 3}) {
    Charges = 5;
    InitialLifeSpan = 2.0f;
    Lamp->Intensity = 250.0f;
    Lamp->SetupAttachment(Root);
    Range = 10.0f;
    Range = 20.0f;
  }
  UE_DEFAULTS { InitialLifeSpan = 9.0f; }
};

class CtorDefaultsKid : public CtorDefaults {
public:
  CtorDefaultsKid() {
    Marks = {2, 3, 4};
    Charges = 0;
    Lamp->Intensity = 100.0f;
  }
};
