/*
SpawnAbstract.cpp - spawning or constructing what the engine will not make.

SpawnActor refuses an abstract class with a warning and returns None (LevelActor.cpp 333-347), which the rest of the
function then reads as Accessed None. SpawnObject creates an abstract class quietly in a Shipping game such as DRG
and asserts in a Development one (UObjectGlobals.cpp 2362), and with a null Outer it logs and returns None
(GameplayStatics.cpp 606-627). The editor's nodes refuse the first two outright when the class is picked on the node
(K2Node_GenericCreateObject.cpp 13-64). All three compile, each with a warning naming the function that makes the call:
the class reaches the engine call through the helpers' TSubclassOf parameter, so the compiler knows it there.
*/
#include "../include/Objects.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SpawnAbstract");

/* Abstract: a pure virtual cooks CLASS_Abstract. */
class AbstractShape : public AActor {
public:
  virtual int32 Sides() = 0;
};

class UAbstractSpec : public UObject {
public:
  virtual int32 N() = 0;
};

class UOuterlessProbe : public UObject {
public:
  int32 V;
};

class SpawnAbstract : public AActor {
public:
  void SpawnShape() { SpawnActor<AbstractShape>(AbstractShape::StaticClass(), FTransform()); }
  void MakeSpec() { NewObject<UAbstractSpec>(this); }
  void MakeOuterless() { NewObject<UOuterlessProbe>(nullptr); }
};
