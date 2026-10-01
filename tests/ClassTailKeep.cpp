#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ClassTailKeep");

/*
What a class takes from its parent without saying so: the ScriptInherit class flags, the class it must be
created inside (ClassWithin) and the ini its config members read from (ClassConfigName). These parents are
the ones AssetGen already follows - an actor, a component, a scene component, a plain object, a function
library, and a mod class under a mod class - so each tail must match what the engine's own compiler would
write, and stay that way.
*/
class ClassTailKeep : public AActor {
public:
  int32 Count;
  UE_REPLICATED(int32, Shared);

  int32 Next() {
    Count = Count + 1;
    return Count;
  }
};

class TailKid : public ClassTailKeep {
public:
  int32 Extra;
  int32 Both() { return Next() + Extra; }
};

class TailPart : public UActorComponent {
public:
  float Rate;
};

class TailScene : public USceneComponent {};

class TailObject : public UObject {
public:
  FString Label;
};

class TailLib : public UBlueprintFunctionLibrary {
public:
  static int32 Twice(int32 V) { return V * 2; }
};
