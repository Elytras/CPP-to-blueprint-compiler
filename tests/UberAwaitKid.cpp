/*
UberAwaitKid.cpp - a child overriding a method that awaits, which awaits too.

UberAwaitBase::Fetch binds its task's OnSuccess to a completion event the compiler generates, by name, on self. The
object is an UberAwaitKid, and a name is found on the most derived class first (UObject::FindFunction), so the name
must not also be one of the child's functions: the child's own await generates one for its Fetch too. Otherwise the
parent's download resumes in the child's ubergraph, at the child's offset, and the parent's code after its await
never runs.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"
#include "UeApi/UMG.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/UberAwaitKid");

class UberAwaitBase : public AActor {
public:
  UTexture2DDynamic *Image;
  int32 Base;
  virtual void Fetch(FString Url) {
    UAsyncTaskDownloadImage *T = UAsyncTaskDownloadImage::DownloadImage(Url);
    Image = UE_AWAIT(T->OnSuccess);
    Base = 1;
  }
};

class UberAwaitKid : public UberAwaitBase {
public:
  int32 Kid;
  void Fetch(FString Url) override {
    UberAwaitBase::Fetch(Url);
    UAsyncTaskDownloadImage *T = UAsyncTaskDownloadImage::DownloadImage(Url);
    UE_AWAIT(T->OnSuccess);
    Kid = 1;
  }
};
