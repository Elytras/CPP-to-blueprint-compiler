#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ReplHiddenMap");

struct FReplBag {
  UE_STRUCT;
  int32 Total;
  TMap<int32, int32> Counts;
};

/* A replicated struct is replicated member by member, and a TMap member sends nothing (FMapProperty::NetSerializeItem
   only logs "Replicated TMaps are not supported."): the TMap one level down must be refused as a TMap variable is. */
class ReplHiddenMap : public AActor {
  UE_REPLICATED(FReplBag, Bag);
};
