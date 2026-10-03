#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SetOperators");

/*
`A + B`, `A - B` and `A & B` on sets are Set_Union, Set_Difference and Set_Intersection into a temp. Diff runs in a
loop, so its temp is reused: each native empties Result first, so the second round sees no leftovers. Chain feeds
one operator's temp to the next.
*/
class SetOperators : public AActor {
public:
  TSet<int32> A;
  TSet<int32> B;
  TSet<int32> C;
  TSet<int32> Out;

  void Union() { Out = A + B; }
  void Inter() { Out = A & B; }

  int32 Diff() {
    int32 Total = 0;
    for (int32 Round = 0; Round < 2; ++Round) {
      Out = A - B;
      Total += Out.Num();
    }
    return Total;
  }

  void Chain() { Out = (A + B) - C; }
};
