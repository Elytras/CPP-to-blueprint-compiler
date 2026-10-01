#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncIfaceStatic");

/*
FuncIfaceStatic implements IFsTell and inherits a static Tell from FsRoot. A static member function implements no
interface function, in C++ or in a Blueprint, and a call to it is bound to FsRoot's (FUNC_Static | FUNC_Final), so no
function of the class can take it over. The class gets the empty stub every function left out gets, and
`FsRoot::Tell(4)` still runs FsRoot's.
*/
class IFsTell {
public:
  UE_INTERFACE;
  int32 Tell(int32 V);
};

class FsRoot : public AActor {
public:
  static int32 Tell(int32 V) { return V + 1; }
};

class FuncIfaceStatic : public FsRoot, public IFsTell {
public:
  int32 Ask() { return FsRoot::Tell(4); }
};
