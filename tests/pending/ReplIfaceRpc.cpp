#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ReplIfaceRpc");

class IReplPinger {
public:
  UE_INTERFACE;
  void Ping();
};

/* An RPC's interface parameter never reaches the other machine: FInterfaceProperty::NetSerializeItem returns false
   and writes nothing, so the server's S sees a null Target. It is refused, as a TMap parameter is. */
class ReplIfaceRpc : public AActor {
  int32 N;

public:
  UE_SERVER void S(TScriptInterface<IReplPinger> Target) { N = 1; }
};
