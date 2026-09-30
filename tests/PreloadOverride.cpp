#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PreloadOverride");

/*
A Blueprint parent and a subclass overriding two of its functions, one of them calling the parent's own. Each
override's SuperStruct is the parent class's function, an import from PreloadBase's package, and the loader fetches
it with bCheckSerialized while it serializes the override (AsyncLoading.cpp 3149-3162).
*/
class PreloadBase : public AActor {
public:
  int32 Count;
  int32 Step(int32 By) {
    Count = Count + By;
    return Count;
  }
  int32 Score() { return 1; }
};

class PreloadOverride : public PreloadBase {
public:
  int32 Step(int32 By) { return PreloadBase::Step(By * 2); }
  int32 Score() { return 2; }
};
