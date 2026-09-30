#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PreloadUber");

/*
A latent call moves Wait's body into ExecuteUbergraph_PreloadUber, and Wait becomes a stub that enters it. The class
names the ubergraph (UberGraphFunction), which Link preloads and CreatePersistentUberGraphFrame needs loaded; the
stub's bytecode names it as its call target.
*/
class PreloadUber : public AActor {
  int32 Stage;

public:
  void Wait(float Seconds) {
    Stage = 1;
    UKismetSystemLibrary::Delay(Seconds);
    Stage = 2;
  }
};
