#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/LocalCtorFlags");

/*
Locals the VM must construct. It memzeroes a function's frame and runs InitializeValue on its locals from
FirstPropertyToInit on (ScriptCore.cpp 909-916, 2005-2011), which UFunction::Link finds only when the function is
FUNC_HasDefaults (Class.cpp 5653-5660); the editor sets that flag whenever a local is not zero-constructible
(KismetCompiler.cpp 2328-2335). Without it: BlankTime reads 0 where FHitResult() says 1, ScaleX 0 where the identity
transform says 1, and TextString hands Conv_TextToString an FText with a null TextData. A TMap local is not
zero-constructible either (its sparse array's free list starts at -1).
*/
class LocalCtorFlags : public AActor {
public:
  float BlankTime() {
    FHitResult H = FHitResult();
    return H.Time;
  }
  float ScaleX() {
    FTransform T;
    return T.Scale3D.X;
  }
  FString TextString() {
    FText T;
    return UKismetTextLibrary::Conv_TextToString(T);
  }
  int32 MapCount() {
    TMap<int32, int32> M;
    M.Add(1, 2);
    return M.Num();
  }
};
