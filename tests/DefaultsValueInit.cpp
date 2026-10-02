#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DefaultsValueInit");

/*
`{}` and `T()` of an engine struct as a default. The engine's constructor sets values no header says: FHitResult's
Time is 1 (FHitResult::Init, EngineTypes.h), FFindFloorResult's HitResult is FHitResult(1.f), FTransform's is the
identity. A class's own default, or a member of one, starts as that fresh value, so leaving the struct's members
untagged keeps the engine's values, however deep the struct sits. Over a parent's value the same forms are refused
(test_bytecode.py's defaults_value_init), but for an engine struct whose constructor sets nothing (FVector2D), whose
zeros are its value (DefaultsZero).
*/
struct FHitHolder {
  UE_STRUCT;
  FHitResult Hit;
  int32 N = 0;
};

struct FFloorHolder {
  UE_STRUCT;
  FFindFloorResult F;
  int32 N = 0;
};

class DefaultsValueInit : public AActor {
public:
  FHitHolder Own = {{}, 5};
  FHitHolder Own2 = {FHitResult(), 5};
  FHitResult Mine = {};
  FFloorHolder Floor = {FFindFloorResult(), 5};
  FFloorHolder Floor2 = {{}, 6};
  FFindFloorResult Bare = FFindFloorResult();
  FTransform Xf = FTransform();
};
