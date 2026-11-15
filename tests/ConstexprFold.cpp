/*
ConstexprFold.cpp - a constexpr function called with constant arguments is worked out when the mod is built, and only
its answer is cooked: as a default, in a constructor, in code. Called with a run-time argument, it is an ordinary call
of the function's Blueprint body. FMath.h's exact functions do the same through their build-time branch.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

#include "../include/FMath.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ConstexprFold");

constexpr int32 Twice(int32 X) { return X * 2; }
constexpr int32 Tri(int32 N) { // a loop: clang runs it, AssetGen sees only 55
  int32 S = 0;
  for (int32 I = 1; I <= N; ++I) S += I;
  return S;
}
consteval int32 Fnv(const char *S) {
  uint32 H = 2166136261u;
  while (*S) H = (H ^ uint32(*S++)) * 16777619u;
  return int32(H & 0x7FFFFFFF);
}
constexpr float Half(float X) { return X * 0.5f; }
inline constexpr int32 SqI(int32 X) { return X * X; } // SqI(3) is 9; SqI(V) is expanded
constexpr int32 Len(const char *S) {
  int32 N = 0;
  while (S[N]) ++N;
  return N;
}
namespace Calc {
constexpr int32 Scale(int32 X, int32 By = 3) { return X * By; }
} // namespace Calc
template <class T, int32 K> constexpr T Times(T X) { return X * K; }
template <class T> struct TBox {
  static constexpr T Twice(T X) { return X * 2; }
};
constexpr int32 kSeven = Twice(3) + 1; // a constant made by a constexpr call

class ConstexprFold : public AActor {
public:
  static constexpr int32 Plus(int32 A, int32 B) { return A + B; }
  template <class T> static constexpr T Neg(T X) { return -X; }
  int32 Doubled = Twice(21);                     // 42
  int32 Summed = Tri(10);                        // 55
  int32 Hashed = Fnv("types");                   // a consteval call straight in a default
  float Halved = Half(5.0f);                     // 2.5
  int32 Scaled = Calc::Scale(4);                 // 12: the default argument too
  int32 Named = Len("seven");                    // 5
  int32 Seven = kSeven;                          // 7
  int32 Summed2 = Plus(2, kSeven);               // 9: a static method
  int64 Tripled = Times<int64, 3>(5);            // 15: a template's instantiation, spelled with its arguments
  float Negated = Neg(1.5f);                     // -1.5: a static member template
  int32 Boxed = TBox<int32>::Twice(4);           // 8: a class template's static method
  int32 Capped = FMath::Clamp(70, 0, 50);        // 50: FMath's build-time branch
  int32 Floored = FMath::FloorToInt(-2.5f);      // -3
  bool bNear = FMath::IsNearlyEqual(1.0f, 1.0f); // true, through the default tolerance
  int32 Nine = SqI(3);                           // 9
  int32 Built;
  ConstexprFold() { Built = Twice(Tri(3)); } // 12, in the constructor

  int32 UseConst() { return Twice(21) + Tri(4); }                       // 52, no call left
  int32 UseRun(int32 V) { return Twice(V) + 1; }                       // the library's Twice runs
  constexpr int32 SqM(int32 X) const { return X * X; }                  // a call on this: always a call
  int32 UseSq(int32 V) { return SqI(V) + SqM(3); }                       // V * V and the SqM call
  int32 UseMath(int32 V) { return FMath::Max(3, 9) + FMath::Max(V, 0); } // 9, and the Max node for V
};
