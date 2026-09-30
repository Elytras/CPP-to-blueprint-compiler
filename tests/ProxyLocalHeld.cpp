/*
ProxyLocalHeld.cpp - a montage callback proxy bound in a plain function and kept in its local.

CreateProxyObjectForPlayMontage makes the proxy in the transient package and sets RF_StrongRefOnFrame
(PlayMontageCallbackProxy.cpp 15-23): the persistent ubergraph frame that holds it is what keeps it alive, and the
anim instance's delegates reach it only weakly. A plain function's locals die with its call, so once Play returns
nothing would reference the proxy, a garbage collection would take it, and Done would never run. The editor cannot
build this: its Play Montage node is an async-task node, placed in event graphs only (K2Node_BaseAsyncTask.cpp 54-64).
The compiler stores the local into a transient member of the class too, where the collector sees it.
*/
#include "UeApi/Types.h"

#include "UeApi/AnimGraphRuntime.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ProxyLocalHeld");

class ProxyLocalHeld : public AActor {
  USkeletalMeshComponent *Mesh;
  UAnimMontage *Montage;
  FName Last;

public:
  void Done(FName NotifyName) { Last = NotifyName; }

  void Play() {
    UPlayMontageCallbackProxy *Proxy =
        UPlayMontageCallbackProxy::CreateProxyObjectForPlayMontage(Mesh, Montage, 1.0f, 0.0f, FName());
    Proxy->OnCompleted.Add(this, &ProxyLocalHeld::Done);
  }
};
