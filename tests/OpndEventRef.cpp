#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/OpndEventRef");

/*
An override of a native event whose parameter is a const reference (AActor::ReceiveHit's `const FHitResult& Hit`,
CPF_OutParm | CPF_ReferenceParm), with no latent call in it. The engine calls it through ProcessEvent, which hands
out parameters only through Stack.OutParms (FUNC_HasOutParms, ScriptCore.cpp 1968-2003); a script caller gives it only
an FOutParmRec at its argument, never a frame slot (865-890). Either way the body must read Hit through
EX_LocalOutVariable: a frame read would see the zeroed slot when a script calls it. Poke is that script caller.
*/
class OpndEventRef : public AActor {
public:
  float HitTime;
  float HitDistance;
  float Seen;

  void ReceiveHit(class UPrimitiveComponent *MyComp, class AActor *Other, class UPrimitiveComponent *OtherComp,
                  bool bSelfMoved, FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult &Hit) {
    HitTime = Hit.Time;
    HitDistance = Hit.Distance;
  }

  float Poke(float T) {
    FHitResult H;
    H.Time = T;
    H.Distance = T * 2.0f;
    ReceiveHit(nullptr, nullptr, nullptr, false, FVector(), FVector(), FVector(), H);
    Seen = HitTime + HitDistance;
    return Seen;
  }
};
