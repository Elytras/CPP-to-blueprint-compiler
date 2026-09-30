/*
AwaitPaths.cpp - UE_AWAIT on an async action, on more than the straight path.

K2Node_AsyncAction binds every dispatcher the node listens on and then calls Activate, once per action, on whichever
path reaches the node (K2Node_BaseAsyncTask.cpp 410-467): Activate may broadcast at once, and an action whose
Activate starts its work (AsyncActionLoadPrimaryAsset.cpp 6-35) runs it again on a second call. The compiler places
each await's Activate by the order the awaits run, not the order it lowers them: the else branch's await activates its
task as the then branch's does, and an await inside a loop activates it on the first round only (the loop's first
round runs as a copy before it, whose await resumes in the loop).
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

  /* The same with the loop's test a `break` before the await: the first round's break leaves without waiting. */
  void Until(FString Url) {
    UAsyncTaskDownloadImage *T = UAsyncTaskDownloadImage::DownloadImage(Url);
    while (true) {
      if (Rounds >= 2)
        break;
      Image = UE_AWAIT(T->OnSuccess);
      ++Rounds;
    }
  }
};
