#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncIfaceInherited");

/*
FuncIfaceInherited implements IFiTell and has none of its functions itself: it inherits each from FiRoot, which does
not list the interface. C++ runs FiRoot's, and so must a call by name or through the interface on a
FuncIfaceInherited. An empty stub would replace FiRoot's for every caller; with no function at all, a call by name
would find IFiTell's own empty one first (UClass::FindFunctionByName looks in the interfaces before the super, Class.cpp
5281-5323). So the class gets an override of each that calls FiRoot's, as an editor override calling its parent does:
FiRoot's inline Twice expanded in it, its authority-only Auth bound. FiKid overrides two of those overrides, and its
`FiRoot::Tell(V)` and `FiRoot::Auth(V)` still run FiRoot's.
*/
class IFiTell {
public:
  UE_INTERFACE;
  int32 Tell(int32 V);
  int32 Kept(int32 V);
  void  Ping();
  int32 Twice(int32 V);
  int32 Auth(int32 V);
};

class FiRoot : public AActor {
public:
  int32 Seen = 0;
  int32 Tell(int32 V) { return V + 1; }
  [[gnu::noinline]] int32 Kept(int32 V) { return V * 10; }
  void Ping() { Seen = 9; }
  inline int32 Twice(int32 V) { return V * 2; }
  UE_AUTHORITY_ONLY int32 Auth(int32 V) { Seen = V; return V * 3; }
};

class FuncIfaceInherited : public FiRoot, public IFiTell {
public:
  int32 Other() { return 0; }
};

class FiKid : public FuncIfaceInherited {
public:
  int32 Tell(int32 V) { return FiRoot::Tell(V) + 100; }
  int32 Auth(int32 V) { return FiRoot::Auth(V) + 100; }
};
