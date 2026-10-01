/*
AwaitNullProxy.cpp - UE_AWAIT on an async action whose factory hands back None.

The editor's async node calls the factory, tests the proxy with UKismetSystemLibrary::IsValid, and binds the
dispatchers and calls Activate only when it is valid; the invalid branch goes straight on
(K2Node_BaseAsyncTask.cpp 393-408, 440-448). Without that test a None proxy turns every bind and the Activate into an
'Accessed None' script warning (ScriptCore.cpp 2904-2937; execAddMulticastDelegate binds nothing, 3085-3101). The
method waits forever either way. UE_AWAIT tests the object the same way before its bind and its Activate.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"
#include "UeApi/UMG.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AwaitNullProxy");

class AwaitNullProxy : public AActor {
  UTexture2DDynamic *Image;

public:
  void Download(FString Url) {
    UAsyncTaskDownloadImage *Task = UAsyncTaskDownloadImage::DownloadImage(Url);
    Image = UE_AWAIT(Task->OnSuccess);
  }
};
