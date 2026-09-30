#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"
#include "UeAssets/UStaticMesh.h"

#include "../include/Objects.h" // NewObject, by its path from this file

UE_MOD_PACKAGE("/Game/_ElytrasMods/PreloadRefs");

/*
Objects a payload names: a mod class as a bytecode constant (NewObject's class in Make) and as a property's class
(Probe), a dispatcher's signature as the target of its Broadcast (Ping), and an engine mesh as a component template's
default. While an export is serialized the loader resolves each as whatever is already created, so each must be
created first: every export lists what its payload names as create-before-serialize, as the cook's DependsMap does.
*/
class UPreloadProbe : public UObject {
public:
  int32 Value;
};

class PreloadRefs : public AActor {
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UStaticMeshComponent, Mesh);
  UE_DEFAULTS { Mesh->StaticMesh = &UeAssets::UStaticMesh::Engine::BasicShapes::Cylinder; }
  UE_DISPATCHER(OnPing, int32 N);
  UPreloadProbe *Probe;

public:
  UObject *Make() { return NewObject<UPreloadProbe>(this); }
  void Ping() { OnPing.Broadcast(1); }
};
