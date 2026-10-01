#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompRootVariable");

/*
Where the SCS lists its DefaultSceneRoot node, the class has a variable of that name, as the editor gives every node it
lists one (KismetCompiler.cpp 884-898) and 40 of the game's classes have it (ENE_EnemySpawner's DefaultSceneRoot):
ExecuteNodeOnActor stores the component there, and logs on every spawn that it found no such property when there is
none (SCS_Node.cpp 159-178). CompRootVariable has no component, RootVarMover a movement component alone, and
RootVarKid's actor finds its parent's variable.
*/
class CompRootVariable : public AActor {
public:
  int32 Count = 3;
};

class RootVarMover : public AActor {
public:
  UE_COMPONENT(URotatingMovementComponent, Spinner);
};

class RootVarKid : public CompRootVariable {
public:
  UE_COMPONENT(UPointLightComponent, Lamp);
};
