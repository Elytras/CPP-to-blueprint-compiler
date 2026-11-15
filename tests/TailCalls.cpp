#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TailCalls");

/* A self-call in tail position is a jump back to the top: REFERENCE.md#tail-calls. Each function here recurses once
   per unit of N, so a deep N only returns if no call is left. */

/* A free function, a static of the source's library. */
int32 FreeSum(int32 N, int32 Acc) {
  if (N <= 0)
    return Acc;
  return FreeSum(N - 1, Acc + N);
}

/* An inline function calls itself only this way: the loop is all there is of it. */
inline int32 InlineSum(int32 N, int32 Acc) {
  if (N <= 0)
    return Acc;
  return InlineSum(N - 1, Acc + N);
}

/* Every argument is read before any parameter is written: the two swap. */
inline int32 Gcd(int32 A, int32 B) { return B == 0 ? A : Gcd(B, A % B); }
inline int32 GcdIf(int32 A, int32 B) {
  if (B == 0)
    return A;
  return GcdIf(B, A % B);
}

class TailCalls final : public AActor {
public:
  int32 Calls;
  TArray<int32> Seen;

  /* A method of a final class: no subclass has another Sum. */
  int32 Sum(int32 N, int32 Acc) {
    if (N <= 0)
      return Acc;
    return Sum(N - 1, Acc + N);
  }

  /* Defined out of the class, and the call under an else and a block. */
  int32 Count(int32 N, int32 Acc);

  static int32 StaticSum(int32 N, int32 Acc) {
    if (N <= 0)
      return Acc;
    return StaticSum(N - 1, Acc + N);
  }

  /* A void function's last statement, and one right before a bare return. */
  void Fill(int32 N) {
    if (N <= 0)
      return;
    Seen.Add(N);
    Fill(N - 1);
  }
  void FillEarly(int32 N) {
    if (N > 0) {
      Seen.Add(N);
      FillEarly(N - 2);
      return;
    }
    Seen.Add(0);
  }

  /* A local the body declares starts fresh each round, as each call's did; an array too. */
  int32 Fresh(int32 N, int32 Acc) {
    int32 Step;
    TArray<int32> Mine;
    if (N <= 0)
      return Acc;
    Step += N;
    Mine.Add(Step);
    return Fresh(N - 1, Acc + Step * Mine.Num());
  }

  /* A const reference that gets another value each round is read through a local: the caller's variable, which the
     parameter is, keeps its value. One handed on as it came stays the caller's. */
  int32 Longest(const FString& S, const TArray<int32>& Limits, int32 N) {
    if (N <= 0)
      return S.Len() + Limits.Num();
    return Longest(S + "ab", Limits, N - 1);
  }
  int32 Pad;
  FString Kept;
  int32 CallLongest(int32 N) {
    Kept = "x";
    TArray<int32> Limits = {1, 2, 3};
    const int32 Got = Longest(Kept, Limits, N);
    return Got * 100 + Kept.Len();
  }

  /* A reference handed on as it came is the same variable every round. */
  void Collect(TArray<int32>& Out, int32 N) {
    if (N <= 0)
      return;
    Out.Add(N);
    Collect(Out, N - 1);
  }
  int32 CallCollect(int32 N) {
    TArray<int32> Got;
    Collect(Got, N);
    return Got.Num() * 1000 + (Got.Num() > 0 ? Got[Got.Num() - 1] : 0);
  }

  /* Under a switch, whose `continue` is the loop's. */
  int32 Steps(int32 N, int32 Acc) {
    switch (N % 3) {
    case 0:
      if (N == 0)
        return Acc;
      return Steps(N - 3, Acc + 1);
    case 1:
      return Steps(N - 1, Acc + 10);
    default:
      break;
    }
    return Steps(N - 2, Acc + 100);
  }

  /* Not the last thing the function does: still a call, and its result is used. */
  int32 Factorial(int32 N) {
    if (N <= 1)
      return 1;
    return N * Factorial(N - 1);
  }

  /* A reference bound to another variable each round cannot be re-bound: still a call. */
  void Halve(int32& V, int32 N) {
    if (N <= 0)
      return;
    V /= 2;
    int32 Other = V + 1;
    Calls += 1;
    Halve(Other, N - 1);
  }
  int32 CallHalve(int32 V, int32 N) {
    Halve(V, N);
    return V;
  }

  /* Under a loop of the body: still a call. */
  int32 Down(int32 N) {
    for (int32 I = 0; I < N; ++I)
      if (I == 2)
        return Down(N - 1);
    return N;
  }

  int32 UseInline(int32 N) { return InlineSum(N, 0) + Gcd(N, 36) * 1000000 + GcdIf(N, 48) * 10000000; }
  int32 UseFree(int32 N) { return FreeSum(N, 0); }
  int32 UseStatic(int32 N) { return StaticSum(N, 0); }
};

int32 TailCalls::Count(int32 N, int32 Acc) {
  if (N > 0) {
    { return Count(N - 1, Acc + 2); }
  } else
    return Acc;
}

/* A class a subclass may follow: a call by name runs that subclass's Sum, so it stays a call. */
class TailOpen : public AActor {
public:
  int32 Sum(int32 N, int32 Acc) {
    if (N <= 0)
      return Acc;
    return Sum(N - 1, Acc + N);
  }
};
