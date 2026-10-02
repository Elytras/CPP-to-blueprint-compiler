/* DispatchNativeCallable: Broadcast on a native dispatcher the engine marks BlueprintCallable, AFSDGameState's
   OnTerrainGenerated (UPROPERTY(BlueprintAssignable, BlueprintCallable)): the editor's Call node takes it, and its
   EX_CallMulticastDelegate needs a signature function with the dispatcher's parameters. It lives in tests/pending
   because only a UeApi that marks callable dispatchers (genueapi's <D>__UeDispatcher) lets it compile: against an older
   one AssetGen cannot tell OnTerrainGenerated from OnDestroyed and refuses both (test_bytecode.py runs it or reports a
   gap accordingly). DispatchNativeKid broadcasts it too: its own signature function must not take the name of
   its parent's, which FindFunctionByName would find in place of the parent's (invariants.py func_super_link). */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DispatchNativeCallable");

class DispatchNativeCallable : public AActor {
public:
  AFSDGameState *State = nullptr;
  int32 Heard = 0;

  void Generated() { Heard += 1; }
  void Hook() { State->OnTerrainGenerated.Add(this, &DispatchNativeCallable::Generated); }
  void Fire() { State->OnTerrainGenerated.Broadcast(); }
};

class DispatchNativeKid : public DispatchNativeCallable {
public:
  void FireKid() { State->OnTerrainGenerated.Broadcast(); }
};
