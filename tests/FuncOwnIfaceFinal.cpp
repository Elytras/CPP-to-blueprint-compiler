#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncOwnIfaceFinal");

/*
An implementation of an interface the class itself lists takes the interface function's flags, as the editor's does
(KismetCompiler.cpp 1842-1857): BlueprintEvent, not Final, also in a `final` class (FoiFinal) and in a UE_FINAL_AS base
(FoiBase), whose own new functions alone are Final. Left, which neither declares, is the empty stub, flagged the same.
*/
class IFoiTell {
public:
  UE_INTERFACE;
  int32 Tell(int32 V);
  int32 Left(int32 V);
};

class FoiFinal final : public AActor, public IFoiTell {
public:
  int32 Tell(int32 V) { return V + 1; }
  int32 Ask(int32 V) { return Tell(V) * 10; }
};

class FoiBase : public AActor, public IFoiTell {
public:
  int32 Tell(int32 V) { return V + 2; }
  int32 Ask(int32 V) { return Tell(V) * 10; }
};

UE_FINAL_AS(FoiBase, FuncOwnIfaceFinal);
