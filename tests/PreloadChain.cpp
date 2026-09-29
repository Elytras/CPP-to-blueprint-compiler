#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PreloadChain");

/*
Three Blueprint classes, each the parent of the next, one package each. Each has one CDO, archetyped on its
parent's CDO, and the event-driven loader must serialize the parent class and the parent CDO before the class, and
the class before its CDO is created: the child CDO is built from the parent's, defaults and all.
*/
class PreloadRoot : public AActor {
public:
  int32 Level = 1;
  float Weight = 2.0f;
};

class PreloadMid : public PreloadRoot {
  UE_DEFAULTS { Level = 2; }
};

class PreloadChain : public PreloadMid {
  UE_DEFAULTS {
    Level = 3;
    Weight = 4.0f;
  }
};
