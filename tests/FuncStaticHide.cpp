#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncStaticHide");

/*
FuncStaticHide's static Tell hides FshRoot's static Tell, as C++ lets one static hide another. Every call to a static is
bound to the one it names, so each runs its own: `Tell(V)` in FuncStaticHide is its own, `FshRoot::Tell(V)` the root's.
The editor's super for a function is ParentClass->FindFunctionByName (KismetCompiler.cpp 1733-1774), FshRoot's Tell.
*/
class FshRoot : public AActor {
public:
  static int32 Tell(int32 V) { return V + 1; }
};

class FuncStaticHide : public FshRoot {
public:
  static int32 Tell(int32 V) { return V * 3; }
  int32 Use(int32 V) { return Tell(V) * 100 + FshRoot::Tell(V); }
};
