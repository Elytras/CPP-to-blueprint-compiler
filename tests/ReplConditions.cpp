#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ReplConditions");

struct FReplPair {
  UE_STRUCT;
  int32 A;
  float B;
};

/* Every ELifetimeCondition a replicated variable can name, each cooked as its UE 4.27 value (CoreNetTypes.h 12-25), a
   COND_-prefixed name the same; a replicated struct with a RepNotify and an array of it; RPCs taking a struct and a
   string. */
class ReplConditions : public AActor {
  UE_REPLICATED_IF(int32, C0, None);
  UE_REPLICATED_IF(int32, C1, InitialOnly);
  UE_REPLICATED_IF(int32, C2, OwnerOnly);
  UE_REPLICATED_IF(int32, C3, SkipOwner);
  UE_REPLICATED_IF(int32, C4, SimulatedOnly);
  UE_REPLICATED_IF(int32, C5, AutonomousOnly);
  UE_REPLICATED_IF(int32, C6, SimulatedOrPhysics);
  UE_REPLICATED_IF(int32, C7, InitialOrOwner);
  UE_REPLICATED_IF(int32, C8, Custom);
  UE_REPLICATED_IF(int32, C9, ReplayOrOwner);
  UE_REPLICATED_IF(int32, C10, ReplayOnly);
  UE_REPLICATED_IF(int32, C11, SimulatedOnlyNoReplay);
  UE_REPLICATED_IF(int32, C12, SimulatedOrPhysicsNoReplay);
  UE_REPLICATED_IF(int32, C13, COND_SkipReplay);
  UE_REPLICATED_USING(FReplPair, Pair, OnRep_Pair);
  UE_REPLICATED(TArray<FReplPair>, Pairs);
  int32 Plain;
  int32 Notified;

public:
  void OnRep_Pair() { Notified += Pair.A; }

  UE_SERVER UE_RELIABLE void SendPair(FReplPair P) {
    Pair = P;
    Pairs.Add(P);
  }
  UE_CLIENT void Tell(int32 N, FString Why) { Plain = N; }
};
