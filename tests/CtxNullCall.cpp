#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CtxNullCall");

/*
A call through another object, used as a value. On a None (or pending-kill) object ProcessContextOpcode skips the
call and clears the context's r-value property in the result slot (ScriptCore.cpp 2940-2953); the editor names the
Let destination there (KismetCompilerVMBackend.cpp 1241-1244), so the destination reads zero. With no r-value it is
left holding its previous value: Got stays 7, and ReadLocal returns whatever its caller's slot held.
*/
class CtxNullCall : public AActor {
public:
  CtxNullCall *Peer;
  int32 Got;

  [[gnu::noinline]] int32 Five() { return 5; }
  void Read() { Got = Peer->Five(); }
  int32 ReadLocal(CtxNullCall *P) {
    int32 N = 7;
    N = P->Five();
    return N;
  }
};
