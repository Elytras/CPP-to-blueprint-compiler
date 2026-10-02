#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ExprTemps");

/*
A temporary written with its type in front, and an assignment used as a value. `TArray<int32>{1, 2, M}` as an argument
is that array, as `TArray<int32>({1, 2})` is; a UE_STRUCT's `FEtSlot()` is one with its members' defaults, as
`FEtSlot S{}` is, passed or stored. `A = B = E` sets B, then A to what B now holds, for a number, a string and a struct
alike; `B = A += E` adds first.
*/
struct FEtSlot {
  UE_STRUCT;
  int32 A = 1;
  int32 B = 2;
};

class ExprTemps : public AActor {
public:
  int32 A;
  int32 B;
  FString SA;
  FString SB;
  FEtSlot TA;
  FEtSlot TB;

  int32 Size(const TArray<int32>& L) { return L.Num(); }
  int32 Last(const TArray<int32>& L) { return L[L.Num() - 1]; }
  int32 Code(const FEtSlot& S) { return S.A * 10 + S.B; }

  int32 ListArg(int32 M) { return Size(TArray<int32>{1, 2, M}) * 100 + Last(TArray<int32>{1, 2, M}); }
  int32 StructArg() { return Code(FEtSlot()); }
  int32 StructLocal(int32 M) { FEtSlot S = FEtSlot(); S.A += M; return Code(S); }
  int32 ChainInt(int32 E) { A = B = E; return A * 10 + B; }
  FString ChainString(FString E) { SA = SB = E; return SA + SB; }
  int32 ChainStruct(int32 M) { FEtSlot E = {M, M + 1}; TA = TB = E; return Code(TA) * 100 + Code(TB); }
  int32 ChainCompound(int32 E) { A = 5; B = A += E; return A * 100 + B; }
};
