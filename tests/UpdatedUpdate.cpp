#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/UpdatedUpdate");

/*
An update, or a struct's operator=, that is itself updated or assigned: `(N += G2()) += 1` is `N += G2();` then
`N += 1` ([expr.ass]: a compound assignment's value is its left operand, an lvalue), with its right side, 1, first
([expr.ass]/1, C++17); `++(N += 1)` likewise ([expr.pre.incr]); `(N += G2()) = N + 5` reads N + 5 before the update
runs; `((T = S).A += 1)` updates T's A after the copy (the implicit operator= returns *this). In a loop condition each
runs every trip ([stmt.while]) (test_bytecode.py's updated_update).
*/
struct FUuPair {
  UE_STRUCT;
  int32 A = 1;
  int32 B = 2;
};

class UpdatedUpdate : public AActor {
public:
  int32 Count;
  int32 N;
  FUuPair T;
  FUuPair S;
  [[gnu::noinline]] int32 G2() { Count += 1; return Count; }

  int32 Loop(int32 M) { Count = 0; N = M; int32 K = 0; while (((N += G2()) += 1) < 20) K += 1; return K * 10000 + N * 100 + Count; }
  int32 Step(int32 M) { N = M; int32 K = 0; while (++(N += 1) < 10) K += 1; return K * 100 + N; }
  int32 Assigned(int32 M) { Count = 0; N = M; (N += G2()) = N + 5; return N * 100 + Count; }
  int32 StructLoop(int32 M) { S.A = M; T.A = 0; int32 K = 0; while (((T = S).A += 1) < 5) { S.A += 1; K += 1; } return K * 10000 + T.A * 100 + S.A; }
};
