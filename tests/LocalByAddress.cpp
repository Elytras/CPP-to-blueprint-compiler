#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/LocalByAddress");

/*
A local read once, by a container function: the Kismet container thunks step the container with no result buffer and
read it where it lies (Stack.MostRecentPropertyAddress, execArray_Length and the rest), so the container must be a
variable - which is why `Two().Num()` is refused. A local holding a call's result stays that variable: an array, a set
and a map, read by Num, Find and Contains, and an inline function's parameter read so.
*/
class LocalByAddress : public AActor {
public:
  TArray<int32> Two() { return {1, 2}; }
  TSet<int32> TwoSet() { return {1, 2}; }
  TMap<int32, int32> TwoMap() { return {{1, 2}, {3, 4}}; }
  inline int32 Count(TArray<int32> A) { return A.Num(); }

  int32 ArrayNum(int32 M) { TArray<int32> R = Two(); return R.Num() + M; }
  int32 ArrayFind(int32 M) { TArray<int32> R = Two(); return R.Find(2) + M; }
  int32 ArrayContains(int32 M) { TArray<int32> R = Two(); return (R.Contains(2) ? 10 : 0) + M; }
  int32 SetNum(int32 M) { TSet<int32> R = TwoSet(); return R.Num() + M; }
  int32 MapNum(int32 M) { TMap<int32, int32> R = TwoMap(); return R.Num() + M; }
  int32 MapFind(int32 M) {
    int32 V = 0;
    TMap<int32, int32> R = TwoMap();
    return R.Find(3, V) ? V + M : -1;
  }
  int32 InlineParm(int32 M) { return Count(Two()) + M; }
};
