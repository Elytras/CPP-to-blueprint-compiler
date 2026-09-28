#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FinalTest");

/*
`final`, on a class or on a method: no subclass has a version of its own, so a call reaches that one function, bound
at build time instead of found by name. A call whose body is known here is expanded in place and the function kept
for everything else that calls it: a bound call on `this`, a call to a parent's, a static of this mod. Recursion,
noinline, UE_NO_OPTIMIZE and a body that resumes later stay calls.
*/
class FinalTest final : public AActor {
public:
  int32      Counter;
  FinalTest *Peer;

  int32 Bump(int32 By) {
    Counter += By;
    return Counter;
  }
  /* Both calls expand; the second sees the first's write. */
  int32 UseBump(int32 V) { return Bump(V) * 100 + Bump(V); }

  /* The function is never expanded into itself: the recursive call stays a call. */
  int32 Fact(int32 V) { return V <= 1 ? 1 : V * Fact(V - 1); }

  /* IsOdd expands into IsEven once, and its call back to IsEven stays a call. */
  bool  IsEven(int32 N) { return N == 0 ? true : IsOdd(N - 1); }
  bool  IsOdd(int32 N) { return N == 0 ? false : IsEven(N - 1); }
  int32 Parity(int32 N) { return IsEven(N) ? 2 : IsOdd(N) ? 1 : 0; }

  /* The reference parameters write the caller's variables through the expansion. */
  void Around(int32 A, int32 &Lo, int32 &Hi) {
    Lo = A - 1;
    Hi = A + 1;
  }
  int32 UseAround(int32 A) {
    int32 L = 0, H = 0;
    Around(A, L, H);
    return L * 1000 + H;
  }

  /* noinline and UE_NO_OPTIMIZE keep the call. */
  [[gnu::noinline]] int32 Kept(int32 V) { return V + 1; }
  UE_NO_OPTIMIZE int32    KeptRaw(int32 V) { return V * 2; }
  int32                   UseKept(int32 V) { return Kept(V) * 10 + KeptRaw(V); }

  /* So does a latent call in the body: expanded, it would make CallsWait resume later, which returns a value. */
  void Wait() {
    UKismetSystemLibrary::Delay(1.0f);
    Counter += 1;
  }
  int32 CallsWait() {
    Wait();
    return Counter;
  }

  static int32 Twice(int32 V) { return V * 2; }
  int32        UseTwice(int32 V) { return Twice(V) + Twice(V + 1); }

  /* On another object the call is bound but not expanded: the body's `this` would have to be Peer. */
  int32 PeerBump(int32 By) { return Peer ? Peer->Bump(By) : -1; }
};

class FinalBase : public AActor {
public:
  int32 N;
  int32 Hook() { return 1; }
  /* Written here, it runs in FinalKid on a FinalKid: its Hook() is FinalKid's, as a call by name would find. */
  inline int32 HookTwice() { return Hook() * 2; }

  /* `final` on the method: no subclass has a Step of its own, so Loop's calls expand though FinalKid exists. */
  virtual int32 Step() final {
    N += 1;
    return N;
  }
  int32 Loop(int32 K) {
    int32 S = 0;
    for (int32 I = 0; I < K; ++I)
      S += Step();
    return S;
  }
};

class FinalKid final : public FinalBase {
public:
  int32 Hook() { return 5; }
  int32 Use() { return HookTwice() * 10 + FinalBase::Hook(); }
};

/* A static's body that leaves out GetPlayerPawn's world context passes the static's own WorldContextObject, as in
   its own function: expanded, that is what the caller passed for it. One without that parameter gets the caller's. */
class FinalWorld : public AActor {
public:
  static APawn *PawnOf(UObject *WorldContextObject = nullptr) { return UGameplayStatics::GetPlayerPawn(0); }
  APawn        *Ask(UObject *Other) { return PawnOf(Other); }
  APawn        *Mine() { return PawnOf(); }
  static APawn *NoContext() { return UGameplayStatics::GetPlayerPawn(0); }
  APawn        *ViaNoContext() { return NoContext(); }
};
