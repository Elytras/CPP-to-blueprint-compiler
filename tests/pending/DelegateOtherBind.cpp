/* DelegateOtherBind: a delegate bound to a function of an object other than `this` - Add and Remove on a dispatcher, and
   a TDelegate value handed to a timer - as the editor's Create Event node with its Object pin wired: EX_BindDelegate
   <name> <a delegate local> <the object> (KismetCompilerVMBackend.cpp 1609-1625), then that local. The engine finds the
   name on the object's class when the broadcast or the timer runs. The object is another class's (DelegateOtherTarget)
   or this class's other instance (Peer), and may be a member of another object (Peer->Peer). AssetGen refuses each
   today ("a delegate can only bind a function of `this`"). */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DelegateOtherBind");

class DelegateOtherTarget : public AActor {
public:
  int32 Got = 0;
  void Take(int32 Points) { Got += Points; }
};

class DelegateOtherBind : public AActor {
public:
  UE_DISPATCHER(OnScore, int32 Points);
  DelegateOtherTarget *T = nullptr;
  DelegateOtherBind *Peer = nullptr;
  int32 Got = 0;

  void Mine(int32 Points) { Got += Points * 10; }
  void Ping() { Got += 100; }
  void BindTarget() { OnScore.Add(T, &DelegateOtherTarget::Take); }
  void BindPeer() { OnScore.Add(Peer, &DelegateOtherBind::Mine); }
  void UnbindPeer() { OnScore.Remove(Peer, &DelegateOtherBind::Mine); }
  void BindPeersPeer() { Peer->OnScore.Add(Peer->Peer, &DelegateOtherBind::Mine); }
  void Fire(int32 P) { OnScore.Broadcast(P); }
  void ArmPeer() { UKismetSystemLibrary::K2_SetTimerDelegate({Peer, &DelegateOtherBind::Ping}, 1.0f, false, 0.0f, 0.0f); }
};
