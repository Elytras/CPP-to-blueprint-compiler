/* FinalAsShared.h: a UE_FINAL_AS base two mods share, the macro beside the class as REFERENCE advises.
   FinalAsOwner.cpp cooks both classes; FinalAsUser.cpp only includes the header and must import them. */
#pragma once
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

class FaShBase : public AActor {
public:
  UE_CLASS("/Game/_ElytrasMods/FinalAsOwner/FaShBase", "FaShBase_C");
  int32 Counter;
  int32 Bump(int32 V);
  int32 Use(int32 V);
};

UE_FINAL_AS(FaShBase, FaShLeaf);
