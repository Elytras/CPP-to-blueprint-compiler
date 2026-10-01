#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompCharRoot");

/*
A character whose own components are none of them scene components, so its SCS keeps the DefaultSceneRoot node in
RootNodes (Blueprint.cpp). The actor has a root before the SCS runs all the same: ACharacter's constructor makes the
capsule the root (Character.cpp 59), and ExecuteConstruction would take the first unattached native scene component
anyway (ActorConstruction.cpp 736-746). So ExecuteScriptOnActor skips the DefaultSceneRoot node
(SimpleConstructionScript.cpp 648), and the rules that model the construction must skip it too.
*/
class CompCharRoot : public ACharacter {
public:
  UE_COMPONENT(UPawnNoiseEmitterComponent, Noise);
};
