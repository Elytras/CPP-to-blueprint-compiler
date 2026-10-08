// A cast over a constant. A consteval call's answer sits on the ConstantExpr under the cast, whatever its spelling
// ((int64)F(), static_cast, an integer widening or a uint64 read as int64): LowerArg looked through implicit casts only
// and went on to lower the call itself, refused as an unknown function. And a uint64 literal past INT64_MAX cast to a
// float is its unsigned value, where the int64-to-float conversion (low 32 bits) made -16 of 0xFFFFFFFFFFFFFFF0
// (test_bytecode.py const_cast).
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ConstCast");

consteval uint64 CcBig() { return 0xFFFFFFFFFFFFFFF0ULL; }
consteval uint32 CcWord() { return 4000000000u; }

class ConstCast : public AActor {
public:
  int64 Whole() { return (int64)CcBig(); }                  // the bits: -16
  int64 Word() { return (int64)CcWord(); }                  // 4000000000
  int64 Static() { return static_cast<int64>(CcWord()) + 1; }
  int64 Local() { int64 X = (int64)CcBig(); return X; }
  float Wide() { return (float)0xFFFFFFFFFFFFFFF0ULL; }    // 1.8446744e19
  float WideVar(float Y) { return Y + (float)0x8000000000000000ULL; }
};
