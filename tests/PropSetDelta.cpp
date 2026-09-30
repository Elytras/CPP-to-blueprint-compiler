/*
PropSetDelta.cpp - a child Blueprint that gives an inherited TSet / TMap a default of its own.

A package archive does intra-property delta: the CDO's value of an inherited set or map is read against the parent CDO's
(the loader copies the parent's value, removes the elements the tag lists as removed, then adds the rest:
PropertySet.cpp 285-358, PropertyMap.cpp 316-400). So the child's tag lists the parent's elements the child lacks as
removed, as the editor saves it; written as additions only, the child would load the union of both defaults.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PropSetDelta");

class PropSetBase : public AActor {
public:
  TSet<int32> Ids = {1, 2};
  TMap<FName, int32> Score = {{"a", 1}, {"c", 3}};
};

class PropSetDelta : public PropSetBase {
public:
  UE_DEFAULTS {
    Ids = {2, 3};
    Score = {{"a", 5}, {"b", 2}};
  }
};
