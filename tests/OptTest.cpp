#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/OptTest");

/* DropUnusedPure: a pure call whose value is unused is dropped; one whose value is used, even twice, is kept. */
class OptTest : public AActor {
public:
  int32 Drop(int32 A, int32 B) {
    UKismetMathLibrary::Abs_Int(A); // dropped
    int32 X = A * B;
    return X + A * B;               // computed again
  }

  int32 Kept;
  /* DropUnusedLocals: Unused and Chain go; the self property and the impure call stay. */
  int32 Locals(int32 A) {
    int32 Unused = A * 3;
    int32 Chain = A + 7;
    int32 Copy = Chain;
    Kept = A - 1;
    int32 Rolled = UKismetMathLibrary::RandomInteger(A);
    return A;
  }

  /* A constant local nothing writes is its uses: no property, no store, the const at each read. One the body
     writes keeps its local. */
  int32 Consts(int32 A) {
    const int32 Three = 3;
    int32 Grows = 10;
    Grows += A;
    return A * Three + Three + Grows;
  }

  /* && / ||: a harmless right side is one BooleanAND / BooleanOR; a faulting one keeps the branch, and a guard
     `if` without an else becomes nested ifs. */
  int32 Logic(int32 X, int32 Y) {
    int32 R = 0;
    bool Both = X > 0 && Y / 2 > 0;
    bool Either = X == 0 || Y == 0;
    if (X != 0 && 10 / X > 2)
      R += 1;
    if (X != 0 && Y != 0)
      R += 2;
    if (Both)
      R += 10;
    if (Either)
      R += 100;
    return R;
  }

  /* UE_NO_OPTIMIZE: the unused pure call, the unread local and the branch all stay. */
  UE_NO_OPTIMIZE int32 Raw(int32 X, int32 Y) {
    UKismetMathLibrary::Multiply_IntInt(X, Y);
    int32 Unused = X + 3;
    return X != 0 && Y != 0 ? 1 : 0;
  }

#pragma clang optimize off
  /* The pragma is the same attribute; its implicit noinline is not UE_AUTHORITY_ONLY. */
  int32 RawPragma(int32 X) {
    UKismetMathLibrary::Abs_Int(X);
    return X;
  }
#pragma clang optimize on
};
