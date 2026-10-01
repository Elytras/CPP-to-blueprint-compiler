/* DispatchInheritedFire: a child class broadcasts the dispatcher its mod parent declares. The engine needs an
   EX_CallMulticastDelegate naming the parent's OnHit__DelegateSignature (an import) over the inherited variable, as
   an editor child Blueprint's Call node does; AssetGen refuses it today ("Broadcast needs the dispatcher's signature
   function, which only a UE_DISPATCHER of this class has"). */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DispatchInheritedFire");

class DispatchInheritedBase : public AActor {
public:
  UE_DISPATCHER(OnHit, int32 Points);
  int32 Got = 0;
};

class DispatchInheritedFire : public DispatchInheritedBase {
public:
  void KidHandle(int32 Points) { Got += Points * 10; }
  void Hook() { OnHit.Add(this, &DispatchInheritedFire::KidHandle); }
  void Fire(int32 P) { OnHit.Broadcast(P); }
};
