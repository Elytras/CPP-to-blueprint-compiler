#include "UeApi/Types.h"

#include "UeApi/AnimGraphRuntime.h"
#include "UeApi/FSD.h"
#include "UeApi/UMG.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AsyncTest");

/* Async action proxies: an object whose multicast delegates fire later. */
class AsyncTest : public AActor {
  USkeletalMeshComponent* Mesh;
  UAnimMontage* Montage;
  FName Last;

public:
  void Done(FName NotifyName) { Last = NotifyName; }

  /* Callback style, as the editor's node with its exec pins wired to custom events. */
  void Play() {
    UPlayMontageCallbackProxy* Proxy = UPlayMontageCallbackProxy::CreateProxyObjectForPlayMontage(Mesh, Montage, 1.0f, 0.0f, FName());
    Proxy->OnCompleted.Add(this, &AsyncTest::Done);
    Proxy->OnInterrupted.Add(this, &AsyncTest::Done);
  }

  /* Await style: the code after UE_AWAIT runs when the dispatcher fires, its parameter as the value. */
  void PlayThen() {
    UPlayMontageCallbackProxy* Proxy = UPlayMontageCallbackProxy::CreateProxyObjectForPlayMontage(Mesh, Montage, 1.0f, 0.0f, FName());
    FName Notify = UE_AWAIT(Proxy->OnCompleted);
    Last = Notify;
  }

  /* A UBlueprintAsyncActionBase is activated after the first bind only. */
  UTexture2DDynamic* Image;
  void Download(FString Url) {
    UAsyncTaskDownloadImage* Task = UAsyncTaskDownloadImage::DownloadImage(Url);
    Image = UE_AWAIT(Task->OnSuccess);
    UE_AWAIT(Task->OnFail);
    Image = nullptr;
  }
};
