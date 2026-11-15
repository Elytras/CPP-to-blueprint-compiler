#pragma once
/*
FMath.h - UE C++'s FMath spellings of the editor's math nodes.

Every function here is `inline`, so AssetGen copies its body into the caller, and the call is exactly the
UKismetMathLibrary node in the body: `FMath::Max(A, B)` is the Max (integer) node, as
`UKismetMathLibrary::Max(A, B)` is. Overloads pick the node by type, int32, int64 or float, the way UE's templates
pick the arithmetic; arguments of two different types are ambiguous, as `FMath::Max(1, 2.0f)` is a deduction error
in UE C++. The engine's FMath has many more functions. Add one the same way when a mod needs it.

The exact ones are also `constexpr`, with a build-time branch: `__builtin_is_constant_evaluated() ? <C++> : <node>`.
Called with constant arguments (`int32 Cap = FMath::Max(3, kSlots);` as a default, `FMath::Clamp(7, 0, 5)` in code),
clang runs the C++ while AssetGen builds the mod and only the answer is cooked. Called with a run-time value, the game
runs the node, and the C++ branch is not compiled at all. The C++ is what UE's FMath does, so both give one answer. The
rest have no such branch and always run in the game: the random ones, and those whose float result could differ in its
last bit from the engine's (Sqrt, Pow, the trigonometry, Lerp).

  int32 Longest = FMath::Max(Line.Len(), Longest);
  float Alpha   = FMath::Clamp(T, 0.0f, 1.0f);
  float Turn    = FMath::Fmod(Delta, 360.0f);         // Percent_FloatFloat: 0 for a divisor of 0
  int32 Roll    = FMath::RandRange(1, 6);             // RandomIntegerInRange: not pure, every call rolls
  int32 Slots   = FMath::Max(4, 6);                    // 6, worked out when the mod is built
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

namespace FMath {

#define UE_FMATH_BUILD(Cpp, Node) return __builtin_is_constant_evaluated() ? (Cpp) : (Node)

inline constexpr int32 Max(int32 A, int32 B) { UE_FMATH_BUILD(A >= B ? A : B, UKismetMathLibrary::Max(A, B)); }
inline constexpr int64 Max(int64 A, int64 B) { UE_FMATH_BUILD(A >= B ? A : B, UKismetMathLibrary::MaxInt64(A, B)); }
inline constexpr float Max(float A, float B) { UE_FMATH_BUILD(A >= B ? A : B, UKismetMathLibrary::FMax(A, B)); }
inline constexpr int32 Min(int32 A, int32 B) { UE_FMATH_BUILD(A <= B ? A : B, UKismetMathLibrary::Min(A, B)); }
inline constexpr int64 Min(int64 A, int64 B) { UE_FMATH_BUILD(A <= B ? A : B, UKismetMathLibrary::MinInt64(A, B)); }
inline constexpr float Min(float A, float B) { UE_FMATH_BUILD(A <= B ? A : B, UKismetMathLibrary::FMin(A, B)); }
inline constexpr int32 Clamp(int32 V, int32 Lo, int32 Hi) { UE_FMATH_BUILD(V < Lo ? Lo : V < Hi ? V : Hi, UKismetMathLibrary::Clamp(V, Lo, Hi)); }
inline constexpr int64 Clamp(int64 V, int64 Lo, int64 Hi) { UE_FMATH_BUILD(V < Lo ? Lo : V < Hi ? V : Hi, UKismetMathLibrary::ClampInt64(V, Lo, Hi)); }
inline constexpr float Clamp(float V, float Lo, float Hi) { UE_FMATH_BUILD(V < Lo ? Lo : V < Hi ? V : Hi, UKismetMathLibrary::FClamp(V, Lo, Hi)); }
inline constexpr int32 Abs(int32 A) { UE_FMATH_BUILD(A >= 0 ? A : -A, UKismetMathLibrary::Abs_Int(A)); }
inline constexpr int64 Abs(int64 A) { UE_FMATH_BUILD(A >= 0 ? A : -A, UKismetMathLibrary::Abs_Int64(A)); }
inline constexpr float Abs(float A) { UE_FMATH_BUILD(__builtin_fabsf(A), UKismetMathLibrary::Abs(A)); }
inline constexpr int32 Sign(int32 A) { UE_FMATH_BUILD(A > 0 ? 1 : A < 0 ? -1 : 0, UKismetMathLibrary::SignOfInteger(A)); }
inline constexpr float Sign(float A) { UE_FMATH_BUILD(A > 0.0f ? 1.0f : A < 0.0f ? -1.0f : 0.0f, UKismetMathLibrary::SignOfFloat(A)); }

inline constexpr float Square(float A) { UE_FMATH_BUILD(A * A, UKismetMathLibrary::Square(A)); }
inline float Sqrt(float A) { return UKismetMathLibrary::Sqrt(A); }
inline float Pow(float A, float B) { return UKismetMathLibrary::MultiplyMultiply_FloatFloat(A, B); }
inline float Exp(float A) { return UKismetMathLibrary::Exp(A); }
inline float Loge(float A) { return UKismetMathLibrary::Loge(A); }
inline float Fmod(float A, float B) { return UKismetMathLibrary::Percent_FloatFloat(A, B); }
inline float Frac(float A) { return UKismetMathLibrary::Fraction(A); }
// int32(A) is C++'s truncation; out of int32's range clang refuses it, and the call is left to the game.
inline constexpr int32 FloorToInt(float A) { UE_FMATH_BUILD(float(int32(A)) > A ? int32(A) - 1 : int32(A), UKismetMathLibrary::FFloor(A)); }
inline constexpr int32 CeilToInt(float A) { UE_FMATH_BUILD(float(int32(A)) < A ? int32(A) + 1 : int32(A), UKismetMathLibrary::FCeil(A)); }
inline constexpr int32 RoundToInt(float A) { UE_FMATH_BUILD(FloorToInt(A + 0.5f), UKismetMathLibrary::Round(A)); }
inline constexpr int32 TruncToInt(float A) { UE_FMATH_BUILD(int32(A), UKismetMathLibrary::FTrunc(A)); }

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
inline constexpr bool IsNearlyEqual(float A, float B, float Tolerance = 1e-8f) {
  UE_FMATH_BUILD(Abs(A - B) <= Tolerance, UKismetMathLibrary::NearlyEqual_FloatFloat(A, B, Tolerance));
}

// The random ones are no pure nodes: each call draws, even when its result is unused.
inline int32 RandRange(int32 Lo, int32 Hi) { return UKismetMathLibrary::RandomIntegerInRange(Lo, Hi); }
inline float FRandRange(float Lo, float Hi) { return UKismetMathLibrary::RandomFloatInRange(Lo, Hi); }
inline float FRand() { return UKismetMathLibrary::RandomFloat(); }
inline int32 RandHelper(int32 Max) { return UKismetMathLibrary::RandomInteger(Max); }

#undef UE_FMATH_BUILD

} // namespace FMath
