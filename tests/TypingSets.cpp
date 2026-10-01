#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TypingSets");

/*
Container literals: EX_SetArray / EX_SetSet / EX_SetMap fill a container variable from element expressions typed as
its inner (key, value) property, and SetSet / SetMap carry the element count, which the VM trusts to be 0 exactly when
no element follows (ScriptCore.cpp 3432-3497). Bytes has a uint8 inner, so its elements are byte constants; Rates
alternates int32 keys and float values. Second reads an element in place (EX_ArrayGetByRef, an int32 index).
*/
class TypingSets : public AActor {
  TSet<int32>        Seen;
  TMap<int32, float> Rates;
  TArray<uint8>      Bytes;
  TArray<FVector>    Points;

public:
  void Fill() {
    Seen = {1, 2, 3};
    Rates = {{1, 0.5f}, {2, 1.5f}};
    Bytes = {7, 250};
  }

  void FillPoints() { Points = {FVector(1, 2, 3), FVector(4, 5, 6)}; }

  uint8 Second() { return Bytes[1]; }
};
