#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DefaultsNestDeep");

/*
Braces in UE_DEFAULTS three deep over a fresh parent O: In's initializer ({.K = 4}) gives K and leaves Hit out, so the
parent's O.In.Hit is still the engine's fresh FHitResult, and the braces `{.Distance = 3.0f}` leave the rest of it as
that (untagged). K, left out of `.In = {...}`, takes its initializer 0 over the parent's 4 (test_bytecode.py's
defaults_nest_deep).
*/
struct FRvInner {
  UE_STRUCT;
  int32 K = 0;
  FHitResult Hit;
};

struct FRvOuter {
  UE_STRUCT;
  FRvInner In = {.K = 4};
};

class RvParent : public AActor {
public:
  FRvOuter O;
};

class DefaultsNestDeep : public RvParent {
public:
  UE_DEFAULTS {
    O = {.In = {.Hit = {.Distance = 3.0f}}};
  }
};
