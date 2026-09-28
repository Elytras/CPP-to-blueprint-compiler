#include "UeApi/Types.h"

#include "UeApi/FSD.h"
#include "UeApi/Game/BP_TracerManager_C.h"
#include "UeApi/SimpleUGC.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SubsystemTest");

/* A subsystem's static Get is the editor's Get node: the library getter for its kind, with a world context left out
   being this (or a static's own world context), the node's hidden pin. */
class SubsystemTest : public AActor {
public:
  UObject *Engine() { return UUGCSubsystem::Get(); }
  UObject *GameInstance() { return UDamageSubsystem::Get(); }
  UObject *World() { return GetSubsystem<UTracerManager>(); }
  UObject *OtherWorld(AActor *Other) { return UTracerManager::Get(Other); }
  static UObject *FromStatic(UObject *WorldContextObject) { return UTracerManager::Get(); }
  UObject *Blueprint() { return BP_TracerManager_C::Get(); }
};
