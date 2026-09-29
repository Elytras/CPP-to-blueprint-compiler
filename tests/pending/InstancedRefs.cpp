#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/InstancedRefs");

/*
Every member that refers to a component is an instanced reference: ActorComponent is DefaultToInstanced, and
so is a mod's own component class, so the engine's compiler flags each such object property
CPF_InstancedReference, each container or struct member that holds one CPF_ContainsInstancedReference, and
the class CLASS_HasInstancedReference. Instancing walks only flagged members, so an unflagged one keeps
pointing at the template's component in every copy spawned from it. This holds for a UE_COMPONENT's
variable and for a plain pointer alike.
*/
class RefPart : public UActorComponent {
public:
  float Rate;
};

class InstancedRefs : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  USceneComponent *Spare;
  TArray<UStaticMeshComponent *> Pieces;
  TMap<int32, USceneComponent *> ByIndex;
  FHitResult LastHit;
  RefPart *Mine;

  int32 CountPieces() { return Pieces.Num(); }
};
