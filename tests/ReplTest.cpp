#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ReplTest");

/* Replication: replicated variables with and without RepNotify and conditions, and the three RPC kinds. */
class ReplTest : public AActor {
  UE_REPLICATED(int32, Score) = 5;
  UE_REPLICATED_USING(bool, bOpen, OnRep_Open);
  UE_REPLICATED_IF(float, Aim, SkipOwner);
  UE_REPLICATED_USING_IF(TArray<int32>, Slots, OnRep_Slots, OwnerOnly);
  int32 Local;
  int32 Notified;

public:
  void OnRep_Open() { Notified += 1; }
  void OnRep_Slots() { Notified += 10; }

  UE_SERVER UE_RELIABLE void ServerOpen(bool bValue);
  UE_CLIENT void ClientPing(int32 Seq) { Local = Seq; }
  UE_MULTICAST void MultiBoom() { Score += 1; }
  /* A reference parameter of an RPC arrives as a copy: writing through it warns. */
  UE_SERVER void ServerBump(int32 &Count) { Count += 1; }
  UE_AUTHORITY_ONLY void AuthOnly() { Local = 3; }
  UE_COSMETIC void Pretty() { Local = 4; }

  void ReceiveBeginPlay() {
    bOpen = true;
    Slots.Add(3);
    Slots[0] = 4;
    Local = 1;
    ServerOpen(false);
    MultiBoom();
  }

  ReplTest *Other;
  /* A set on another object wakes and notifies that object; a native replicated property only wakes. */
  void SetOther() {
    Other->bOpen = false;
    Other->Local = 2;
    bReplicateMovement = true;
  }
};

void ReplTest::ServerOpen(bool bValue) { bOpen = bValue; }

/*
An override of an RPC is that RPC: it takes no marker, carries its parent's net flags, and names the parent's
function as its super (the Kismet compiler refuses anything else - a mismatch "will trigger an assert in
Link()"). Helper is an ordinary method overridden the same way.
*/
class ReplKid : public ReplTest {
  int32 Seen = 0;

public:
  void ServerOpen(bool bValue) { Seen = bValue ? 7 : 8; }
  void MultiBoom() { Seen = 9; }
  void OnRep_Open() { Seen = 10; }
};
