#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CommaSlotRight");

/*
A struct's or FString's `=` onto a comma, its right side an element: `(Bump(), T) = L[Count]`. C++17 sequences an
assignment's right operand before its left ([expr.ass]/1), and an overloaded operator's operands in that same order
([over.match.oper]/2), so L[Count] is located (Count read) before the comma's Bump runs; operator= then reads the
element through its reference. As a statement, in a loop condition, and on the right of &&. BumpS is the other half:
a place the comma's left side changes is read after it, as operator= reads it last.
*/
struct FCsrPair {
  UE_STRUCT;
  int32 A = 1;
  int32 B = 2;
};

class CommaSlotRight : public AActor {
public:
  int32 Count;
  FCsrPair S;
  FCsrPair T;
  FString Str;
  TArray<FCsrPair> L;
  TArray<FString> Names;
  void Bump() { Count += 1; }
  void BumpS() { S.A += 1; }
  [[gnu::noinline]] int32 UsePairV(FCsrPair Q) { return Q.A * 10 + Q.B; }
  [[gnu::noinline]] int32 IsX(FString Q) { return Q == FString("x") ? 1 : 2; }
  void Fill() {
    FCsrPair X1; X1.A = 10; FCsrPair X2; X2.A = 20; FCsrPair X3; X3.A = 30;
    L = {X1, X2, X3};
    Names = {FString("x"), FString("yy"), FString("zzz")};
  }

  int32 SlotStmt(int32 M) { Fill(); Count = 0; (Bump(), T) = L[Count]; return T.A * 100 + Count; }
  int32 StrSlotStmt(int32 M) { Fill(); Count = 0; (Bump(), Str) = Names[Count]; return IsX(Str) * 100 + Count; }
  int32 SlotLoop(int32 M) { Fill(); Count = 0; while (UsePairV((Bump(), T) = L[Count]) < 150) {} return T.A * 100 + Count; }
  int32 StrSlotLoop(int32 M) { Fill(); Count = 0; while (IsX((Bump(), Str) = Names[Count]) < 2) {} return IsX(Str) * 100 + Count; }
  int32 SlotAnd(int32 M) { Fill(); Count = 0; T.A = 1; bool R = M > 0 && UsePairV((Bump(), T) = L[Count]) > 0; return T.A * 100 + Count; }
  int32 PlaceAfter(int32 M) { S.A = M; S.B = 2; (BumpS(), T) = S; return T.A; }
};
