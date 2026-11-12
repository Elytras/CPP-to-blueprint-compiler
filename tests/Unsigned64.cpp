#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/Unsigned64");

/* A uint64 value in code: REFERENCE.md#bitwise-operators-and.uint64-x-4. Blueprint's int64 is signed and a uint64 is its bits, so a
   right shift and an order comparison take the unsigned forms. */
class Unsigned64 : public AActor {
public:
  int64 Shr(int64 X) { return int64(uint64(X) >> 4); }
  int64 Top(int64 X) { return int64(uint64(X) >> 63); }
  int64 Shr0(int64 X) { return int64(uint64(X) >> 0); }
  bool Less(int64 A, int64 B) { return uint64(A) < uint64(B); }
  bool Greater(int64 A, int64 B) { return uint64(A) > uint64(B); }
  bool LessEq(int64 A, int64 B) { return uint64(A) <= uint64(B); }
  bool GreaterEq(int64 A, int64 B) { return uint64(A) >= uint64(B); }
  bool Past(int64 A) { return uint64(A) > 5; } // a constant side
  bool Same(int64 A, int64 B) { return uint64(A) == uint64(B); }
  int64 Div(int64 A) { return int64(uint64(A) / 3); } // warns: a signed division
  int64 Signed(int64 X) { return X >> 4; } // unchanged: floors
};
