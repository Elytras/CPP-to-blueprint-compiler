#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FlowTest");

/* Control flow: break / continue in while and for, nested loops. */
class FlowTest : public AActor {
  int32 Total;

public:
  void ReceiveBeginPlay() {
    Total = SumSkipping(10, 3) + FirstOver(5) + Nested(4);
  }

  int32 SumSkipping(int32 Count, int32 Skip) {
    int32 Sum = 0;
    for (int32 I = 0; I < Count; I = I + 1) {
      if (I == Skip) continue;
      if (I > 7) break;
      Sum = Sum + I;
    }
    return Sum;
  }

  int32 FirstOver(int32 Limit) {
    int32 N = 0;
    while (true) {
      N = N + 1;
      if (N * N > Limit) break;
    }
    return N;
  }

  int32 Nested(int32 Size) {
    int32 Hits = 0;
    for (int32 Y = 0; Y < Size; Y = Y + 1) {
      for (int32 X = 0; X < Size; X = X + 1) {
        if (X == Y) continue;
        if (X > Y) break;
        Hits = Hits + 1;
      }
    }
    return Hits;
  }
  int32 Classify(int32 Code) {
    int32 Score = 0;
    switch (Code) {
    case 1:
      Score = 10;
      break;
    case 2:
    case 3:
      Score = 20;
    case 4:
      Score = Score + 5;
      break;
    default:
      Score = -1;
    }
    return Score;
  }

  /* Two dense runs with holes, each its own table, and two outliers compared on their own. */
  int32 Clusters(int32 Code) {
    switch (Code) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 3;
    case 5: return 6;
    case 9: return 10;
    case 50: return 500;
    case 100: return 1000;
    case 103: return 1003;
    case 104: return 1004;
    case 110: return 1010;
    case 7000: return 7;
    default: return -1;
    }
  }

  int32 NoDefault(int32 Code) {
    int32 R = 7;
    switch (Code) {
    case 0:
      R = 0;
      break;
    case 9:
      return 99;
    }
    return R;
  }

  int32 DefaultFirst(int32 Code) {
    int32 R = 0;
    switch (Code) {
    default:
      R = R + 100;
    case 5:
      R = R + 5;
      break;
    case 6:
      R = 6;
    }
    return R;
  }

  int32 SwitchInLoop(int32 Count) {
    int32 Sum = 0;
    for (int32 I = 0; I < Count; I = I + 1) {
      switch (I % 3) {
      case 0:
        continue;
      case 1:
        Sum = Sum + 1;
        break;
      default:
        Sum = Sum + 10;
      }
      Sum = Sum + 100;
      if (Sum > 1000) break;
    }
    return Sum;
  }

  uint8 ByteSwitch(uint8 Mode) {
    switch (Mode) {
    case 2: return 20;
    case 255: return 1;
    }
    return 0;
  }
  /* Mode promotes to 0..255: these three cases never match, and the first two are reached only by falling through. */
  int32 ByteSwitchStray(uint8 Mode) {
    int32 R = 0;
    switch (Mode) {
    case -1: R += 100;
    case 256: R += 10;
    case 7: R += 1; break;
    case 300: return 9;
    }
    return R;
  }
  int32 DenseHoles(int32 Code) {
    switch (Code) {
    case 10: return 1;
    case 11: return 2;
    case 13: return 4;
    case 14: return 5;
    default: return -9;
    }
  }

  int32 Negative(int32 Code) {
    int32 R = 50;
    switch (Code) {
    case -2: R = -20; break;
    case -1: R = -10; break;
    case 0: R = 0;
    }
    return R;
  }

  uint8 DenseByte(uint8 Mode) {
    uint8 R = 0;
    switch (Mode) {
    case 0: R = 3; break;
    case 1: R = 4; break;
    case 2: R = 5; break;
    }
    return R;
  }
  int32 Arith(int32 A, int32 B) { return A / B + (A % B) * 10 - -A + ~B; }

  /* Updates used as values: `X++` is X before the store, `++X` and `X op= Y` after it. */
  int32 PostInc(int32 X) { int32 Old = X++; return Old * 100 + X; }
  int32 PreInc(int32 X) { int32 New = ++X; return New * 100 + X; }
  int32 UpdateChain(int32 X) { int32 Y = 1; int32 Z = (Y += X) * 10; return Z + Y; }
  int32 OrAssign(int32 N) { bool Any = false; bool R = (Any |= N > 0); return (R ? 10 : 0) + (Any ? 1 : 0); }

  /* The destination is located once: Next() runs one time, and the read and the store share its index. */
  TArray<int32> Slots;
  int32 Cursor = 0;
  int32 NextSlot() { return Cursor++; }
  void BumpSlot(int32 By) { Slots[NextSlot()] += By; }

  /* Member templates have no UFunction per instantiation: each one a call names is inlined. */
  template <class T> requires(sizeof(T) <= 8) T Scaled(T V) { return V * T(3) + T(Cursor); }
  int32 TemplateMember(int32 X) { int64 Wide = Scaled<int64>(X); return Scaled(X) + int32(Wide); }

  /* A member of a returned struct: the call lands in a temp, StructMemberContext offsets into that. */
  float CallMember() { return GetTransform().Translation.Y; }

  /* `if constexpr` lowers only the branch clang kept: no jump over a constant is left. */
  template <class T> int32 WidthOf() { if constexpr (sizeof(T) == 8) return 8; else return 4; }
  int32 ConstexprPick() { return WidthOf<int64>() * 10 + WidthOf<int32>(); }

  /* Inline temps share properties once their spans end (CoalesceTemps). The condition's expansion is re-run at the
     top of every iteration, so it may share with the others; the two in one statement are live together. */
  inline int32 TwicePlus(int32 V) { int32 T = V * 2; return T + 1; }
  int32 CoalesceLoop(int32 N) {
    int32 Sum = 0;
    int32 Before = TwicePlus(N);
    for (int32 I = 0; I < TwicePlus(N); ++I) Sum += TwicePlus(I) + TwicePlus(I + 1);
    return Sum * 1000 + Before;
  }

  /* S and N live across SumTo's loop while TwicePlus's int temps come and go inside it: they must not share. */
  inline int32 SumTo(int32 N) { int32 S = 0; for (int32 I = 0; I < N; ++I) S += TwicePlus(I); return S; }
  int32 LoopInline(int32 N) { return SumTo(N) * 100 + SumTo(TwicePlus(N)); }

  /* A local declared without an initializer is empty / zero on every expansion, shared property or not. */
  inline int32 TwoOf(int32 V) { TArray<int32> Acc; int32 Seen; Seen += V; Acc.Add(V); Acc.Add(Seen); return Acc.Num() * 100 + Seen - V; }
  int32 FreshLocals(int32 N) {
    int32 Total = TwoOf(N) + TwoOf(N + 1);
    for (int32 I = 0; I < N; ++I) Total += TwoOf(I);
    return Total;
  }

  /* The same with nothing to share a property with: the loop alone re-runs the inline body. */
  int32 FreshInLoop(int32 N) { int32 Total = 0; for (int32 I = 0; I < N; ++I) Total += TwoOf(I); return Total; }

  bool SafeRatio(int32 X) { return X != 0 && 10 / X > 2; }

  bool EitherZero(int32 X, int32 Y) { return X == 0 || 100 / X == Y; }

  int32 Pick(int32 X) { return X > 0 ? X * 2 : X < -5 ? -1 : 7; }

  int32 Compound(int32 N) {
    int32 Acc = 1;
    for (int32 I = 0; I < N; ++I) {
      Acc += I;
      Acc *= 2;
      Acc -= 1;
      Acc %= 1000;
    }
    int32 Down = N;
    Down--;
    --Down;
    return Acc + Down;
  }

  float FloatStep(float F) {
    F += 0.5f;
    F++;
    return -F;
  }

  int32 WhileAnd(int32 Limit) {
    int32 I = 0;
    int32 Hits = 0;
    while (I < Limit && 100 / (Limit - I) > 1) {
      Hits += 1;
      I++;
    }
    return Hits * 1000 + I;
  }

  /* do/while: the body runs once even when the test fails, and `continue` lands on the test. */
  int32 DoOnce(int32 Limit) {
    int32 N = 0;
    do {
      N += 1;
    } while (N < Limit);
    return N;
  }

  int32 DoContinue(int32 Limit) {
    int32 I = 0;
    int32 Sum = 0;
    do {
      I += 1;
      if (I % 2 == 0) continue; // reaches the test: from the top, an even Limit would never end
      if (I > 9) break;
      Sum += I;
    } while (I < Limit);
    return Sum * 100 + I;
  }

  /* goto: backward (a loop), forward over code, and out of two loops at once. */
  int32 GotoLoop(int32 N) {
    int32 Sum = 0;
    int32 I = 0;
  again:
    if (I >= N) goto done;
    Sum += I;
    I += 1;
    goto again;
  done:
    return Sum;
  }

  int32 GotoOut(int32 Size, int32 Want) {
    int32 Found = -1;
    for (int32 Y = 0; Y < Size; Y++) {
      for (int32 X = 0; X < Size; X++) {
        if (X * Y == Want) {
          Found = Y * 100 + X;
          goto out;
        }
      }
    }
    Found = -2;
  out:
    return Found;
  }

  /* An inline function with a label, expanded twice into one caller: each expansion's label is its own. */
  inline int32 FirstSquareAbove(int32 Floor) {
    int32 N = 0;
  next:
    N += 1;
    if (N * N <= Floor) goto next;
    return N;
  }

  int32 GotoInlined(int32 A, int32 B) { return FirstSquareAbove(A) * 100 + FirstSquareAbove(B); }

  /* An inline body's goto loop, with a temp in it and a value live across it: the two keep their own slots. */
  inline int32 GotoLoopKeeps(int32 N) {
    int32 Base = N * 3;
    int32 Sum = 0;
    int32 I = 0;
  again:
    Sum += Base;
    int32 Twice = Sum * 2;
    Sum = Twice - Sum + Twice * 0 + I;
    I += 1;
    if (I < 3) goto again;
    return Sum;
  }

  int32 GotoInlinedLive(int32 N) { return GotoLoopKeeps(N); }

  /* A declaration a goto reaches again is constructed again, as one in a loop is. */
  int32 GotoRedeclares(int32 Rounds) {
    int32 Round = 0;
    int32 Total = 0;
  again:
    int32 Fresh;
    Fresh += 5;
    Total += Fresh;
    Round += 1;
    if (Round < Rounds) goto again;
    return Total;
  }

  /* An init-statement and a condition variable: both run once, before the plain statement. */
  int32 IfInit(int32 V) {
    int32 R = 0;
    if (int32 Twice = V * 2; Twice > 10)
      R = Twice;
    else
      R = -Twice;
    if (int32 Rest = V % 3) R += Rest * 1000;
    return R;
  }

  int32 SwitchInit(int32 V) {
    switch (int32 K = V + 1; K) {
    case 1:
      return 10;
    case 2:
      return 20 + K;
    default:
      break;
    }
    switch (int32 M = V % 4) {
    case 3:
      return 300 + M;
    default:
      return M;
    }
  }

  /* A while's condition variable is made again each time round. */
  int32 WhileVar(int32 Start) {
    int32 Steps = 0;
    while (int32 Left = Start - Steps) {
      Steps += 1;
      if (Left < 0) break;
    }
    return Steps;
  }

  /* `if (C) break;` where C comes to a constant: taken every time, or never. */
  static inline int32 StopBelow(int32 Max) {
    int32 N = 0;
    while (true) {
      N += 1;
      if (Max < 3) break;
      if (N >= 10) break;
    }
    return N;
  }
  int32 ConstBreak(int32 X) {
    const int32 Lim = 1;
    int32 N = 0;
    for (int32 I = 0; I < 5; ++I) {
      if (Lim > 3) break;
      if (2 < 1) continue;
      N += 1;
    }
    return StopBelow(1) * 100 + StopBelow(5) * 10 + N;
  }

  /* A defaulted argument, to a method and to an inline function: clang 18 writes the CXXDefaultArgExpr empty. */
  int32 Times(int32 X, int32 By = 3) { return X * By; }
  static inline int32 Offset(int32 X, int32 By = 7) { return X + By; }
  int32 UseDefault(int32 X) { return Times(X) + Times(X, 10) + Offset(X) * 1000; }

  /* UE_NAME_SWITCH: a comparison per case, case-insensitive as FName is. */
  /* FName converts to bool as Name != None. */
  int32 NameSet(FName N) { bool B = static_cast<bool>(N); if (N) return B ? 1 : 9; return !N ? 2 : 9; }

  int32 NameKind(FName Kind) {
    switch (UE_NAME_SWITCH(Kind)) {
    case UE_NAME_CASE("IntProperty"):
      return 1;
    case UE_NAME_CASE("FloatProperty"):
    case UE_NAME_CASE("DoubleProperty"):
      return 2;
    case UE_NAME_CASE("StructProperty"):
      return 3;
    default:
      return 0;
    }
  }
};
