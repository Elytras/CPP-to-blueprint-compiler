#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DropResult");

/*
Calls whose result a statement throws away. A statement's value is stepped into ProcessLocalScriptFunction's 64-byte
stack buffer, which nothing constructs or destroys (ScriptCore.cpp 1058, 1120): an FString or TArray result is
assigned over garbage there and never freed. So a call returning one needs a local to land in, as the editor gives
every return pin; Bump's int32 may stay a bare statement.
*/
class DropResult : public AActor {
public:
  FString Name;
  TArray<int32> Items;
  int32 Calls;

  [[gnu::noinline]] FString Describe() {
    Calls += 1;
    return Name;
  }
  [[gnu::noinline]] TArray<int32> Copy() {
    Calls += 10;
    return Items;
  }
  [[gnu::noinline]] int32 Bump() {
    Calls += 100;
    return Calls;
  }
  void Run() {
    Describe();
    Copy();
    Bump();
  }
};
