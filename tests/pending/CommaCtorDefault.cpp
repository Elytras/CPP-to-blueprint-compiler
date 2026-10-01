#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CommaCtorDefault");

/*
A comma among a parenthesised constructor's arguments, which C++ evaluates in no fixed order, as a call's (braces fix
theirs); and a comma beside a left-out argument, which stands for its parameter's default: a constant runs nothing.
*/
class CommaCtorDefault : public AActor {
public:
  float Count;
  int32 Bumps;

  float Get() { return Count + 3.f; }
  void Bump() { Count += 1.f; }
  void BumpI() { Bumps += 1; }
  void Inc(int32 &X, int32 By = 5) { X += By; }

  /* Get runs before the comma or after it: X is Count + 3 or Count + 4, Y is M. */
  float Ctor(float M) {
    FVector V = FVector(Get(), (Bump(), M), 0.f);
    return V.X * 100.f + V.Y;
  }
  /* The comma's right side is an element bound to Inc's reference: only the default 5 is beside it. */
  int32 DefaultRef(int32 M) {
    TArray<int32> L = {M};
    Inc((BumpI(), L[0]));
    return L[0];
  }
};
