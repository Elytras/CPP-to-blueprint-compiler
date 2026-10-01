#include "UeApi/Types.h"

#include "UeApi/Engine.h"

#include "FuncImportCall.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncImportUser");

/*
Calls into another mod's Blueprint classes, imported through FuncImportCall.h. A script function is no native whose
C++ thunk dispatches: EX_FinalFunction runs exactly the UFunction it names (ScriptCore.cpp 3005-3009). So a call to
Bump, which FicKid overrides, goes by name and reaches FicKid's on a FicKid, as the editor calls a function without
FUNC_Final; Fixed, final, and the static Twice are bound; and a parent call from an override here is the parent's.
*/
class FuncImportUser : public AActor {
public:
  int32 Via(FicBase *B, int32 V) { return B->Bump(V); }
  int32 ViaFixed(FicBase *B, int32 V) { return B->Fixed(V); }
  int32 ViaStatic(int32 V) { return FicBase::Twice(V); }
};

class FicUserKid : public FicBase {
public:
  int32 Bump(int32 V) override { return FicBase::Bump(V) + 1000; }
};
