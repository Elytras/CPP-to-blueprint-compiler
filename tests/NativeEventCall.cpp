// A call to a native BlueprintEvent from mod code. An event is overridden by script, so the call is by name (EX_VirtualFunction
// to ProcessEvent), as the editor makes it (KismetCompilerVMBackend.cpp 1222, 1253-1282): EX_FinalFunction on the engine's
// UFunction runs its thunk, which for a BlueprintNativeEvent calls _Implementation and for an implementable one is empty,
// so a Blueprint subclass's override never runs (test_bytecode.py native_event_call).
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/NativeEventCall");

class NativeEventCall : public AActor {
public:
  int32 Hits = 0;
  void ReceiveAnyDamage(float Damage, const UDamageType *DamageType, AController *InstigatedBy, AActor *DamageCauser) { Hits = Hits + 1; }
  void CallSelf() { ReceiveBeginPlay(); }
  void CallOther(NativeEventCall *Other) { Other->ReceiveBeginPlay(); }
  void CallOtherActor(AActor *Other) { Other->ReceiveBeginPlay(); }
  void CallArgs(AActor *Other) { Other->ReceiveAnyDamage(2.5f, nullptr, nullptr, this); }
};
