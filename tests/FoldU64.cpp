// Constant folding in unsigned 64-bit arithmetic. FoldConst keeps a value as int64 bits, which / % >> read as signed: a
// uint64 past INT64_MAX divides, takes the remainder and shifts as a negative number. C++ says the unsigned result
// (test_bytecode.py fold_u64). A consteval call's answer, which clang prints as a decimal, saturated in strtoll.
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FoldU64");

consteval uint64 FuBig() { return 0xFFFFFFFFFFFFFFF0ULL; }

class FoldU64 : public AActor {
public:
  static constexpr uint64 kBig = 0xFFFFFFFFFFFFFFF1ULL;
  int64 Div() { return (int64)(0xFFFFFFFFFFFFFFF0ULL / 16ULL); }   // 0x0FFFFFFFFFFFFFFF
  int64 Mod() { return (int64)(0xFFFFFFFFFFFFFFFFULL % 10ULL); }   // 5
  int64 Shr() { return (int64)(0xFFFFFFFFFFFFFFF0ULL >> 4); }      // 0x0FFFFFFFFFFFFFFF
  int64 ConstVar() { return (int64)(kBig / 16ULL); }               // 0x0FFFFFFFFFFFFFFF
  int64 Eval() { return (int64)(FuBig() >> 4); }                   // 0x0FFFFFFFFFFFFFFF
};
