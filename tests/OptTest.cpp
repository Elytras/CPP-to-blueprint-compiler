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

  /* Bool `|` / `&` / `^`: one BooleanOR / BooleanAND / BooleanXOR, which runs both sides as C++ does; a constant side
     decides it or drops out. */
  int32 Calls;
  bool Bump(bool V) {
    Calls += 1;
    return V;
  }
  int32 Bools(bool A, bool B) {
    bool Any = A;
    Any |= B;
    bool All = A & B;
    bool One = A ^ B;
    bool Stuck = A | true;
    bool Same = B & true;
    bool Both = Bump(A) | Bump(B);
    bool Kept = Bump(B) | true; // true, but Bump still runs
    return int32(Any) + 2 * int32(All) + 4 * int32(One) + 8 * int32(Stuck) + 16 * int32(Same) + 32 * int32(Both) +
           64 * int32(Kept) + 128 * Calls;
  }

  /* ForwardSingleUse moves a value to its one read past stores to other locals (U past D), but never a call past a
     store that reads what the call changes: B is the Calls that the first Tick left. */
  int32 Tick() {
    Calls += 1;
    return Calls;
  }
  int32 Forward(int32 A) {
    int32 T = Tick();
    int32 B = Calls;
    int32 C = A * 2;
    int32 U = Tick();
    int32 D = C + 1;
    return T * 1000000 + B * 10000 + U * 100 + D;
  }

  /* An inline's parameter is the caller's local itself only while nothing can change the local: a call that does not
     name it (Tick) cannot, a reference parameter bound to it can (AddTo's Acc), and so can another argument that writes
     it (Bumped). C++ leaves the order of Order's arguments open, so Order only has to agree with OrderRaw. */
  inline int32 Pair(int32 A, int32 B) { return A * 1000 + B; }
  inline int32 AddTo(int32 V, int32 &Acc) {
    Acc += V;
    return V * 1000 + Acc;
  }
  int32 Bumped(int32 &V) {
    V += 1;
    return V;
  }
  int32 InPlace(int32 X) {
    int32 L = X;
    int32 P = Pair(L, Tick());
    int32 Q = AddTo(L, L);
    return P + Q * 3;
  }
  int32 Order(int32 X) {
    int32 L = X;
    return Pair(L, Bumped(L));
  }
  UE_NO_OPTIMIZE int32 OrderRaw(int32 X) {
    int32 L = X;
    return Pair(L, Bumped(L));
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
