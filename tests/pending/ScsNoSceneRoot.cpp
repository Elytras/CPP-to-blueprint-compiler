#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ScsNoSceneRoot");

/*
An actor whose only component is not a scene component still needs a root: the rotating movement
component turns the actor's root (its UpdatedComponent), and an actor without one has no transform at
all. USimpleConstructionScript::ExecuteScriptOnActor makes a SceneComponent root only when RootNodes is
empty; with Spinner as the one root node it makes none, and no node of Spinner's makes one either. The
editor keeps its DefaultSceneRoot node in RootNodes until another SCENE component can be the root.
Pending: RootNodes is [Spinner] and the DefaultSceneRoot node is left out, so the actor ends its
construction script without a RootComponent.
*/
class ScsNoSceneRoot : public AActor {
public:
  UE_COMPONENT(URotatingMovementComponent, Spinner);

  UE_DEFAULTS { Spinner->RotationRate = FRotator(0.0f, 90.0f, 0.0f); }
};
