#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CommaTargets");

/*
The comma operator assigned to or updated: `(Bump(), N) += 1` is `Bump(); N += 1` ([expr.comma]: the comma is its
right side, the lvalue N). In a loop condition, which reruns it on every trip, on the right of &&, and as a statement
whose right side is a call. C++17 sequences an assignment's right side before its left ([expr.ass]/1), so G() reads
Count before the comma's Bump moves it (CompoundCall, StmtAssign, StmtCompound).
*/
class CommaTargets : public AActor {
public:
  int32 Count;
  int32 N;
  void Bump() { Count += 1; }
  [[gnu::noinline]] int32 G() { return Count; }

  int32 Compound(int32 M) { Count = 0; N = M; while (((Bump(), N) += 1) < 5) {} return N * 100 + Count; }
  int32 Pre(int32 M) { Count = 0; N = M; while (++(Bump(), N) < 5) {} return N * 100 + Count; }
  int32 Post(int32 M) { Count = 0; N = M; while ((Bump(), N)++ < 5) {} return N * 100 + Count; }
  int32 Assign(int32 M) { Count = 0; N = M; while (((Bump(), N) = N + 1) < 5) {} return N * 100 + Count; }
  int32 CompoundCall(int32 M) { Count = 0; N = M; while (((Bump(), N) += G()) < 9) {} return N * 100 + Count; }
  int32 AndRight(int32 M) { Count = 0; N = M; bool R = M > 0 && ((Bump(), N) += 1) > 2; return (R ? 1000 : 0) + N * 10 + Count; }
  int32 StmtAssign(int32 M) { Count = M; N = 0; (Bump(), N) = G(); return N * 100 + Count; }
  int32 StmtCompound(int32 M) { Count = M; N = 1; (Bump(), N) += G(); return N * 100 + Count; }
};
