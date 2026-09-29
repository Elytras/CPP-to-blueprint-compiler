#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/RpcStatic");

/* A static function goes through GetGlobalFunctionCallspace, which never answers Remote: a static RPC is never sent.
   It is refused, or cooked as an RPC the engine routes (not static, called through the net routing). */
class RpcStatic : public AActor {
public:
  UE_SERVER static void S(int32 X) {}
  void Call() { S(3); }
};
