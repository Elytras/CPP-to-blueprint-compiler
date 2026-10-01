/*
UberDeferGuard.cpp - deferred spawns and component adds finished once, but not the way the editor's nodes do it.

The engine asks only that a deferred actor or component is finished at most once (FinishSpawning's ensure,
Actor.cpp 3206; OnComponentCreated's, ActorComponent.cpp 1359). Finishing it behind a null test, from another
function, or at another transform than it was begun with (recomposed, Actor.cpp 3210-3232) is correct code that
the editor's nodes never write, so the game's own content cannot show that a rule accepts it: this mod does.
*/
#include "../include/Objects.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/UberDeferGuard");

class UberDeferGuard : public AActor {
public:
  int32 Tag;
  UberDeferGuard *Pending;
  USceneComponent *Part;

  /* Finished only when the spawn gave an actor. */
  void GuardIf(FTransform Where) {
    UberDeferGuard *T = SpawnActorDeferred<UberDeferGuard>(UberDeferGuard::StaticClass(), Where);
    if (T) {
      T->Tag = 1;
      FinishSpawning(T, Where);
    }
  }

  /* The same, leaving early. */
  void GuardReturn(FTransform Where) {
    UberDeferGuard *T = SpawnActorDeferred<UberDeferGuard>(UberDeferGuard::StaticClass(), Where);
    if (!UKismetSystemLibrary::IsValid(T))
      return;
    T->Tag = 2;
    FinishSpawning(T, Where);
  }

  /* Begun by one event, finished by another. */
  void Begin(FTransform Where) { Pending = SpawnActorDeferred<UberDeferGuard>(UberDeferGuard::StaticClass(), Where); }
  void Finish(FTransform Where) { FinishSpawning(Pending, Where); }

  /* Finished at another transform than it was begun with: the engine moves it there. */
  void Moved(FTransform From, FTransform To) {
    UberDeferGuard *T = SpawnActorDeferred<UberDeferGuard>(UberDeferGuard::StaticClass(), From);
    T->Tag = 3;
    FinishSpawning(T, To);
  }

  /* A component finished only when there is one. */
  void CompGuard() {
    USceneComponent *C = AddComponentDeferred<USceneComponent>(this);
    if (C) {
      C->bHiddenInGame = true;
      FinishComponent(this, C);
    }
    Part = C;
  }
};
