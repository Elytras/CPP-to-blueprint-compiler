#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SelfRefImport");

/*
A package's own objects are exports; it never imports itself. The event-driven loader check()s that no import's
package is the one being loaded (AsyncLoading.cpp 1986-1989, 2075-2078), and in Shipping a self import resolves back
to the package's own export with extra arcs between the two. A member typed as the mod's own class, and a call to its
own function on another instance, both name this package's objects. Pending: both go through an import of
/Game/_ElytrasMods/SelfRefImport itself.
*/
class SelfRefImport : public AActor {
public:
  SelfRefImport *Peer = nullptr;
  int32 Base = 1;

  int32 Get() { return Base; }
  int32 Ask() { return Peer ? Peer->Get() : 0; }
};
