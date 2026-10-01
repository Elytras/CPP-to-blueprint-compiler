/*
UberChain.cpp - a parent and a child class that both wait.

A class whose methods make latent calls gets an ubergraph and a persistent frame of its own, and one object of the
child class runs both: each latent call resumes, found by name on the object, in the ubergraph of the class that made
it, on that class's frame, with the locals it had. Two classes of one leaf name in two namespaces - two folders, one
the other's parent - keep their ubergraphs apart the same way.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/UberChain");

class UberChain : public AActor {
public:
  int32 Stage;
  TArray<int32> Log;

  /* L is computed before the wait and read after it, so it lives in UberChain's frame. */
  void WaitA() {
    int32 L = Stage + 1;
    UKismetSystemLibrary::Delay(0.1f);
    Log.Add(L);
  }
};

class UberChainKid : public UberChain {
public:
  /* A local of the same name, in UberChainKid's frame. */
  void WaitB() {
    int32 L = Stage + 2;
    UKismetSystemLibrary::Delay(0.2f);
    Log.Add(L);
  }

  /* An override that runs the parent's version, which waits in UberChain's ubergraph, then waits itself: the method,
     and so its frame slot for L, has the same name in both classes' frames. */
  void WaitA() {
    UberChain::WaitA();
    int32 L = Stage + 3;
    UKismetSystemLibrary::Delay(0.3f);
    Log.Add(L);
  }
};

namespace UberA {
class Gun : public AActor {
public:
  int32 Hits;
  /* By is copied into the frame before the wait. */
  void Fire(int32 By) {
    UKismetSystemLibrary::Delay(0.1f);
    Hits += By;
  }
};
} // namespace UberA

namespace UberB {
class Gun : public UberA::Gun {
public:
  int32 Kicks;
  void Kick(int32 By) {
    UKismetSystemLibrary::Delay(0.1f);
    Kicks += By;
  }
};
} // namespace UberB
