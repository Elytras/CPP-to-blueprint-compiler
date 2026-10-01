#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompRootMember");

/*
A member named DefaultSceneRoot where no construction script lists the DefaultSceneRoot node is a member like any
other. CompRootMember's Body takes the root, so its SCS lists no such node and the class has no variable of that name
but this one; RootMemberKid's actor gets its root from its parent's Body too.
*/
class CompRootMember : public AActor {
public:
  UE_COMPONENT(UStaticMeshComponent, Body);
  int32 DefaultSceneRoot = 5;
};

class RootMemberKid : public CompRootMember {
public:
  UE_COMPONENT(UPointLightComponent, Lamp);
  int32 Count = 7;
};
