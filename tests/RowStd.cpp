#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include <algorithm>

UE_MOD_PACKAGE("/Game/_ElytrasMods/RowStd");

/*
REFERENCE.md rows that once said "refused" and are now built: std::min from <algorithm> compiles into a function of the
source's library class, and a whole TArray is copied out of memory through a pointer (test_bytecode.py, row_std).
*/
class RowStd : public AActor {
public:
  int32 MinOf(int32 A, int32 B) { return std::min(A, B); }
  int32 SumThrough(TArray<int32> *Pa) {
    TArray<int32> A = *Pa;
    int32 S = 0;
    for (int32 X : A) S += X;
    return S * 100 + A.Num();
  }
  TMap<int32, FTransform> Placed;
  int32 BumpAndLeave() {
    for (auto &[K, V] : Placed) {
      V.Scale3D.X = 2.0f;
      goto done;
    }
  done:
    return 0;
  }
};
