/* DelegateParmSig: TDelegate parameters of mod functions, and the delegate values handed to them. The editor's override
   copies the parent function's parameters (K2Node_FunctionEntry::AllocateDefaultPins -> CreatePinsForFunctionEntryExit
   -> ConvertPropertyToPinType), so an override's DelegateProperty names the parent parameter's SignatureFunction
   (CreatePropertyOnScope, KismetCompilerMisc.cpp 1189-1197); a Create Event node types its delegate with the signature
   of the pin it is wired to (GetDelegateSignature, K2Node_CreateDelegate.cpp 317-331; DelegateNodeHandlers.cpp 258-262):
   a parameter's, or a variable's for a Set node. A static hiding its parent's static has that one for its super, and is
   held to its parameters the same. DelegateParmSig sorts before its parent DelegateParmTop, so it is generated first;
   DelegateParmZKid after it. */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DelegateParmSig");

class DelegateParmHelper : public AActor {
public:
  int32 Got = 0;
  void Ping() { Got += 1; }
};

class DelegateParmSib : public AActor {
public:
  void Sib(TDelegate<void()> S) { UKismetSystemLibrary::K2_SetTimerDelegate(S, 3.0f, false, 0.0f, 0.0f); }
};

class DelegateParmTop : public AActor {
public:
  DelegateParmHelper *H = nullptr;
  DelegateParmSib *S = nullptr;
  TDelegate<void()> Var;
  virtual void Use(TDelegate<void()> D) { UKismetSystemLibrary::K2_SetTimerDelegate(D, 1.0f, false, 0.0f, 0.0f); }
  void Take(TDelegate<void()> T) { Var = T; }
  virtual void Out(TDelegate<void()> &O) { O = Var; }
  static void Arm(TDelegate<void()> A) { UKismetSystemLibrary::K2_SetTimerDelegate(A, 5.0f, false, 0.0f, 0.0f); }
};

class DelegateParmSig : public DelegateParmTop {
public:
  void Use(TDelegate<void()> D) override { UKismetSystemLibrary::K2_SetTimerDelegate(D, 2.0f, false, 0.0f, 0.0f); }
  void Out(TDelegate<void()> &O) override { O = Var; }
  static void Arm(TDelegate<void()> A) { UKismetSystemLibrary::K2_SetTimerDelegate(A, 6.0f, false, 0.0f, 0.0f); }
  void Mine(TDelegate<void()> M) { Var = M; }
  void CallTake() { Take({H, &DelegateParmHelper::Ping}); }
  void CallUse() { Use({H, &DelegateParmHelper::Ping}); }
  void CallMine() { Mine({H, &DelegateParmHelper::Ping}); }
  void CallSib() { S->Sib({H, &DelegateParmHelper::Ping}); }
  void SetVar() { Var = {H, &DelegateParmHelper::Ping}; }
};

class DelegateParmZKid : public DelegateParmTop {
public:
  void Use(TDelegate<void()> D) override { UKismetSystemLibrary::K2_SetTimerDelegate(D, 4.0f, false, 0.0f, 0.0f); }
};
