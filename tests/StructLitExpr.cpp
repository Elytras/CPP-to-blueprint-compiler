#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/StructLitExpr");

/*
Whole-struct literals whose members are not constants: a `?:`, a `&&`, a call, a member of the variable being
assigned, a value for a Transient member. Each one is what C++ makes of it: the members run left to right, each once,
and a member that reads the destination reads it as it was before the assignment. A local, a member variable, an
argument, a return value, a struct nested in another, the elements of a TArray, and a UE_STRUCT's braces.
*/
struct FCondSlot {
  UE_STRUCT;
  int32 A = 0;
  int32 B = 0;
};

class StructLitExpr : public AActor {
public:
  FVector2D Pos;
  int32 Count = 0;

  int32 Twice(int32 V) { return V * 2; }
  int32 Bump() { Count += 1; return Count; }
  float PosX() { return Pos.X; }
  float Take(FVector2D V) { return V.X * 10.0f + V.Y; }
  FVector2D Make(int32 M) { return {M == 0 ? 1.0f : 2.0f, 3.0f}; }

  float Local(int32 M) { FVector2D V = {1.0f, M == 0 ? 2.0f : 3.0f}; return V.X * 10.0f + V.Y; }
  float Paren(int32 M) { FVector2D V(M == 0 ? 2.0f : 3.0f, 1.0f); return V.X * 10.0f + V.Y; }
  float Member(int32 M) { Pos = {M == 0 ? 4.0f : 5.0f, 6.0f}; return Pos.X * 10.0f + Pos.Y; }
  float Arg(int32 M) { return Take(FVector2D(1.0f, M == 0 ? 7.0f : 8.0f)); }
  float ArgBraced(int32 M) { return Take({M == 0 ? 7.0f : 8.0f, 1.0f}); }
  float Ret(int32 M) { FVector2D V = Make(M); return V.X * 10.0f + V.Y; }
  float Nested(int32 M) { FBox2D B = {{1.0f, M == 0 ? 2.0f : 3.0f}, {4.0f, 5.0f}, 1}; return B.Min.Y + B.Max.X * 10.0f; }
  float Array(int32 M) { TArray<FVector2D> A = {{1.0f, M == 0 ? 2.0f : 3.0f}, {4.0f, 5.0f}}; return A[0].Y + A[1].X * 10.0f; }
  int32 Both(int32 M) { FIntPoint P = {M, (int32)(M > 0 && M < 5)}; return P.X * 10 + P.Y; }
  int32 Call(int32 M) { FIntPoint P = {Twice(M), 1}; return P.X * 10 + P.Y; }
  int32 Order(int32 M) { Count = 0; FIntPoint P = {Bump(), M == 0 ? Count * 10 : 0}; return P.X * 100 + P.Y; }
  float Swap(int32 M) { FVector2D V = {1.0f, 2.0f}; V = {V.Y, V.X}; return V.X * 10.0f + V.Y; }
  float SwapMember(int32 M) { Pos = {1.0f, 2.0f}; Pos = {Pos.Y, Pos.X}; return Pos.X * 10.0f + Pos.Y; }
  float ReadBack(int32 M) { Pos = {1.0f, 2.0f}; Pos = {3.0f, PosX()}; return Pos.X * 10.0f + Pos.Y; }
  int32 Mod(int32 M) { FCondSlot S = {1, M == 0 ? 2 : 3}; return S.A * 10 + S.B; }
  int32 ModSwap(int32 M) { FCondSlot S = {1, 2}; S = {S.B, S.A}; return S.A * 10 + S.B; }
  int32 Transient(int32 M) {
    FMaterialAttributesInput S = FMaterialAttributesInput(3, FName("In"), FName("Ex"), M + 4);
    return S.PropertyConnectedBitmask * 10 + S.OutputIndex;
  }
};
