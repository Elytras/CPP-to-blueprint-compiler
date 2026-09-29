#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/StructRepl");

/* A struct member is not replicated on its own: only the replicating class's own CPF_Net properties enter its
   replication list, and a replicated struct variable is sent whole, every member. The marker on A has no effect, so
   the compiler says so. */
struct FReplHp {
  UE_STRUCT;
  UE_REPLICATED(int32, A);
  int32 B;
};

class StructRepl : public AActor {
  UE_REPLICATED(FReplHp, Hp);
};
