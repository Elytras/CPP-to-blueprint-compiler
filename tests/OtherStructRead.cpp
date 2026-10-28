#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/OtherStructRead");

/*
FDboOther is another mod's struct, from the header the two share (UE_STRUCT_IN; DboOtherMod cooks it). Its members
are the UserDefinedStruct's minted names there, so a read of O.B and a braced value name those, not the C++ spelling
(test_bytecode.py's other_struct_read).
*/
struct FDboOther {
  UE_STRUCT_IN("/Game/_ElytrasMods/DboOtherMod");
  int32 A = 3;
  int32 B = 4;
};

class OtherStructRead : public AActor {
public:
  FDboOther O = {.A = 1, .B = 2};
  int32 Read() { return O.A * 10 + O.B; }
  int32 Built(int32 M) { FDboOther L = {.A = M, .B = 5}; return L.A * 10 + L.B; }
};
