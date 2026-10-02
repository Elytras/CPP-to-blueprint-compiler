#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CommaPlaces");

/*
The comma operator, and a plain `=` used as a value, where no statement before the one holding it fits: a loop
condition, which reruns its left side on every trip, the right side of && / || and the arms of ?:, which may not run,
and an argument of a call on another object, whose object C++ evaluates first. The left side runs where C++ runs it,
and the value is the right side's, read right after it; passed to a reference parameter it is the right side's
variable itself (OtherRef, WhileRef: IncRef's `V += 3` lands in N).
*/
class CommaPlaces : public AActor {
public:
  int32 Count;
  int32 N;
  CommaPlaces* P;
  void Bump() { Count += 1; }
  [[gnu::noinline]] int32 Twice(int32 X) { return X * 2; }
  [[gnu::noinline]] int32 IncRef(int32 X, int32& V) { V += 3; return X * 1000 + V; }

  int32 While(int32 M) { Count = 0; N = M; while ((Bump(), N) < 5) N += 1; return N * 100 + Count; }
  int32 For(int32 M) { Count = 0; int32 I = M; for (; Bump(), I < 5; I += 1) {} return I * 100 + Count; }
  int32 And(int32 M) { Count = 0; bool R = M > 0 && (Bump(), M > 2); return (R ? 1000 : 0) + Count; }
  int32 Or(int32 M) { Count = 0; bool R = M > 0 || (Bump(), Bump(), M < -1); return (R ? 1000 : 0) + Count; }
  int32 Cond(int32 M) { Count = 0; int32 R = M > 0 ? (Bump(), M + Count) : (Bump(), Bump(), M - Count); return R * 100 + Count; }
  int32 Other(int32 M) { Count = 0; P = this; int32 R = P->Twice((Bump(), M + Count)); return R * 100 + Count; }
  int32 OtherRef(int32 M) { Count = 0; N = M; P = this; int32 R = P->IncRef(1, (Bump(), N)); return R * 100 + N * 10 + Count; }
  int32 WhileRef(int32 M) { Count = 0; N = M; int32 T = 0; while (IncRef(0, (Bump(), N)) < 20) T += 1; return T * 100 + N; }
  int32 WhileAssign(int32 M) { int32 V = 0; int32 I = 0; while ((V = I * 2) < M) I += 1; return I * 100 + V; }
};
