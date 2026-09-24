#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/LatentTest");

/* A plain UObject can wait as well. The editor hides Delay here because a UObject does not override GetWorld, but
   the world is found through the Outer chain and UWorld::Tick resumes every object's latent actions: make the job
   with an actor as its Outer. */
class LatentJob : public UObject {
  int32 Done;

public:
  void Run(float Seconds) {
    UKismetSystemLibrary::Delay(Seconds);
    Done = 1;
  }
};

/* Latent calls: a function that makes one moves into ExecuteUbergraph_LatentTest and resumes after it. */
class LatentTest : public AActor {
  int32 Stage;
  TArray<int32> Log;

  inline void Pause(float Seconds) { UKismetSystemLibrary::Delay(Seconds); }

public:
  void ReceiveBeginPlay() {
    int32 Local = Stage + 7;    // not a constant: a folded one would need no frame slot to survive the delay
    Stage = 1;
    UKismetSystemLibrary::Delay(this, 0.5f);
    Stage = Local + 1;
    for (int32 I = 0; I < 3; ++I) {
      UKismetSystemLibrary::Delay(0.25f);
      Log.Add(I);
    }
    Wait(1.0f, 100);
  }

  /* Parameters are copied into the frame, and Tag lives across the delay. */
  void Wait(float Seconds, int32 Tag) {
    Log.Add(Tag);
    UKismetSystemLibrary::Delay(Seconds);
    Log.Add(Tag + 1);
  }

  /* Through an inline helper, and with a local that shares a name with Wait's. */
  void ViaInline() {
    int32 Tag = Stage + 3;      // as in ReceiveBeginPlay: a constant would fold into its use
    Pause(2.0f);
    Stage = Tag;
  }

  /* The completion delegate is the compiler's: the call's value is its Loaded parameter, read after the resume. */
  TSoftClassPtr<UClass> Wanted;
  TSubclassOf<UObject> Got;
  void Load() {
    auto Cls = UKismetSystemLibrary::LoadAssetClass(Wanted);
    Got = Cls;
    UObject* Obj = UKismetSystemLibrary::LoadAsset(Icon);
    Stage = UKismetSystemLibrary::IsValid(Obj) ? 1 : 0;
  }
  TSoftObjectPtr<UObject> Icon;

  void Plain() { Stage = 5; }
};
