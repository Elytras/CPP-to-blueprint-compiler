// An RPC the engine marks BlueprintCallable is one the editor's Create Event node binds (FunctionCanBeUsedInDelegate
// asks only for BlueprintCallable, not pure, not latent): APlayerController::ClientClearCameraLensEffects (NetClient)
// on another object and AFSDPlayerController::Server_ResetHUD (NetServer) on this one (test_bytecode.py
// delegate_callable_rpc).
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DelegateCallableRpc");

class DelegateCallableRpc : public AActor
{
public:
    UE_DISPATCHER(OnPing);
    APlayerController *PC = nullptr;

    void Bind() { OnPing.Add(PC, &APlayerController::ClientClearCameraLensEffects); }
    void Fire() { OnPing.Broadcast(); }
};

class DelegateCallableRpcPc : public AFSDPlayerController
{
public:
    UE_DISPATCHER(OnReset);

    void Bind() { OnReset.Add(this, &AFSDPlayerController::Server_ResetHUD); }
};
