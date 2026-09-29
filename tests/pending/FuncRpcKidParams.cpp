#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncRpcKidParams");

/*
An override of a mod parent's RPC with another parameter list. The override is the RPC (it takes the parent's net
flags and names it as super), and a received RPC is read with the topmost function's layout into a buffer of the
override's ParmsSize (DataReplication.cpp:1218-1235): V arrives as an int32's bits in a float, W past the buffer the
parent's layout filled. The compiler must refuse it, naming ServerNudge, or give the override the parent's parameter
block.
*/
class RpcKidParamsBase : public AActor {
public:
  UE_SERVER void ServerNudge(int32 V) {}
};

class FuncRpcKidParams : public RpcKidParamsBase {
  int32 Got = 0;

public:
  void ServerNudge(float V, int32 W) { Got = W; }
};
