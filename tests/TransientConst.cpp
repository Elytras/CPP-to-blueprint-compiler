#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TransientConst");

/*
A native struct literal whose Transient member is given a value. execStructConst skips a Transient member
(ScriptCore.cpp 3376-3405), so a literal of constants cannot set it; a computed member already makes the literal the
editor's Make Struct, which sets every member. A non-zero constant for the Transient member is a value the literal
would drop, so it makes the Make Struct too: whether the value counts does not hang on another member. A zero there
stays a literal, which the member of a fresh value already holds.
*/
class TransientConst : public AActor {
public:
  int32 AllConst(int32 M) {
    FMaterialAttributesInput S = FMaterialAttributesInput(3, FName("In"), FName("Ex"), 4);
    return S.PropertyConnectedBitmask * 10 + S.OutputIndex + M;
  }
  int32 ZeroConst(int32 M) {
    FMaterialAttributesInput S = FMaterialAttributesInput(3, FName("In"), FName("Ex"), 0);
    return S.PropertyConnectedBitmask * 10 + S.OutputIndex + M;
  }
};
