#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ReplIfaceNest");

class IReplNestMark {
public:
  UE_INTERFACE;
  void Mark();
};

struct FReplNest {
  UE_STRUCT;
  int32 N;
  TArray<TScriptInterface<IReplNestMark>> Marks;
};

/* A replicated struct is sent member by member and an array element by element (RepLayout InitFromStructProperty /
   InitFromProperty_r), down to FInterfaceProperty::NetSerializeItem, which writes nothing: the interfaces two levels
   down never reach a client, as a replicated TScriptInterface would not (ReplHiddenIface). Refused like it. */
class ReplIfaceNest : public AActor {
  UE_REPLICATED(FReplNest, Nest);
};
