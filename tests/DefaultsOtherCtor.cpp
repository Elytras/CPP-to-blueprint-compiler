#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DefaultsOtherCtor");

/*
FDboOther is another mod's struct, from the header the two share (UE_STRUCT_IN; DboOtherMod cooks it): no engine struct.
`FDboOther()` and `{}` are its value-initialisation, which that header spells member by member, A 3 and B 4, written
over the parent's (1, 2) and (5, 6) (test_bytecode.py's defaults_other_ctor).
*/
struct FDboOther {
  UE_STRUCT_IN("/Game/_ElytrasMods/DboOtherMod");
  int32 A = 3;
  int32 B = 4;
};

class DocParent : public AActor {
public:
  FDboOther O = {.A = 1, .B = 2};
  FDboOther P = {.A = 5, .B = 6};
};

class DefaultsOtherCtor : public DocParent {
public:
  UE_DEFAULTS {
    O = FDboOther();
    P = {};
  }
};
