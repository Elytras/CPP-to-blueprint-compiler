#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssignedCommaStmt");

/*
An assignment onto a comma used as a value in a statement: `int32 X = ((Bump(), N) = G());` is `(Bump(), N) = G();`
then `X = N` ([expr.ass]: its value is its left operand, the lvalue N, [expr.comma]); C++17 sequences G() before the
comma ([expr.ass]/1), so it reads Count before Bump moves it. The same with a struct member, `(Bump(), T).A = G()`,
and an update of an assignment to N (test_bytecode.py's assigned_comma_stmt).
*/
struct FAcsPair {
  UE_STRUCT;
  int32 A = 1;
  int32 B = 2;
};

class AssignedCommaStmt : public AActor {
public:
  int32 Count;
  int32 N;
  FAcsPair T;
  void Bump() { Count += 1; }
  [[gnu::noinline]] int32 G() { return Count * 10; }

  int32 Value(int32 M) { Count = M; N = 0; int32 X = ((Bump(), N) = G()); return X * 100 + Count; }
  int32 Member(int32 M) { Count = M; T.A = 0; int32 X = ((Bump(), T).A = G()); return X * 100 + Count; }
  int32 Update(int32 M) { Count = M; N = 0; ((Bump(), N) = G()) += 1; return N * 100 + Count; }
};
