/* DispatchOtherFire: Broadcast on a dispatcher another class declares, reached through a pointer: a sibling mod
   class's UE_DISPATCHER and a game Blueprint's (BP_BurrowComponent_C::OnBurrowComplete). The editor's Call node has a
   Target pin and takes any Blueprint dispatcher, every one being BlueprintCallable: its EX_CallMulticastDelegate names
   the declaring class's own <Name>__DelegateSignature, imported, over the other object's variable. AssetGen refuses it
   today ("Broadcast needs the dispatcher's signature function, which only a UE_DISPATCHER of this class or a Blueprint
   parent has"). */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"
#include "UeApi/Game/BP_BurrowComponent_C.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DispatchOtherFire");

class DispatchOtherTarget : public AActor {
public:
  UE_DISPATCHER(OnHit, int32 Points, FName Tag);
  int32 Got = 0;
};

class DispatchOtherFire : public AActor {
public:
  DispatchOtherTarget *T = nullptr;
  BP_BurrowComponent_C *Burrow = nullptr;
  int32 Got = 0;
  FName LastTag;
  bool Emerged = false;

  void Mine(int32 Points, FName Tag) {
    Got += Points * 10;
    LastTag = Tag;
  }
  void Dug(bool IsEmerging) { Emerged = IsEmerging; }
  void Hook() {
    T->OnHit.Add(this, &DispatchOtherFire::Mine);
    Burrow->OnBurrowComplete.Add(this, &DispatchOtherFire::Dug);
  }
  void Fire(int32 P) { T->OnHit.Broadcast(P, "far"); }
  void FireBurrow() { Burrow->OnBurrowComplete.Broadcast(true); }
};
