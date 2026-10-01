#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncFinalInherited");

/*
A `final` class has no subclass, so a call by name on one of its objects reaches the one version of a function its chain
has: FfBase's AuthOnly, ServerBump and Kept for FuncFinalInherited, FfOwn's own AuthOnly for FfOwn. None of them is
FUNC_Final (an ancestor's is an ordinary function, an override never is), and the editor binds a call to a function
without FUNC_Final only as a call to a parent's function from an override of it: these are calls by name, which find the
same function. Their bodies are not copied in (authority-only, an RPC, noinline). FfOther calls through a pointer whose
type is the final class.
*/
class FfBase : public AActor {
public:
  int32 Seen = 0;
  UE_AUTHORITY_ONLY void AuthOnly() { Seen = 3; }
  UE_SERVER void ServerBump(int32 By) { Seen = Seen + By; }
  [[gnu::noinline]] int32 Kept(int32 V) { return V + 1; }
};

class FuncFinalInherited final : public FfBase {
public:
  void  CallAuth() { AuthOnly(); }
  void  CallServer() { ServerBump(2); }
  int32 CallKept() { return Kept(1); }
};

class FfOwn final : public FfBase {
public:
  void AuthOnly() { Seen = 30; }
  void CallAuth() { AuthOnly(); }
};

class FfOther : public AActor {
public:
  FuncFinalInherited* Other = nullptr;
  void Poke() {
    if (Other) Other->AuthOnly();
  }
};
