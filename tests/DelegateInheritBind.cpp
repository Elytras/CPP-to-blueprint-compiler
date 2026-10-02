/* DelegateInheritBind: a function of another object bound to a dispatcher this class inherits - a mod parent's
   (DelegateInheritParent's OnHit) and a game Blueprint parent's (BP_BurrowComponent_C's OnBurrowComplete). The editor's
   Create Event node types its delegate with the dispatcher's own signature, the parent's <Name>__DelegateSignature,
   which this class imports; a function of its own by that name would hide the parent's in FindFunctionByName and carry
   no link to it (invariants.py func_super_link). The same holds for a TDelegate value handed to a timer in a parent and
   in its child: each class makes a signature function for it, and the child's must not take the parent's name. */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"
#include "UeApi/Game/BP_BurrowComponent_C.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DelegateInheritBind");

class DelegateInheritHelper : public AActor {
public:
  int32 Got = 0;
  void Take(int32 Points) { Got += Points; }
  void Dug(bool IsEmerging) { Got += 1000; }
  void Ping() { Got += 100; }
};

class DelegateInheritParent : public AActor {
public:
  UE_DISPATCHER(OnHit, int32 Points);
  DelegateInheritHelper *H = nullptr;
  void ArmParent() { UKismetSystemLibrary::K2_SetTimerDelegate({H, &DelegateInheritHelper::Ping}, 1.0f, false, 0.0f, 0.0f); }
};

class DelegateInheritBind : public DelegateInheritParent {
public:
  void Bind() { OnHit.Add(H, &DelegateInheritHelper::Take); }
  void Unbind() { OnHit.Remove(H, &DelegateInheritHelper::Take); }
  void Fire(int32 P) { OnHit.Broadcast(P); }
  void ArmChild() { UKismetSystemLibrary::K2_SetTimerDelegate({H, &DelegateInheritHelper::Ping}, 2.0f, false, 0.0f, 0.0f); }
};

class DelegateInheritBurrow : public BP_BurrowComponent_C {
public:
  DelegateInheritHelper *H = nullptr;
  void Bind() { OnBurrowComplete.Add(H, &DelegateInheritHelper::Dug); }
  void Fire() { OnBurrowComplete.Broadcast(true); }
};
