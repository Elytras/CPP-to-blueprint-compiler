#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CommaHoist");

/*
The comma operator inside an expression: its left side runs first, as a statement of its own, wherever nothing in the
statement runs before it, and its value is the right side's, read where C++ reads it. An initialiser, a TEnum<E>'s, an
argument (beside a constant, or beside a call, whose order against it C++ leaves open), a struct literal's first member,
a return value, an assignment's right side, a switch over a TEnum<E>, a reference argument, and a comma inside a comma;
beside a call that sets what it reads, by value or by reference, it runs whole before that call or after it. A class
default over constants is its right side.
*/
enum class EChPick : uint8 { Zero, One, Two, Three };
UE_ENUM(EChPick);

struct FChSlot {
  UE_STRUCT;
  int32 A = 1;
  int32 B = 2;
};

class CommaHoist : public AActor {
public:
  int32 Count;
  int32 D = (1, 4);

  void Bump() { Count += 1; }
  int32 Twice(int32 X) { return X * 2; }
  int32 Sum(int32 X, int32 Y) { return X * 10 + Y; }
  void Inc(int32 &X) { X += 5; }
  int32 SetCount(int32 X) { Count = 100; return X; }
  int32 AddTo(int32 X, int32 &Y) { Y += X; return Y; }

  int32 Brace(int32 M) { int32 N{(Bump(), M)}; return N + Count * 100; }
  int32 Enum(int32 M) { TEnum<EChPick> T = (Bump(), (EChPick)M); return (int32)(EChPick)T + Count * 100; }
  int32 Arg(int32 M) { return Twice((Bump(), M + Count)); }
  int32 Beside(int32 M) { return Sum(7, (Bump(), M + Count)); }
  int32 BesideCall(int32 M) { return Sum(Twice(M), (Bump(), Count)); }
  int32 Literal(int32 M) { FChSlot S = {(Bump(), M), Count}; return S.A * 10 + S.B; }
  int32 Ret(int32 M) { return (Bump(), M + Count); }
  int32 Assign(int32 M) { int32 N = 0; N = (Bump(), M); return N + Count * 100; }
  int32 Pick(int32 M) {
    TEnum<EChPick> T = (EChPick)M;
    switch (Bump(), T) {
    case EChPick::One: return 1000 + Count;
    default: return Count;
    }
  }
  int32 Ref(int32 M) { int32 N = M; Inc((Bump(), N)); return N * 100 + Count; }
  int32 Nested(int32 M) { return Twice(Sum(1, (Bump(), (Bump(), M)))); }
  /* Beside a call that sets what the comma reads: C++ runs the whole comma before the call or after it, never the call
     between its two sides. By value, the comma's Count is Bump's (before SetCount) or 101 (after); by reference the
     callee adds to Count itself, 100 + M or 101 + M. */
  int32 Interleave(int32 M) { return Sum(SetCount(M), (Bump(), Count)); }
  int32 RefBeside(int32 M) { return AddTo(SetCount(M), (Bump(), Count)); }
};
