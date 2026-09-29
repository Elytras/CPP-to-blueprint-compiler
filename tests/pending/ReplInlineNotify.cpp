#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ReplInlineNotify");

/* An inline method is no UFunction: as a RepNotify it names a function the class does not have, so a client's
   RepLayout never finds it and the OnRep never runs there. Either it is refused, or it is cooked as a function. */
class ReplInlineNotify : public AActor {
  UE_REPLICATED_USING(int32, Ammo, OnRep_Ammo);
  int32 Seen;

public:
  inline void OnRep_Ammo() { Seen += 1; }
  void Fire() { Ammo = 3; }
};
