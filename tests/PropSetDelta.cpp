/*
PropSetDelta.cpp - a child Blueprint that gives an inherited TSet / TMap a default of its own.

A package archive does intra-property delta: the CDO's value of an inherited set or map is read against the parent CDO's
(the loader copies the parent's value, removes the elements the tag lists as removed, then adds the rest:
PropertySet.cpp 285-358, PropertyMap.cpp 316-400). So the child's tag lists the parent's elements the child lacks as
removed, as the editor saves it; written as additions only, the child would load the union of both defaults. A set or
map inside a struct written as tags is one too: the struct loads each member over the parent's struct's
(UScriptStruct::SerializeItem passes the defaults on, Class.cpp 2775). Deep, which the child leaves alone, is what
MapPatch reaches by a path the CDO has no tag for.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PropSetDelta");

struct FPropSetHeld {
  UE_STRUCT_IN("/Game/_ElytrasMods/PropSetDelta");
  TSet<int32> Ids;
  TMap<FName, int32> Score;
  int32 N;
};

struct FPropSetDeep {
  UE_STRUCT_IN("/Game/_ElytrasMods/PropSetDelta");
  FPropSetHeld In;
  int32 M;
};

class PropSetBase : public AActor {
public:
  TSet<int32> Ids = {1, 2};
  TMap<FName, int32> Score = {{"a", 1}, {"c", 3}};
  FPropSetHeld Held = {{1, 2}, {{"a", 1}, {"c", 3}}, 3};
  FPropSetDeep Deep = {{{1, 2}, {{"a", 1}, {"c", 3}}, 3}, 4};
  TMap<FString, int32> Text = {{"A", 1}};   // MapPatch's FString case
};

class PropSetDelta : public PropSetBase {
public:
  UE_DEFAULTS {
    Ids = {2, 3};
    Score = {{"a", 5}, {"b", 2}};
    Held = {{2, 3}, {{"a", 5}, {"b", 2}}, 3};
  }
};
