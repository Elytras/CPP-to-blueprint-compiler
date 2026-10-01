#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompTickComponent");

/*
A component Blueprint that ticks. Its tick function is PrimaryComponentTick, registered only when bCanEverTick is set
(UActorComponent::SetupActorComponentTickFunction), which UActorComponent leaves false; the Blueprint compiler sets it
for a class that overrides ReceiveTick. An actor's PrimaryActorTick is no property of a component.
*/
class CompTickComponent : public UActorComponent {
public:
  int32 N;

  void ReceiveTick(float DeltaSeconds) { N += 1; }
};
