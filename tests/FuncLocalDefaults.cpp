#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncLocalDefaults");

/*
Locals whose zeroed memory is not their constructed value. The VM memzeroes a function's frame and constructs its
locals (InitializeValue from FirstPropertyToInit on) only when the function has FUNC_HasDefaults (Class.cpp:5653-5660,
ScriptCore.cpp:2005-2011); the Kismet compiler sets it for the first local without CPF_ZeroConstructor. Without it an
FText local is a null TextData (reading it crashes), an FTransform is all zero rather than identity, an FHitResult has
Time 0 rather than 1, and a struct with member defaults has none of them.
*/
struct FTallied {
  UE_STRUCT;
  int32 Count = 5;
  float Scale = 2.0f;
};

class FuncLocalDefaults : public AActor {
public:
  FString EmptyText() {
    FText T;
    return FString(T);
  }
  float IdentityScale() {
    FTransform T;
    return T.Scale3D.X + T.Rotation.W;
  }
  int32 StructDefault() {
    FTallied U;
    return U.Count;
  }
  float HitTime() {
    FHitResult H;
    return H.Time;
  }
};
