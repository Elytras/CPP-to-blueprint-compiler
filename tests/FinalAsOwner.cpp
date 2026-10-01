#include "UeApi/Types.h"

#include "UeApi/Engine.h"

#include "FinalAsShared.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FinalAsOwner");

/*
The owner of FinalAsShared.h's classes: the base's path is this mod's, so it cooks FaShBase here, and FaShLeaf, the
one class made, beside it.
*/
int32 FaShBase::Bump(int32 V) {
  Counter += V;
  return Counter;
}

int32 FaShBase::Use(int32 V) { return Bump(V) * 10; }
