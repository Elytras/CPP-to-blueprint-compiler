#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/RowDefaults");

/*
Class Defaults the REFERENCE.md rows describe, one member each; test_bytecode.py (row_defaults) reads the cooked
default object back and compares every value with the one written here.
*/
class RowDefaults : public AActor {
public:
  TSoftClassPtr<AActor> Kind = "/Game/A/BP_A.BP_A_C";
  TArray<TSoftClassPtr<AActor>> Kinds = {"/Game/A/BP_A.BP_A_C", "/Game/B/BP_B.BP_B_C"};
  FFloatInterval Delay = FFloatInterval(1, 5);
  float Slow = 0.75;
  TSubclassOf<AActor> Pick = AActor::StaticClass();
};
