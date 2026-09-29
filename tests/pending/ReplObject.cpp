#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ReplObject");

/* Only an actor or an actor component replicates: UObject's GetLifetimeReplicatedProps never lists a Blueprint's
   replicated variables, and its GetFunctionCallspace / CallRemoteFunction run every RPC locally. So a replicated
   variable or an RPC on a plain UObject class does nothing, and the compiler says so. ReplComponentCtl, a component
   with the same members, replicates and gets no such word. */
class ReplObject : public UObject {
  UE_REPLICATED(int32, A);

public:
  UE_SERVER void S() { A = 1; }
};

class ReplComponentCtl : public UActorComponent {
  UE_REPLICATED(int32, A);

public:
  UE_SERVER void S() { A = 1; }
};
