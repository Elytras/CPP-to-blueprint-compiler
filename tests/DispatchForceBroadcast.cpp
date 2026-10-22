/* DispatchForceBroadcast: UE_FORCE_BROADCAST on native dispatchers the engine marks BlueprintAssignable but not
   BlueprintCallable, AActor's sparse OnDestroyed on this actor and UEnemySpawnManager's inline OnEnemySpawned on
   another object. The editor's Call node refuses both; execCallMulticastDelegate does not check the flag, so the bound
   handlers run. Gone logs through LogToModManager (noinline, so the DLL's sink sees it), which the in-game check reads. */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DispatchForceBroadcast");

class DispatchForceBroadcast : public AActor {
public:
  UEnemySpawnManager *Spawner = nullptr;
  int32 Heard = 0;
  int32 Spawned = 0;

  [[gnu::noinline]] void LogToModManager(FString Msg) {}
  void Gone(AActor *DestroyedActor) {
    Heard += 1;
    LogToModManager("DispatchForceBroadcast: OnDestroyed handler ran");
  }
  void Seen(APawn *Enemy, UEnemyDescriptor *Descriptor) { Spawned += 1; }

  int32 Fire() {
    OnDestroyed.Add(this, &DispatchForceBroadcast::Gone);
    UE_FORCE_BROADCAST(OnDestroyed).Broadcast(this);
    OnDestroyed.Remove(this, &DispatchForceBroadcast::Gone);
    return Heard;
  }
  void HookSpawner() { Spawner->OnEnemySpawned.Add(this, &DispatchForceBroadcast::Seen); }
  void FireSpawner() { UE_FORCE_BROADCAST(Spawner->OnEnemySpawned).Broadcast(nullptr, nullptr); }
};
