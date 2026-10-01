#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DefaultsValueInit");

/*
`{}` and `T()` of an engine struct as a default. FHitResult's header declares no constructor, but the engine's sets
Time to 1 (FHitResult::Init, EngineTypes.h), as a function body's `FHitResult()` keeps. A class's own default, or a
member of one, starts as that fresh value, so leaving the struct's members untagged keeps the engine's values.
FFindFloorResult declares a constructor, which AssetGen takes for all zeros, as a function body's FFindFloorResult()
does: written over the parent's value, every member is zero, its FHitResult's Time included.
*/
struct FHitHolder {
  UE_STRUCT;
  FHitResult Hit;
  int32 N = 0;
};

class DefaultsValueInitParent : public AActor {
public:
  FFindFloorResult Floor = {true, true, true, 2.0f, 3.0f, {}};
};

class DefaultsValueInit : public DefaultsValueInitParent {
public:
  FHitHolder Own = {{}, 5};
  FHitHolder Own2 = {FHitResult(), 5};
  FHitResult Mine = {};

  UE_DEFAULTS {
    Floor = FFindFloorResult();
  }
};
