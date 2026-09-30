/*
SpawnAbstract.cpp - spawning or constructing what the engine will not make.

SpawnActor refuses an abstract class with a warning and returns None (LevelActor.cpp 333-347), which the rest of the
function then reads as Accessed None. SpawnObject with a null Outer logs and returns None (GameplayStatics.cpp
606-627). Both compile, each with a warning naming the function that makes the call: the class reaches the engine
call through the helpers' TSubclassOf parameter, so the compiler knows it there. The third thing the engine will not
make, SpawnObject of an abstract class (a Development game asserts, UObjectGlobals.cpp 2362), is refused, as the
editor's Construct Object node refuses the class (K2Node_GenericCreateObject.cpp 13-64): spawn_abstract checks that
with a mod of its own.
*/
#include "../include/Objects.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SpawnAbstract");

/* Abstract: a pure virtual cooks CLASS_Abstract. */
class AbstractShape : public AActor {
public:
  virtual int32 Sides() = 0;
};

class UOuterlessProbe : public UObject {
public:
  int32 V;
};

class SpawnAbstract : public AActor {
public:
  void SpawnShape() { SpawnActor<AbstractShape>(AbstractShape::StaticClass(), FTransform()); }
  void MakeOuterless() { NewObject<UOuterlessProbe>(nullptr); }
};
