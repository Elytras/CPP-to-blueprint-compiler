#pragma once
/*
FMath.h - UE C++'s FMath spellings of the editor's math nodes.

Every function here is `inline`, so AssetGen copies its body into the caller, and the call is exactly the
UKismetMathLibrary node in the body: `FMath::Max(A, B)` is the Max (integer) node, as
`UKismetMathLibrary::Max(A, B)` is. Overloads pick the node by type, int32, int64 or float, the way UE's templates
pick the arithmetic; arguments of two different types are ambiguous, as `FMath::Max(1, 2.0f)` is a deduction error
in UE C++. The engine's FMath has many more functions. Add one the same way when a mod needs it.

  int32 Longest = FMath::Max(Line.Len(), Longest);
  float Alpha   = FMath::Clamp(T, 0.0f, 1.0f);
  float Turn    = FMath::Fmod(Delta, 360.0f);         // Percent_FloatFloat: 0 for a divisor of 0
  int32 Roll    = FMath::RandRange(1, 6);             // RandomIntegerInRange: not pure, every call rolls
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

namespace FMath {

inline int32 Max(int32 A, int32 B) { return UKismetMathLibrary::Max(A, B); }
inline int64 Max(int64 A, int64 B) { return UKismetMathLibrary::MaxInt64(A, B); }
inline float Max(float A, float B) { return UKismetMathLibrary::FMax(A, B); }
inline int32 Min(int32 A, int32 B) { return UKismetMathLibrary::Min(A, B); }
inline int64 Min(int64 A, int64 B) { return UKismetMathLibrary::MinInt64(A, B); }
inline float Min(float A, float B) { return UKismetMathLibrary::FMin(A, B); }
inline int32 Clamp(int32 V, int32 Lo, int32 Hi) { return UKismetMathLibrary::Clamp(V, Lo, Hi); }
inline int64 Clamp(int64 V, int64 Lo, int64 Hi) { return UKismetMathLibrary::ClampInt64(V, Lo, Hi); }
inline float Clamp(float V, float Lo, float Hi) { return UKismetMathLibrary::FClamp(V, Lo, Hi); }
inline int32 Abs(int32 A) { return UKismetMathLibrary::Abs_Int(A); }
inline int64 Abs(int64 A) { return UKismetMathLibrary::Abs_Int64(A); }
inline float Abs(float A) { return UKismetMathLibrary::Abs(A); }
inline int32 Sign(int32 A) { return UKismetMathLibrary::SignOfInteger(A); }
inline float Sign(float A) { return UKismetMathLibrary::SignOfFloat(A); }

inline float Square(float A) { return UKismetMathLibrary::Square(A); }
inline float Sqrt(float A) { return UKismetMathLibrary::Sqrt(A); }
inline float Pow(float A, float B) { return UKismetMathLibrary::MultiplyMultiply_FloatFloat(A, B); }
inline float Exp(float A) { return UKismetMathLibrary::Exp(A); }
inline float Loge(float A) { return UKismetMathLibrary::Loge(A); }
inline float Fmod(float A, float B) { return UKismetMathLibrary::Percent_FloatFloat(A, B); }
inline float Frac(float A) { return UKismetMathLibrary::Fraction(A); }
inline int32 FloorToInt(float A) { return UKismetMathLibrary::FFloor(A); }
inline int32 CeilToInt(float A) { return UKismetMathLibrary::FCeil(A); }
inline int32 RoundToInt(float A) { return UKismetMathLibrary::Round(A); }
inline int32 TruncToInt(float A) { return UKismetMathLibrary::FTrunc(A); }

inline float Sin(float A) { return UKismetMathLibrary::Sin(A); }
inline float Cos(float A) { return UKismetMathLibrary::Cos(A); }
inline float Tan(float A) { return UKismetMathLibrary::Tan(A); }
inline float Asin(float A) { return UKismetMathLibrary::Asin(A); }
inline float Acos(float A) { return UKismetMathLibrary::Acos(A); }
inline float Atan(float A) { return UKismetMathLibrary::Atan(A); }
inline float Atan2(float Y, float X) { return UKismetMathLibrary::Atan2(Y, X); }
inline float DegreesToRadians(float A) { return UKismetMathLibrary::DegreesToRadians(A); }
inline float RadiansToDegrees(float A) { return UKismetMathLibrary::RadiansToDegrees(A); }

inline float Lerp(float A, float B, float Alpha) { return UKismetMathLibrary::Lerp(A, B, Alpha); }
inline bool IsNearlyEqual(float A, float B, float Tolerance = 1e-8f) {
  return UKismetMathLibrary::NearlyEqual_FloatFloat(A, B, Tolerance);
}

// The random ones are no pure nodes: each call draws, even when its result is unused.
inline int32 RandRange(int32 Lo, int32 Hi) { return UKismetMathLibrary::RandomIntegerInRange(Lo, Hi); }
inline float FRandRange(float Lo, float Hi) { return UKismetMathLibrary::RandomFloatInRange(Lo, Hi); }
inline float FRand() { return UKismetMathLibrary::RandomFloat(); }
inline int32 RandHelper(int32 Max) { return UKismetMathLibrary::RandomInteger(Max); }

} // namespace FMath
