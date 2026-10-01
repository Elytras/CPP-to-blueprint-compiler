#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncIfaceUnnamed");

/*
FiuRoot's Tell and Kept leave a parameter unnamed. FuncIfaceUnnamed implements IFiuTell, whose Tell it inherits from
FiuRoot, and calls `FiuRoot::Kept`, which is noinline: each needs an override in FuncIfaceUnnamed that calls FiuRoot's,
as an editor override calling its parent does, and the override names the parameter to pass it on. Tell's unnamed
parameter comes before one named P0, so the override names it P0_; were it dropped, P0 would arrive in its place.
*/
class IFiuTell {
public:
  UE_INTERFACE;
  int32 Tell(int32 A, int32 B);
};

class FiuRoot : public AActor {
public:
  int32 Seen = 0;
  int32 Tell(int32, int32 P0) { return P0 * 10; }
  [[gnu::noinline]] int32 Kept(int32, int32 B) {
    Seen = B;
    return B * 7;
  }
};

class FuncIfaceUnnamed : public FiuRoot, public IFiuTell {
public:
  int32 Use() { return FiuRoot::Kept(1, 2); }
};
