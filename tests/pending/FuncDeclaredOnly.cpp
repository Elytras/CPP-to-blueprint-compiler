#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncDeclaredOnly");

/*
FdoRoot declares Tell and Scale and defines neither, so it compiles no function of those names: FuncDeclaredOnly's own
Tell (a static) and Scale hide nothing a Blueprint has, and have no super, as ParentClass->FindFunctionByName finds none
(KismetCompiler.cpp 1733-1774). Plus is defined, and stays FdoRoot's.
*/
class FdoRoot : public AActor {
public:
  static int32 Tell(int32 V);
  int32 Scale(int32 V);
  int32 Plus(int32 V) { return V + 1; }
};

class FuncDeclaredOnly : public FdoRoot {
public:
  static int32 Tell(int32 V) { return V * 2; }
  int32 Scale(int32 V) { return V * 3; }
  int32 Use(int32 V) { return Tell(V) * 100 + Scale(V) + Plus(V) * 10000; }
};
