#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/StructLinkOrder");

/*
Whole-struct literals the VM reads in PropertyLink order (execStructConst, ScriptCore.cpp 3376-3405): a struct with
no super (FLinearColor: R, G, B, A) and one whose members all come from its super (FVector_NetQuantize : FVector, so
its PropertyLink is FVector's X, Y, Z). Both are written member for member in the order C++ declares them.
*/
class StructLinkOrder : public AActor {
public:
  FVector_NetQuantize Aim;
  FLinearColor Tint;

  void Fill() {
    Aim = FVector_NetQuantize(1.0f, 2.0f, 3.0f);
    Tint = FLinearColor(0.25f, 0.5f, 0.75f, 1.0f);
  }
  void Clear() {
    Aim = FVector_NetQuantize();
    Tint = FLinearColor();
  }
};
