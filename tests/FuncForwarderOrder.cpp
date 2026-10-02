#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncForwarderOrder");

/*
FoMid's `FoRoot::Auth()` gives FoMid a forwarding override of Auth, and each kid's gives it one too. A kid's is an
override of FoMid's, its super, and calls that one, as the editor's call to the parent function does - whatever the
kid's name, which decides only where it sorts: AaFoKid sorts before FoMid, ZzFoKid after.
*/
class FoRoot : public AActor {
public:
  int32 Seen = 0;
  UE_AUTHORITY_ONLY void Auth() { Seen = 3; }
};

class FoMid : public FoRoot {
public:
  void MidCall() { FoRoot::Auth(); }
};

class AaFoKid : public FoMid {
public:
  void KidCall() { FoRoot::Auth(); }
};

class ZzFoKid : public FoMid {
public:
  void KidCall() { FoRoot::Auth(); }
};
