#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/RootTwin");

/*
Every actor class gets a DefaultSceneRoot component, cooked as the template DefaultSceneRoot_GEN_VARIABLE under the
class. A component of the mod's own called DefaultSceneRoot must not become a second export of that name there: the
loader finds exports by outer and name, so the second would load as the first object (AsyncLoading.cpp 2898-2952) and
another package's reference would reach only one of them. Refusing the name, or making the mod's component the root
under that name, both keep one. Pending: two DefaultSceneRoot_GEN_VARIABLE are written.
*/
class RootTwin : public AActor {
public:
  UE_COMPONENT(USceneComponent, DefaultSceneRoot);
  int32 N = 1;
};
