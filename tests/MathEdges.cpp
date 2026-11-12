#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/MathEdges");

/* Where Blueprint's arithmetic leaves C++: a zero divisor gives 0 (REFERENCE.md#arithmetic.hits-shots) and an int64
   sum wraps at 64 bits (REFERENCE.md#arithmetic.x-1). */
class MathEdges : public AActor {
public:
  int32 Div(int32 A, int32 B) { return A / B; }
  int32 Mod(int32 A, int32 B) { return A % B; }
  int64 Div64(int64 A, int64 B) { return A / B; }
  float DivF(float A, float B) { return A / B; }
  int32 Guarded(int32 A, int32 B) { return B != 0 && A / B > 2 ? 1 : 0; }
  int64 Add64(int64 A, int64 B) { return A + B; }
  int64 Sub64(int64 A, int64 B) { return A - B; }
  int64 Mul64(int64 A, int64 B) { return A * B; }
};
