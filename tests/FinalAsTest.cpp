#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FinalAsTest");

/*
UE_FINAL_AS: UFinalAsBase is final through its one leaf, FinalAsTest, the class that is made. The base's own code is
compiled as if it were final - its calls on `this` expand, as FinalTest's do - and it is cooked Abstract.
*/
class UFinalAsBase : public AActor {
public:
  int32 Counter;

  int32 Bump(int32 By) {
    Counter += By;
    return Counter;
  }
  /* Both calls expand; the second sees the first's write. */
  int32 UseBump(int32 V) { return Bump(V) * 100 + Bump(V); }
};

UE_FINAL_AS(UFinalAsBase, FinalAsTest);
