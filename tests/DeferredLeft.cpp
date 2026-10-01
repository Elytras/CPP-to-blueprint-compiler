/*
DeferredLeft.cpp - deferred spawns and component adds that are never finished, or finished differently.

An actor from BeginDeferredActorSpawnFromClass runs its construction script and BeginPlay only in FinishSpawningActor
(Actor.cpp 3185-3251). A component added with bDeferredFinish is attached, registered and told it was created only in
FinishAddComponent (ActorConstruction.cpp 1165-1212), and the bManualAttachment given to the add is then never read
(1157-1160): the finish's own decides the attachment. The editor wires each pair itself (K2Node_SpawnActorFromClass.cpp
412-435, K2Node_AddComponent.cpp 463-507); in C++ nothing ties them. (A finish at another transform than the spawn's
is not here: FinishSpawning recomposes it on purpose, Actor.cpp 3212-3232, as if spawned there.) The compiler warns
at each function here. One that hands the object on - stores it in a member or out parameter, returns it, or passes
it to a script function - is not warned about: UberDeferGuard finishes in Finish() what Begin() stored.
*/
#include "../include/Objects.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DeferredLeft");

class DeferredLeft : public AActor {
public:
  /* Never finished: the actor stays in the world with no construction script and no BeginPlay. */
  void NoFinish() { SpawnActorDeferred<AActor>(AActor::StaticClass(), FTransform()); }

  /* Never finished: the component is not attached, registered or ticked. */
  void CompNoFinish() { AddComponentDeferred<USceneComponent>(this); }

  /* Added with bManualAttachment, finished without it: the finish attaches it to the root after all. */
  void CompManual() {
    USceneComponent *C = AddComponentDeferred<USceneComponent>(this, USceneComponent::StaticClass(), true);
    FinishComponent(this, C);
  }
};
