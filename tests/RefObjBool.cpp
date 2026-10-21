#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/RefObjBool");

/* An object passed as `T *const &` tests like a plain pointer: `if (A)` and `!A` are Is Valid. */
class RefObjBool : public AActor {
public:
  int32 Tested(AActor *const &A) { return A ? 1 : 2; }
  int32 Negated(AActor *const &A) {
    if (!A)
      return 3;
    return 4;
  }
};
