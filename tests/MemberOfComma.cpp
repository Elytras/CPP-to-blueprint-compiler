#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/MemberOfComma");

/*
A member or an element of a comma updated or assigned in a loop condition: `((Bump(), T).A += 1)` is `Bump();` then
`T.A += 1` ([expr.comma]: the comma is its right side, the lvalue T; [expr.ref]: T.A an lvalue of it), every trip
([stmt.while]). `(Bump(), IL)[0]` is IL's element after Bump (an overloaded operator's operands sequenced as the
built-in's, [over.match.oper]/2, [expr.sub]: the array first). In `(Bump(), T).A = T.A + Count` C++17 reads the right
side first ([expr.ass]/1), Count before Bump moves it (test_bytecode.py's member_of_comma). The same as statements, an
if condition, an initializer and a reference argument: `(Bump(), T).A += G();` runs G before Bump ([expr.ass]/1), and
`IncRef(0, (Bump(), T).A += 1)` passes T.A itself, which IncRef then adds 3 to (member_of_comma_stmt).
*/
struct FMocPair {
  UE_STRUCT;
  int32 A = 1;
  int32 B = 2;
};

class MemberOfComma : public AActor {
public:
  int32 Count;
  FMocPair T;
  TArray<int32> IL;
  void Bump() { Count += 1; }

  int32 Member(int32 M) { Count = 0; T.A = M; while (((Bump(), T).A += 1) < 5) {} return T.A * 100 + Count; }
  int32 Element(int32 M) { Count = 0; IL = {M}; while (((Bump(), IL)[0] += 1) < 5) {} return IL[0] * 100 + Count; }
  int32 Assign(int32 M) { Count = 0; T.A = M; while (((Bump(), T).A = T.A + Count) < 9) {} return T.A * 100 + Count; }
  int32 Step(int32 M) { Count = 0; T.A = M; while (++(Bump(), T).A < 5) {} return T.A * 100 + Count; }

  int32 N;
  [[gnu::noinline]] int32 G() { return Count * 10; }
  [[gnu::noinline]] int32 IncRef(int32 X, int32& V) { V += 3; return V; }
  int32 CompoundStmt(int32 M) { Count = M; T.A = 1; (Bump(), T).A += G(); return T.A * 100 + Count; }
  int32 AssignStmt(int32 M) { Count = M; T.A = 1; (Bump(), T).A = G(); return T.A * 100 + Count; }
  int32 ElementStmt(int32 M) { Count = M; IL = {1}; (Bump(), IL)[0] += G(); return IL[0] * 100 + Count; }
  int32 RightReadStmt(int32 M) { Count = M; T.A = 1; (Bump(), T).A = T.A + Count; return T.A * 100 + Count; }
  int32 StepStmt(int32 M) { Count = M; T.A = 1; ++(Bump(), T).A; return T.A * 100 + Count; }
  int32 IfCond(int32 M) { Count = M; T.A = 1; N = 0; if (((Bump(), T).A += G()) > 0) N = 5; return N * 10000 + T.A * 100 + Count; }
  int32 Value(int32 M) { Count = M; T.A = 1; int32 X = ((Bump(), T).A += G()); return X * 100 + Count; }
  int32 RefArg(int32 M) { Count = M; T.A = M; IncRef(0, (Bump(), T).A += 1); return T.A * 100 + Count; }
};
