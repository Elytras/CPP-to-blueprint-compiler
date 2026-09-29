#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ReplHiddenRpc");

struct FReplSack {
  UE_STRUCT;
  int32 Total;
  TMap<int32, int32> Counts;
};

/* An RPC's parameters are laid out member by member too (FRepLayout::InitFromFunction): the TMap inside the struct
   never reaches the server, as a TMap parameter would not, which is refused. */
class ReplHiddenRpc : public AActor {
  int32 Got;

public:
  UE_SERVER void Send(FReplSack Sack) { Got = Sack.Total; }
};
