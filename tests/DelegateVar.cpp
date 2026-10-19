/* DelegateVar: a single-cast delegate kept in a variable - a class variable and a local - then handed to a timer.
   Each is a DelegateProperty whose tail names its SignatureFunction, a *__DelegateSignature function of the class
   with FUNC_Delegate (PropertyDelegate.cpp 161-175), and what reaches K2_SetTimerDelegate is OnTimer bound on this
   object. */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DelegateVar");

class DelegateVar : public AActor {
public:
  int32 Fired = 0;
  TDelegate<void()> Stored;

  void OnTimer() { Fired += 1; }
  void Arm() {
    Stored = {this, &DelegateVar::OnTimer};
    UKismetSystemLibrary::K2_SetTimerDelegate(Stored, 0.5f, false, 0.0f, 0.0f);
  }
  void ArmLocal() {
    TDelegate<void()> Local = {this, &DelegateVar::OnTimer};
    UKismetSystemLibrary::K2_SetTimerDelegate(Local, 2.0f, true, 0.0f, 0.0f);
  }

  /* A dispatcher takes a delegate value as it is: the editor's Add / Remove node fed a delegate pin. */
  UE_DISPATCHER(OnHit, int32 Points);
  int32 Got = 0;
  TDelegate<void(int32 Points)> Held;
  void Take(int32 Points) { Got += Points; }
  void Hook() {
    Held = {this, &DelegateVar::Take};
    OnHit.Add(Held);
  }
  void Unhook() { OnHit.Remove(Held); }
  void Fire(int32 Points) { OnHit.Broadcast(Points); }

  /* A native dispatcher's handler may take a parameter by const reference where UeApi spells it by value. */
  UE_COMPONENT(UBoxComponent, Box);
  void Bumped(UPrimitiveComponent *Hit, AActor *Other, UPrimitiveComponent *OtherComp, FVector Impulse,
              const FHitResult &Result) {}
  void HookBox() { Box->OnComponentHit.Add(this, &DelegateVar::Bumped); }
};
