#include "UeApi/Types.h"

#include "../include/FMath.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FMathTest");

/* FMath.h: UE C++'s FMath spellings, each expanded to the math node of its argument type. */
class FMathTest : public AActor {
public:
  int32 MaxInt(int32 A, int32 B) { return FMath::Max(A, B); }
  float MaxFloat(float A, float B) { return FMath::Max(A, B); }
  int32 ClampAbs(int32 V) { return FMath::Clamp(V, 0, 10) + FMath::Abs(V - 3); }
  float FloatMix(float A, float B) { return FMath::Min(FMath::Abs(A), B) + FMath::Clamp(A, -1.0f, 1.0f) + FMath::Fmod(A, 2.0f); }
};
