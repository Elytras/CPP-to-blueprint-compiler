/*
WaitHold.cpp - an object kept only in a waiting method's local.

A method that waits keeps its locals in the ubergraph's persistent frame, and the garbage collector reads an object
reference there as a weak one unless the object carries RF_StrongRefOnFrame (UObjectGlobals.cpp 3460-3483), which
only SpawnObject and the async proxies set. A widget made before the Delay and read after it can be collected in
between, and the local then reads None. The editor behaves the same; the compiler knows which locals live across a
wait, so it warns at F. The rest are controls it must not warn at: a widget read out of a member (its owner keeps
it), one made after the wait, an object SpawnObject made (RF_StrongRefOnFrame), and an actor (its level keeps it).
*/
#include "../include/Objects.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/WaitHold");

class WaitHold : public AActor {
  UUserWidget *Kept;
  UObject *Made;
  AActor *Near;

public:
  void F(APlayerController *P) {
    UUserWidget *W = CreateWidget<UUserWidget>(P, UUserWidget::StaticClass());
    UKismetSystemLibrary::Delay(1.0f);
    Kept = W;
  }

  void FromMember() {
    UUserWidget *Held = Kept;
    UKismetSystemLibrary::Delay(1.0f);
    Kept = Held;
  }

  void AfterWait(APlayerController *P) {
    UKismetSystemLibrary::Delay(1.0f);
    UUserWidget *Late = CreateWidget<UUserWidget>(P, UUserWidget::StaticClass());
    Kept = Late;
  }

  void Spawned() {
    UObject *O = NewObject<UObject>(this);
    UKismetSystemLibrary::Delay(1.0f);
    Made = O;
  }

  void FindNear() {
    AActor *A = UGameplayStatics::GetActorOfClass(this, AActor::StaticClass());
    UKismetSystemLibrary::Delay(1.0f);
    Near = A;
  }
};
