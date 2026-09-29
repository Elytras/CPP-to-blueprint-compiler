#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/NoWorldCtx");

/*
A world-context call left to its default. AssetGen passes self, as the editor wires the hidden WorldContext pin to
self - but the editor does that only when the Blueprint's class implements GetWorld, and otherwise refuses with "Pin
must have a connection" (CallFunctionHandler.cpp 547-598). A plain UObject finds a world only through its Outer, so
GetPlayerPawn(self) returns None unless it was made inside one: the compile should say so, as it already does for a
latent call in such a class. The actor below has a world of its own and must stay silent.
*/
class NoWorldCtx : public UObject {
public:
  APawn *Pawn;
  void Run() { Pawn = UGameplayStatics::GetPlayerPawn(0); }
};

class NoWorldCtxActor : public AActor {
public:
  APawn *Pawn;
  void Run() { Pawn = UGameplayStatics::GetPlayerPawn(0); }
};
