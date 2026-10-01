#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompTick");

/*
Ticking. An actor registers its tick function only when its CDO's PrimaryActorTick.bCanEverTick is set, which AActor
leaves false; the Blueprint compiler sets it for a class that overrides ReceiveTick (KismetCompiler.cpp,
SetCanEverTick). So CompTick's ReceiveTick runs every frame, and TickLess, which has none, never ticks.
*/
class CompTick : public AActor {
public:
  int32 Frames = 0;

  void ReceiveTick(float DeltaSeconds) { Frames = Frames + 1; }
};

class TickLess : public AActor {
public:
  int32 Frames = 0;

  void ReceiveBeginPlay() { Frames = 0; }
};
