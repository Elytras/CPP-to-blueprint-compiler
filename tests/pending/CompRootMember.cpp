#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompRootMember");

/*
A member named DefaultSceneRoot where nothing reads it as the root's variable is a member like any other. CompRootMember's
Body takes the root, so its construction script lists no DefaultSceneRoot node and the class has no variable of that
name but this one. RootMemberKid's is no object property, and ExecuteNodeOnActor looks the root's variable up as one
(FindFProperty<FObjectPropertyBase>, SCS_Node.cpp 164), so RootMemberBase's own still holds the root.
*/
class CompRootMember : public AActor {
public:
  UE_COMPONENT(UStaticMeshComponent, Body);
  int32 DefaultSceneRoot = 5;
};

class RootMemberBase : public AActor {
public:
  int32 Count = 1;
};

class RootMemberKid : public RootMemberBase {
public:
  int32 DefaultSceneRoot = 7;
};
