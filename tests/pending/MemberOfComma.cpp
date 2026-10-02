#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/MemberOfComma");

/*
A member or an element of a comma updated or assigned in a loop condition: `((Bump(), T).A += 1)` is `Bump();` then
`T.A += 1` ([expr.comma]: the comma is its right side, the lvalue T; [expr.ref]: T.A an lvalue of it), every trip
([stmt.while]). `(Bump(), IL)[0]` is IL's element after Bump (an overloaded operator's operands sequenced as the
built-in's, [over.match.oper]/2, [expr.sub]: the array first). In `(Bump(), T).A = T.A + Count` C++17 reads the right
side first ([expr.ass]/1), Count before Bump moves it (test_bytecode.py's member_of_comma).
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
};
