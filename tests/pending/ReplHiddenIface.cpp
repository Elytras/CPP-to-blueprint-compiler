#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ReplHiddenIface");

class IReplMarker {
public:
  UE_INTERFACE;
  void Mark();
};

/* An interface variable does not replicate: FInterfaceProperty::NetSerializeItem returns false and writes nothing. */
class ReplHiddenIface : public AActor {
  UE_REPLICATED(TScriptInterface<IReplMarker>, Target);
};
