/*
AwaitPaths.cpp - UE_AWAIT on an async action, on more than the straight path.

K2Node_AsyncAction binds every dispatcher the node listens on and then calls Activate, once per action, on whichever
path reaches the node (K2Node_BaseAsyncTask.cpp 410-467): Activate may broadcast at once, and an action whose
Activate starts its work (AsyncActionLoadPrimaryAsset.cpp 6-35) runs it again on a second call. Pending: the compiler
activates the first await of each variable in the order it lowers them, not the order they run, so the else branch's
await never activates its task and an await inside a loop activates it on every round.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"
#include "UeApi/UMG.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AwaitPaths");

class AwaitPaths : public AActor {
  UTexture2DDynamic *Image;
  int32 Rounds;

public:
  /* One await per branch: whichever runs activates the task. */
  void Either(FString Url, bool bOk) {
    UAsyncTaskDownloadImage *T = UAsyncTaskDownloadImage::DownloadImage(Url);
    if (bOk)
      Image = UE_AWAIT(T->OnSuccess);
    else {
      UE_AWAIT(T->OnFail);
      Rounds = -1;
    }
  }

  /* One task awaited on each round: activated on the first only, the second waits on the action already running. */
  void Twice(FString Url) {
    UAsyncTaskDownloadImage *T = UAsyncTaskDownloadImage::DownloadImage(Url);
    for (int32 I = 0; I < 2; ++I) {
      Image = UE_AWAIT(T->OnSuccess);
      ++Rounds;
    }
  }
};
