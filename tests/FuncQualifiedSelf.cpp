#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncQualifiedSelf");

/*
`FuncQualifiedSelf::H()` in FuncQualifiedSelf's own code runs its H in C++, also on an SqKid, which overrides H. The
editor binds no call to a function a subclass can override except a parent call, so H's body is copied in; Auth's
cannot be (authority-only), so that call stays a call by name, and says so.
*/
class FuncQualifiedSelf : public AActor {
public:
  int32 Seen = 0;
  int32 H() { return 5; }
  UE_AUTHORITY_ONLY void Auth() { Seen = 3; }
  int32 CallH() { return FuncQualifiedSelf::H(); }
  void  CallAuth() { FuncQualifiedSelf::Auth(); }
};

class SqKid : public FuncQualifiedSelf {
public:
  int32 H() { return 50; }
  void  Auth() { Seen = 30; }
};
