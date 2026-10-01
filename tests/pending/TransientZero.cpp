#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TransientZero");

/*
A native struct literal that gives a Transient member zero, or leaves a struct's one Transient member to `T()` / `{}`,
over a destination that holds something else: a member variable set before, and a local declared in a loop, which a
Blueprint does not make afresh each time round. execStructConst skips a Transient member (ScriptCore.cpp 3376-3405) and
steps the rest straight into the destination, so a literal would leave the member as it was; C++ sets it to zero.
*/
class TransientZero : public AActor {
public:
  FMaterialAttributesInput Kept;
  FTimerHandle Handle;

  int32 SetFour() {
    Kept = FMaterialAttributesInput(3, FName("In"), FName("Ex"), 4);
    return Kept.PropertyConnectedBitmask;
  }
  int32 SetZero() {
    Kept = FMaterialAttributesInput(3, FName("In"), FName("Ex"), 0);
    return Kept.PropertyConnectedBitmask;
  }
  int32 Loop(int32 M) {
    int32 R = 0;
    for (int32 I = 0; I < 2; ++I) {
      FMaterialAttributesInput S = FMaterialAttributesInput(3, FName("In"), FName("Ex"), 0);
      R = R * 10 + S.PropertyConnectedBitmask;
      S.PropertyConnectedBitmask = 7;
    }
    return R + M;
  }
  void ResetHandle() { Handle = FTimerHandle(); }
  void ClearHandle() { Handle = {}; }
};
