#include "UeApi/Types.h"

#include "UeApi/Engine.h"

#include "FinalAsShared.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FinalAsUser");

/*
A mod that includes FinalAsShared.h and cooks neither of its classes: a cast to the leaf names the owner's FaShLeaf,
which every leaf object is, and a call through one reaches the owner's functions.
*/
class FinalAsUser : public AActor {
public:
  bool IsLeaf(AActor *A) { return Cast<FaShLeaf>(A) != nullptr; }
  int32 Via(FaShLeaf *L, int32 V) { return L->Use(V); }
};
