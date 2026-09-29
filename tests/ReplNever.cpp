#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ReplNever");

/* UE_REPLICATED_IF(..., Never) is COND_Never, 15 in UE 4.27 (CoreNetTypes.h 26): RepLayout skips a COND_Never
   property at setup. 14 is no ELifetimeCondition. SkipReplay, the one before it, is 13. */
class ReplNever : public AActor {
  UE_REPLICATED_IF(int32, Hidden, Never);
  UE_REPLICATED_IF(int32, NoReplay, SkipReplay);
  UE_REPLICATED(int32, Shown);
};
