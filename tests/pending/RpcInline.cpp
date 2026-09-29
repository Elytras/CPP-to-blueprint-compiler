#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/RpcInline");

/* An inline method is expanded where it is called, so the engine never sees a call to route: a net, authority-only
   or cosmetic marker on one is refused, warned about, or kept by cooking the method as a function the call reaches
   through the net routing. */
class RpcInline : public AActor {
  int32 Pings;

public:
  UE_SERVER inline void ServerPing() { Pings += 1; }
  UE_AUTHORITY_ONLY inline void Auth() { Pings = 5; }
  UE_COSMETIC inline void Cos() { Pings = 7; }
  void Go() {
    ServerPing();
    Auth();
    Cos();
  }
};
