#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompCharRoot");

/*
A character whose own components are none of them scene components. The actor has a root before its SCS runs all the
same: ACharacter's constructor makes the capsule the root (Character.cpp 59), and ExecuteConstruction would take the
first unattached native scene component anyway (ActorConstruction.cpp 736-746). So ExecuteScriptOnActor would skip a
DefaultSceneRoot node (SimpleConstructionScript.cpp 648), the editor lists none (ValidateSceneRootNodes, 1132-1150),
and the rules that model the construction must build none either way.
*/
class CompCharRoot : public ACharacter {
public:
  UE_COMPONENT(UPawnNoiseEmitterComponent, Noise);
};

/* The same under a Blueprint parent: NoiseBase's SCS leaves the actor its root before NoiseKid's runs. */
class NoiseBase : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
};

class NoiseKid : public NoiseBase {
public:
  UE_COMPONENT(UPawnNoiseEmitterComponent, Noise);
};
