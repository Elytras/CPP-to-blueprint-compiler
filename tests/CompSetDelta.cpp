/*
CompSetDelta.cpp - UE_DEFAULTS giving a component's TSet / TMap a value of its own.

A component's template loads over its archetype: the component class's default object for a class's own component, the
parent class's template for one it inherits. A set or a map loads as the archetype's value with the tag's removals
taken out and its elements added (PropertySet.cpp 285-358, PropertyMap.cpp 316-400), so the tag lists the archetype's
elements the new value lacks as removed, as PropSetDelta's does for a class's own member.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompSetDelta");

struct FCsdHeld {
  UE_STRUCT;
  TSet<int32> Ids;
  int32 N;
};

class UCsdHolder : public UActorComponent {
public:
  TSet<int32> Ids = {1, 2};
  TMap<FName, int32> Score = {{"a", 1}, {"c", 3}};
  FCsdHeld Held = {{1, 2}, 3};
};

class CsdBase : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UCsdHolder, Holder);
  UE_COMPONENT(UCsdHolder, Plain); // left alone here

  UE_DEFAULTS {
    Holder->Ids = {2, 3};                 // over the component class's {1, 2}
    Holder->Score = {{"a", 5}, {"b", 2}}; // over {a: 1, c: 3}
    Holder->Held = {{2, 4}, 3};           // a set inside a struct
  }
};

class CompSetDelta : public CsdBase {
  UE_DEFAULTS {
    Holder->Ids = {3, 9}; // an inherited component: over the parent's template, {2, 3}
    Plain->Ids = {7};     // one the parent left alone: over the component class's {1, 2}
  }
};
