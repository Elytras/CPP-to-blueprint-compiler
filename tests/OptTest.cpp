#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/OptTest");

/* DropUnusedPure: a pure call whose value is unused is dropped; one whose value is used is kept, and the same call
   again reads the local the first one's value went to (ReuseCalls). */
class OptTest : public AActor {
public:
  int32 Drop(int32 A, int32 B) {
    UKismetMathLibrary::Abs_Int(A); // dropped
    int32 X = A * B;
    return X + A * B;               // X: not computed again
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

  /* Locals whose spans do not overlap share a property: Sq and After, I and Copy, but not Total, which the loop reads
     round. A copy whose source is read again (Copy) keeps its own, and an array declared without a value starts empty
     in a shared property too (Second). */
  int32 Slots(int32 N) {
    int32 Total = 0;
    for (int32 I = 0; I < N; ++I) {
      int32 Sq = I * I;
      Total += Sq + Sq / 2;
    }
    int32 After = Total * 2;
    int32 Copy  = After;
    Copy += 1;
    return Copy * 1000 + After + Total;
  }
  int32 Arrays(int32 N) {
    TArray<int32> First;
    First.Add(N);
    First.Add(N + 1);
    int32         Had = First.Num();
    TArray<int32> Second;
    Second.Add(N);
    return Had * 10 + Second.Num();
  }

  /* An `if` over an inline bool function whose returns are all constants: each return goes straight to the branch it
     picks, out of the loop too, and a branch that is a lone `return R` is copied to it. */
  inline bool IsSquare(int32 V) {
    for (int32 I = 0; I <= V; ++I)
      if (I * I == V)
        return true;
    return false;
  }
  inline bool SquarePair(int32 V) {
    if (!IsSquare(V))
      return false;
    if (IsSquare(V + 1))
      return true;
    return false;
  }
  int32 Threads(int32 V) {
    int32 R = 0;
    if (IsSquare(V))
      R += 1;
    if (!IsSquare(V + 1))
      R += 2;
    else
      R += 4;
    if (SquarePair(V))
      R += 8;
    if (IsSquare(V * 2))
      return R;
    return R + 16;
  }

  /* ReuseCalls: a UE_STABLE call that an identical earlier one is sure to have run before is that call's value and out
     value. Loads counts the runs, which a real UE_STABLE getter would not let anyone see. */
  int32 Loads;
  UE_STABLE int32 Load(int32 Slot, int32 &Twice) {
    Loads += 1;
    Twice = Slot * 2;
    return Slot * 10;
  }
  int32 Reuse(int32 A, int32 B) {
    int32 First = 0, Again = 0, Other = 0;
    int32 X = Load(A, First);
    int32 Y = Load(A, Again);       // the first's: Again is First
    int32 Z = Load(B, Other);       // another argument: runs
    if (B > 0) {
      int32 Inner = 0;
      int32 V = Load(A, Inner);     // the first's, under an if
      Z += V + Inner;
    }
    Kept = B;                       // a store to a member reads no local: Load(A) stays known
    int32 W = Load(A, Other);
    Tick();                         // a call that may act: what follows runs again
    int32 U = Load(A, Other);
    return (X + Y + Z + W + U + First + Again + Other) * 100 + Loads;
  }
  /* Two `if`s on one value take the same branch: the second's Load is the first's. */
  inline int32 Guarded(int32 A) {
    if (A <= 0)
      return -1;
    int32 T = 0;
    int32 V = Load(A, T);
    return V + T;
  }
  int32 Correlated(int32 A) {
    int32 P = Guarded(A);
    int32 Q = Guarded(A);
    return P * 1000 + Q + Loads * 1000000;
  }
  /* Only a call the first is sure to have run before: B decides whether the first ran at all. */
  int32 NotHoisted(int32 A, int32 B) {
    int32 T = 0, X = 0;
    if (B > 0)
      X = Load(A, T);
    int32 Y = Load(A, T);
    return X + Y + T + Loads * 1000000;
  }

  /* MergeFind: `Map[Key]` where Contains(Key) surely ran is that Contains, made a Map_Find: one map call. Through an
     `||` too, whose temp says Contains ran when it is false. */
  TMap<FName, int32> Scores;
  int32 Lookup(FName Key) {
    if (!Scores.Contains(Key))
      return -1;
    return Scores[Key];
  }
  int32 LookupUnless(bool Skip, FName Key) {
    if (Skip || !Scores.Contains(Key))
      return -1;
    return Scores[Key] * 10;
  }

  /* UE_NO_OPTIMIZE: the unused pure call, the unread local and the branch all stay. */
  UE_NO_OPTIMIZE int32 Raw(int32 X, int32 Y) {
    UKismetMathLibrary::Multiply_IntInt(X, Y);
    int32 Unused = X + 3;
    return X != 0 && Y != 0 ? 1 : 0;
  }

#pragma clang optimize off
  /* The pragma is the same attribute, with an implicit noinline beside it that sets no flag. */
  int32 RawPragma(int32 X) {
    UKismetMathLibrary::Abs_Int(X);
    return X;
  }
#pragma clang optimize on
};
