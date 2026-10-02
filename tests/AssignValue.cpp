#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssignValue");

struct FAvPair {
  UE_STRUCT;
  int32 A = 1;
  int32 B = 2;
};

/*
A struct's or an FString's `=` (operator=, a call) used as a value where no statement before the one holding it fits:
a loop condition, which reruns it on every trip, the right side of && and an arm of ?:, which may not run, and an
argument of a call on another object. Passed by value, its value is a copy of what it assigned, read right after the
assignment, as a number's `=` already is.
*/
class AssignValue : public AActor {
public:
  FAvPair S;
  FAvPair T;
  FString Str;
  AssignValue* P;
  [[gnu::noinline]] int32 UsePairV(FAvPair Q) { return Q.A * 10 + Q.B; }
  [[gnu::noinline]] int32 IsAb(FString Q) { return Q == FString("ab") ? 2 : 1; }

  int32 While(int32 M) { S.A = M; S.B = 2; T.A = 0; T.B = 0; int32 K = 0; while (UsePairV(T = S) < 100) { S.A += 3; K += 1; } return K * 1000 + T.A; }
  int32 And(int32 M) { S.A = M; S.B = 2; T.A = 0; T.B = 0; bool R = M > 0 && UsePairV(T = S) > 50; return (R ? 1000 : 0) + T.A; }
  int32 Cond(int32 M) { S.A = M; S.B = 2; T.A = 0; T.B = 0; int32 R = M > 0 ? UsePairV(T = S) : 5; return R * 100 + T.A; }
  int32 Other(int32 M) { S.A = M; S.B = 2; T.A = 0; T.B = 0; P = this; int32 R = P->UsePairV(T = S); return R * 100 + T.A; }
  int32 StrWhile(int32 M) { Str = FString(""); int32 K = 0; while (IsAb(Str = FString("ab")) < M) { K += 1; if (K > 3) break; } return K * 10 + (Str == FString("ab") ? 1 : 0); }
  int32 StrAnd(int32 M) { Str = FString(""); bool R = M > 0 && IsAb(Str = FString("ab")) == 2; return (R ? 10 : 0) + (Str == FString("ab") ? 1 : 0); }
};
