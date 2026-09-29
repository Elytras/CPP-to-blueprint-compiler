#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/OpndLatentHit");

/*
A latent call in an override of a native event that takes a const reference (AActor::ReceiveHit's `const FHitResult&
Hit`, CPF_OutParm | CPF_ReferenceParm). The editor's event stub copies such a parameter into the ubergraph's frame
through EX_LocalOutVariable - a script caller hands it only an out-parameter record, never a frame slot (ScriptCore.cpp
865-890) - and the game's own stubs all do (BP_FireCracker's ReceiveHit, every ComponentHitSignature event). A const
reference is only read, so the copy is the value; the body after the Delay reads it from the frame.
*/
class OpndLatentHit : public AActor {
public:
  float HitTime;
  float HitDistance;

  void ReceiveHit(class UPrimitiveComponent *MyComp, class AActor *Other, class UPrimitiveComponent *OtherComp,
                  bool bSelfMoved, FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult &Hit) {
    HitTime = Hit.Time;
    UKismetSystemLibrary::Delay(0.5f);
    HitDistance = Hit.Distance;
  }
};
