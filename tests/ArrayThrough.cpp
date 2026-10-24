#include "UeApi/Types.h"

#include "../include/Intrin.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ArrayThrough");

/* A TArray through a pointer, in place: a reference bound to it once, indexed, written, counted and copied; two at once;
   one per loop round; an element read straight off `(*P)[I]`; a TArray of structs. Each runs offline over runscript's
   MEM, the array's header and its elements laid out there as the engine lays them. */

class ArrayThrough : public AActor {
public:
  int32 Sum(const TArray<int32> *P) {
    const TArray<int32> &R = *P;
    int32 S = 0;
    for (int32 I = 0; I < R.Num(); ++I)
      S = S + R[I];
    return S;
  }

  void Bump(TArray<int32> *P, int32 I) {
    TArray<int32> &R = *P;
    R[I] = R[I] + 1;
  }

  TArray<int32> CopyOf(const TArray<int32> *P) { return *P; }

  float FirstFloat(const TArray<float> *P) { return (*P)[0]; }

  // Two references at once, so each keeps its own view.
  int32 FirstsAndCounts(const TArray<int32> *A, const TArray<int32> *B) {
    const TArray<int32> &RA = *A;
    const TArray<int32> &RB = *B;
    return RA[0] * 1000 + RB[0] * 100 + RA.Num() * 10 + RB.Num();
  }

  // A reference declared in a loop is bound again each round, to the array the round names.
  int32 LastOfEach(const TArray<int32> *A, const TArray<int32> *B) {
    int32 S = 0;
    for (int32 K = 0; K < 2; ++K) {
      const TArray<int32> *Q = K == 0 ? A : B;
      const TArray<int32> &R = *Q;
      S = S * 100 + R[R.Num() - 1];
    }
    return S;
  }

  float SumZ(const TArray<FVector> *P) {
    const TArray<FVector> &R = *P;
    float S = 0.0f;
    for (int32 I = 0; I < R.Num(); ++I)
      S = S + R[I].Z;
    return S;
  }
};
