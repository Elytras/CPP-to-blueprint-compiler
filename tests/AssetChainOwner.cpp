#include "UeApi/Types.h"

#include "UeApi/FSD.h"

#include "AssetChainShared.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetChainOwner");

/* The owner of AssetChainShared.h's class: its path is this mod's, so it cooks it here, warning that UAcsNodes's
   UE_DEFAULTS lies over the engine's value. */
class AssetChainOwner : public AActor {
public:
  int32 Count = 0;
};
