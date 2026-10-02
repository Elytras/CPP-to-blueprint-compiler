#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DboOtherMod");

/*
The other mod of DefaultsBracesOther: it cooks FDboOther, which DefaultsBracesOther takes from the header the two share
(UE_STRUCT_IN), so that one's imports of it resolve (invariants.py's imports_resolve finds the mods built beside it).
*/
struct FDboOther {
  UE_STRUCT;
  int32 A = 3;
  int32 B = 4;
};
