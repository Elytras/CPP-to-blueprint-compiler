#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DefaultsBracesTwice");

/*
Two UE_DEFAULTS statements on one member: in C++ the second is a whole new FHitResult, Distance 6 and the rest what the
engine's constructor sets (Time 1), whatever the first gave. The parent's Hit is fresh, so the second braces' left-out
members are left untagged; the first statement's Time 3 must not stay under them (test_bytecode.py's
defaults_braces_twice).
*/
class DbtParent : public AActor {
public:
  FHitResult Hit;
};

class DefaultsBracesTwice : public DbtParent {
public:
  UE_DEFAULTS {
    Hit = {.Time = 3.0f};
    Hit = {.Distance = 6.0f};
  }
};
