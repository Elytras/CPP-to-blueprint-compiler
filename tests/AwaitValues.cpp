/*
AwaitValues.cpp - UE_AWAIT on a dispatcher of two or more parameters.

Each value goes to the name a structured binding gives it, as the editor's async node gives each of a delegate's
outputs a pin of its own (K2Node_BaseAsyncTask.cpp 410-467). An await as a statement waits and keeps none of them.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AwaitValues");

class AwaitValues : public AActor {
  UE_DISPATCHER(OnScored, AActor *Who, int32 Points);
  UE_DISPATCHER(OnHit, AActor *Who, FVector Where, float Damage);
  AActor *Last;
  int32 Total;
  FVector At;
  float Hurt;
  int32 Woken;

public:
  void WaitScore() {
    auto [Who, Points] = UE_AWAIT(OnScored);
    Last = Who;
    Total += Points;
  }
  void WaitHitThenScore() {
    const auto &[Who, Where, Damage] = UE_AWAIT(OnHit);
    At = Where;
    Hurt += Damage;
    auto [Scorer, Points] = UE_AWAIT(OnScored);
    if (Scorer == Who) Total += Points * 10;
  }
  void WaitAny() {
    UE_AWAIT(OnHit);
    Woken += 1;
  }
};
