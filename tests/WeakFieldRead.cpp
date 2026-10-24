/* WeakFieldRead: a native struct's TWeakObjectPtr member (FDamageData's Instigator and DamageCauser, FDamageDataBase's
   Component) reads like an object reference, as a Break node reads one: EX_StructMemberContext of the member, whose
   read hands out the object. Take calls a function on one, stores one in an object variable and tests one. A class's
   weak member (AActor's ParentComponent, USceneComponent's PhysicsVolume) reads the same way, through
   EX_InstanceVariable in a context: Owned does the same three things with those. */
#include "UeApi/Types.h"
#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/WeakFieldRead");

class WeakFieldRead : public AActor {
public:
  APawn *Pawn = nullptr;
  AActor *Causer = nullptr;
  bool Had = false;
  void Take(FDamageData Data) {
    Pawn = Data.Instigator->K2_GetPawn();
    Causer = Data.DamageCauser;
    if (Data.Component) Had = true;
  }
  UChildActorComponent *Parent = nullptr;
  bool InVolume = false;
  void Owned(AActor *Other, USceneComponent *Comp) {
    Causer = Other->ParentComponent->GetOwner();
    Parent = Other->ParentComponent;
    if (Comp->PhysicsVolume) InVolume = true;
  }
};
