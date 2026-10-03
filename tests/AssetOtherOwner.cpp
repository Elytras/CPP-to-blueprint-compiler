#include "UeApi/Types.h"

#include "UeApi/FSD.h"

#include "AssetOtherShared.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetOtherOwner");

/* The owner of AssetOtherShared.h's classes: their paths are this mod's, so it cooks them here. */
class AssetOtherOwner : public AActor {
public:
  int32 Count = 0;
};
