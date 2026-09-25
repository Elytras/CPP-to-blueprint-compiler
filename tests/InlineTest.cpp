#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/InlineTest");

/* inline functions: expanded at each call, never a UFunction. */
class InlineTest : public AActor {
  int32         Counter;
  int32         Calls;
  TArray<int32> Arr;

public:
  inline int32 Clamp(int32 V, int32 Lo, int32 Hi); // inline on the declaration, body below
  int32        Half(int32 V);                      // inline only on the definition below
  inline int32 Twice(int32 V) { return V * 2; }
  inline void  Bump(int32 &V, int32 By) {
    V += By;
    Counter += 1;
  }
  inline int32 FirstAbove(int32 Limit) {
    for (int32 I = 0; I < 100; I++) {
      if (I * I > Limit)
        return I;
    }
    return -1;
  }
  inline int32 Nest(int32 V) { return Twice(Twice(V)) + Clamp(V, 0, 10); }

  int32 UseClamp(int32 V) { return Clamp(V, -5, 5) + Twice(V) + Half(V); }

  int32 UseBump(int32 V) {
    int32 X = V;
    Bump(X, 3);
    Bump(X, V);
    return X * 100 + Counter;
  }

  int32 UseLoop(int32 L) {
    int32 I   = 0;
    int32 Sum = 0;
    while (I < 3) {
      Sum += FirstAbove(L + I);
      I++;
    }
    return Sum;
  }

  int32 UseNest(int32 V) { return Nest(V) + Nest(V + 1); }

  /* A call nested in an argument of the same function binds its parameters first: the outer call's own binding
     must win, and one it already made must survive. */
  int32 TwiceTwice(int32 V) { return Twice(Twice(V)) + Twice(Twice(3)); }
  int32 ClampInArg(int32 V) { return Clamp(4, Clamp(V, 0, 10), 8); }

  inline int32 Dec(int32 N) {
    N -= 1;
    return N;
  }

  int32 ConstThenVar(int32 V) { return Twice(3) + Twice(V) + Clamp(V, 0, 5) + Clamp(2, V, 9) + Dec(5) + Dec(V); }

  /* An argument read once, first thing, goes in place; one read after something else runs stays a local. */
  inline int32 Plus1(int32 A) { return A + 1; }
  inline int32 Late(int32 A) {
    Counter += 1;
    return A;
  }
  int32 InPlace(int32 V) { return Plus1(V * 3) + Plus1(V); }
  int32 KeptLocal(int32 V) { return Late(V * 3); }

  /* static works the same: expanded, no UFunction. */
  static inline int32 SDouble(int32 V) { return V * 2; }
  static int32        SPred(int32 V); // inline only on the definition below
  static int32        UseStatic(int32 V) { return SDouble(V) + SPred(V) + InlineTest::SDouble(3); }
  int32               StaticFromInst(int32 V) { return SDouble(V) + SPred(V); }

  int32 InCond(int32 V) {
    if (Twice(V) > 10 && Clamp(V, 0, 3) == 3)
      return 1;
    return 0;
  }

  /* A by-value parameter is the argument's value at the call, though the body changes what it was read from. */
  int32 LateMember() {
    int32 R = Late(Counter);
    return R * 100 + Counter;
  }

  /* A T& parameter bound to an element is that element, its index computed once, at the call. */
  int32 NextIdx() {
    Calls += 1;
    return 0;
  }
  int32 BumpElem(int32 By) {
    Arr.Add(10);
    Bump(Arr[NextIdx()], By);
    return Arr[0] * 100 + Calls * 10 + Counter;
  }

  /* do/while runs its body before the first test, when the test is an inline call too; continue goes to the test. */
  int32 DoInline(int32 N) {
    int32 I = 0;
    do {
      I++;
      if (I == 2)
        continue;
    } while (Twice(I) < N);
    return I;
  }

  /* A local an inline declares is made again at each call, also when the call is a loop's test. */
  inline int32 FreshCount(int32 V) {
    TArray<int32> A;
    A.Add(V);
    return A.Num();
  }
  int32 WhileFresh(int32 L) {
    int32 I = 0, T = 0;
    while (FreshCount(I) + I < L) {
      I++;
      T += 1;
    }
    return T;
  }
  int32 ForFresh(int32 L) {
    int32 T = 0;
    for (int32 I = 0; FreshCount(I) + I < L; I++)
      T += I;
    return T;
  }
  int32 DoFresh(int32 L) {
    int32 I = 0;
    do {
      I++;
    } while (FreshCount(I) + I < L);
    return I;
  }

  /* Same-name overloads: each call expands the one C++ picks, and one may call another. */
  inline int32 Pick(int32 V) { return V + 1; }
  inline int32 Pick(bool B) { return B ? 100 : 200; }
  inline int32 Pick(int32 A, int32 B); // body below
  int32        PickOverloads(int32 V, bool B) { return Pick(B) + Pick(V) * 1000 + Pick(V, 3) * 10; }
};

int32 InlineTest::Clamp(int32 V, int32 Lo, int32 Hi) {
  if (V < Lo)
    return Lo;
  if (V > Hi)
    return Hi;
  return V;
}

inline int32 InlineTest::Half(int32 V) { return V / 2; }

inline int32 InlineTest::SPred(int32 V) { return V - 1; }

int32 InlineTest::Pick(int32 A, int32 B) { return Pick(A) * B; }
