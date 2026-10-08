// An override of a function returning a TDelegate: the engine matches an override's parameters, the return value
// included, against its parent's (func_override_params): a DelegateProperty names the signature function it is typed by,
// and the override's ReturnValue named a signature of its own where its parent's names the parent's. A parameter has
// had the parent's since r7 (ImportedParamSignature); the return value did not, as no return could be a delegate
// (test_bytecode.py delegate_return_override).
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DelegateReturnKid");

class DrkBase : public AActor {
public:
  TDelegate<void()> Stored;
  virtual TDelegate<void()> Get() { return Stored; }
};

class DrkKid : public DrkBase {
public:
  int32 Fired = 0;
  void OnTimer() { Fired += 1; }
  TDelegate<void()> Get() override { return {this, &DrkKid::OnTimer}; }
};

class DrkUser : public AActor {
public:
  DrkBase *Other = nullptr;
  void Use() { UKismetSystemLibrary::K2_SetTimerDelegate(Other->Get(), 1.0f, false, 0.0f, 0.0f); }
};
