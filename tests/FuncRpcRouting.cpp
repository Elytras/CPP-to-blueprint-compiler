#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncRpcRouting");

/*
Calls that must keep the engine's net routing. A server RPC, an authority-only, a cosmetic and a multicast function of a
mod parent, reached from a child: unqualified (a call by name, the most derived definition), and the RPC the child
overrides once more through `RpcRouteBase::ServerOpen`, which is the parent's function and still an RPC. Every one of
these calls goes through CallFunction's callspace check (EX_VirtualFunction / EX_FinalFunction); a Local* call or
EX_CallMath would run it on the calling machine.
*/
class RpcRouteBase : public AActor {
public:
  int32 Opened = 0;
  UE_SERVER UE_RELIABLE void ServerOpen(bool bValue) { Opened = bValue ? 1 : 2; }
  UE_AUTHORITY_ONLY void AuthOnly() { Opened = Opened + 10; }
  UE_COSMETIC void Pretty() { Opened = Opened + 100; }
  UE_MULTICAST void MultiPing(int32 V) { Opened = Opened + V; }
};

class FuncRpcRouting : public RpcRouteBase {
public:
  void ServerOpen(bool bValue) { Opened = 7; }
  void OpenViaParent() { RpcRouteBase::ServerOpen(true); }
  void Inherited() {
    AuthOnly();
    Pretty();
    MultiPing(4);
  }
  void OwnRpc() { ServerOpen(false); }
};
