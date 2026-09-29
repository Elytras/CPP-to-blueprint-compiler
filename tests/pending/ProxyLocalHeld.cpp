/*
ProxyLocalHeld.cpp - a montage callback proxy bound in a plain function and kept in its local.

CreateProxyObjectForPlayMontage makes the proxy in the transient package and sets RF_StrongRefOnFrame
(PlayMontageCallbackProxy.cpp 15-23): the persistent ubergraph frame that holds it is what keeps it alive, and the
anim instance's delegates reach it only weakly. A plain function's locals die with its call, so once Play returns
nothing references the proxy, a garbage collection takes it, and Done never runs. The editor cannot build this: its
Play Montage node is an async-task node, placed in event graphs only (K2Node_BaseAsyncTask.cpp 54-64). Pending: the
compiler keeps the proxy in the function's local without a word; it should keep it where the collector sees it (a
member, the ubergraph's frame) or warn at Play.
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
