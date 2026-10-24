// A function that returns a TDelegate. The return type is cut from the function type at the '(' that opens the parameter
// list, the first one outside the template's angle brackets: cut at the first '(' it was "TDelegate<void", refused
// "unimplemented delegate type" (test_bytecode.py delegate_return).
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DelegateReturn");

class DelegateReturn : public AActor {
public:
  int32 Fired = 0;
  TDelegate<void()> Stored;
  TDelegate<void(int32)> Sink;

  void OnTimer() { Fired += 1; }
  void OnInt(int32 X) { Fired += X; }
  TDelegate<void()> Get() { return Stored; }
  TDelegate<void()> Make() { return {this, &DelegateReturn::OnTimer}; }
  TDelegate<void(int32)> GetSink() { return Sink; }
  void Arm() { Stored = Make(); Sink = {this, &DelegateReturn::OnInt}; }
  void Use() { UKismetSystemLibrary::K2_SetTimerDelegate(Get(), 1.0f, false, 0.0f, 0.0f); }
};
