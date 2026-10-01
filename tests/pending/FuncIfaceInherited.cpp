#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncIfaceInherited");

/*
FuncIfaceInherited implements IFiTell and has none of its functions itself: it inherits each from FiRoot, which does
not list the interface. C++ runs FiRoot's, and so must a call by name or through the interface on a
FuncIfaceInherited. An empty stub would replace FiRoot's for every caller; with no function at all, a call by name
would find IFiTell's own empty one first (UClass::FindFunctionByName looks in the interfaces before the super, Class.cpp
5281-5323). So the class gets an override of each that calls FiRoot's, as an editor override calling its parent does.
*/
class IFiTell {
public:
  UE_INTERFACE;
  int32 Tell(int32 V);
  int32 Kept(int32 V);
  void  Ping();
};

class FiRoot : public AActor {
public:
  int32 Seen = 0;
  int32 Tell(int32 V) { return V + 1; }
  [[gnu::noinline]] int32 Kept(int32 V) { return V * 10; }
  void Ping() { Seen = 9; }
};

class FuncIfaceInherited : public FiRoot, public IFiTell {
public:
  int32 Other() { return 0; }
};
