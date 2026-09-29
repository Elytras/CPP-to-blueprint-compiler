#include "../include/Objects.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompUcs");

/*
The construction script. The engine calls only the most derived class's UserConstructionScript, by name
(AActor::ProcessUserConstructionScript), so a parent's construction logic runs in a child only through an explicit
parent call, as the editor's "call to parent function" node does: UcsBase::UserConstructionScript() below.
*/
class UcsBase : public AActor {
public:
  int32 Hits = 0;

  void UserConstructionScript() { Hits += 1; }
};

/* Runs its parent's construction first, once. */
class CompUcs : public UcsBase {
public:
  void UserConstructionScript() {
    UcsBase::UserConstructionScript();
    Hits += 10;
  }
};

/* Replaces it: the parent's never runs. */
class UcsSolo : public UcsBase {
public:
  void UserConstructionScript() { Hits += 100; }
};

/* Adding a component in the construction script is what it is for, and stays accepted. */
class UcsAdder : public AActor {
public:
  USceneComponent *Made;

  void UserConstructionScript() { Made = AddComponentByType<USceneComponent>(this); }
};
