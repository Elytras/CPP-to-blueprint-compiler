#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/RpcTwoWays");

/* An RPC goes one way: the sender tests Multicast, then Client, then Server, while the receiver accepts by its own
   flags. Two direction markers on one method are refused, or cooked to one direction. (UE_SERVER UE_CLIENT together
   never gets this far: clang refuses [[gnu::hot]] with [[gnu::cold]].) */
class RpcTwoWays : public AActor {
  int32 N;

public:
  UE_MULTICAST UE_SERVER void Also() { N = 2; }
  UE_MULTICAST UE_CLIENT void Wide() { N = 3; }
};
