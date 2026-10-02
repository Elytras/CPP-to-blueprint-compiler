#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DefaultsBracesOther");

/*
FDboOther is another mod's struct, from the header the two share (UE_STRUCT_IN): no engine struct, and its header says
what each member starts as. `O = {.A = 7}` is A 7 and B its initializer 4, written as such (test_bytecode.py's
defaults_braces_other_mod).
*/
struct FDboOther {
  UE_STRUCT_IN("/Game/_ElytrasMods/DboOtherMod");
  int32 A = 3;
  int32 B = 4;
};

class DboParent : public AActor {
public:
  FDboOther O = {.A = 1, .B = 2};
};

class DefaultsBracesOther : public DboParent {
public:
  UE_DEFAULTS { O = {.A = 7}; }
};
