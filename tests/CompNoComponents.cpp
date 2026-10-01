#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompNoComponents");

/*
An actor class that declares no UE_COMPONENT and has no root to inherit. The editor keeps its DefaultSceneRoot node in
RootNodes and AllNodes, as 40 of the game's classes save it (ENE_EnemySpawner), so the actor's root is that node's
component: named DefaultSceneRoot and net addressable (SCS_Node.cpp 99, 107). With neither list ExecuteScriptOnActor
makes a plain SceneComponent instead (SimpleConstructionScript.cpp 690-702), which nothing marks net addressable
(ActorComponent.cpp 1901-1913). NoCompKid's Lamp attaches to its parent's root and keeps its offset.
*/
class CompNoComponents : public AActor {
public:
  int32 Count = 3;

  void ReceiveBeginPlay() { Count = Count + 1; }
};

class NoCompKid : public CompNoComponents {
public:
  UE_COMPONENT(UPointLightComponent, Lamp);

  UE_DEFAULTS { Lamp->RelativeLocation = FVector(0.0f, 0.0f, 40.0f); }
};
