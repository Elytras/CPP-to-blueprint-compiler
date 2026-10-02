#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DefaultsOwnNest");

/*
New braces for a struct member with an initializer of its own (`Hit = {.Time = 0.5f}`), in a value that starts as the
struct's default instance (UUserDefinedStruct::InitializeStruct copies it into every new value): a class's own default
(H, another mod's struct; L, a UE_STRUCT), a struct's member initializer (FDonOuter::In) and a UE_DEFAULTS statement over
a parent declared with braces (P = {.N = 2}). Each new Hit gives Time 1 and Distance 5; the members both braces leave
out hold the engine's FHitResult values in the start value and in the new one, so they stay unwritten. N, left out,
takes its initializer 0 ([dcl.init.aggr]/5), over P's 2 (test_bytecode.py's defaults_own_nest).
*/
struct FDonOther {
  UE_STRUCT_IN("/Game/_ElytrasMods/DboOtherMod");
  FHitResult Hit = {.Time = 0.5f};
  int32 N = 0;
};

struct FDonLocal {
  UE_STRUCT;
  FHitResult Hit = {.Time = 0.5f};
  int32 N = 0;
};

struct FDonOuter {
  UE_STRUCT;
  FDonLocal In = {.Hit = {.Distance = 5.0f, .Time = 1.0f}};
};

class DonParent : public AActor {
public:
  FDonLocal P = {.N = 2};
};

class DefaultsOwnNest : public DonParent {
public:
  FDonOther H = {.Hit = {.Distance = 5.0f, .Time = 1.0f}};
  FDonLocal L = {.Hit = {.Distance = 5.0f, .Time = 1.0f}};
  FDonOuter O;

  UE_DEFAULTS {
    P = {.Hit = {.Distance = 5.0f, .Time = 1.0f}};
  }
};
