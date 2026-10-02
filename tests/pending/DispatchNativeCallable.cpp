/* DispatchNativeCallable: Broadcast on a native dispatcher the engine marks BlueprintCallable, AFSDGameState's
   OnTerrainGenerated (UPROPERTY(BlueprintAssignable, BlueprintCallable)): the editor's Call node takes it, and its
   EX_CallMulticastDelegate needs a signature function with the dispatcher's parameters. AssetGen refuses it today
   ("Broadcast needs the dispatcher's signature function, which only a UE_DISPATCHER of this class or a Blueprint parent
   has"). */
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
