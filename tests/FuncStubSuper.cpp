#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncStubSuper");

/*
FssRoot lists IFssTell and leaves Tell out, so it gets the editor's empty stub of it. A subclass's Tell overrides that
stub, as ParentClass->FindFunctionByName finds it: its super is FssRoot's Tell and it takes the stub's flags, in a plain
subclass (FssKid) and in a UE_FINAL_AS base (FssBase), whose own new functions alone are Final.
*/
class IFssTell {
public:
  UE_INTERFACE;
  int32 Tell(int32 V);
};

class FssRoot : public AActor, public IFssTell {
public:
  int32 RootCall(int32 V) { return Tell(V) + 1; }
};

class FssKid : public FssRoot {
public:
  int32 Tell(int32 V) { return V * 10; }
};

class FssBase : public FssRoot {
public:
  int32 Tell(int32 V) { return V * 20; }
  int32 UseTell(int32 V) { return Tell(V) + 2; }
};

UE_FINAL_AS(FssBase, FuncStubSuper);
