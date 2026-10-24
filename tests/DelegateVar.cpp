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

  /* Execute calls the value it is given: a parameter, a variable, a map's value; an unbound one does nothing, and two
     calls in a row run the second value alone. Down calls itself through a delegate until N is 0. */
  TMap<FString, TDelegate<void(int32 Points)>> Handlers;
  void Run(TDelegate<void(int32 Points)> D, int32 Points) { D.Execute(Points); }
  void RunHeld(int32 Points) { Held.ExecuteIfBound(Points); }
  void Register() {
    Held = {this, &DelegateVar::Take};
    Handlers.Add("take", Held);
  }
  void RunKey(FString Key, int32 Points) {
    TDelegate<void(int32 Points)> D;
    if (Handlers.Find(Key, D))
      D.Execute(Points);
  }
  void RunUnbound() {
    TDelegate<void(int32 Points)> D;
    D.Execute(9);
  }
  void Double(int32 Points) { Got += 2 * Points; }
  void RunTwo() {
    TDelegate<void(int32 Points)> A = {this, &DelegateVar::Take};
    TDelegate<void(int32 Points)> B = {this, &DelegateVar::Double};
    A.Execute(1);
    B.Execute(10);
  }
  TDelegate<void(int32 N)> Again;
  void Down(int32 N) {
    Got += 1;
    if (N > 0)
      Again.Execute(N - 1);
  }
  void RunDown(int32 N) {
    Again = {this, &DelegateVar::Down};
    Again.Execute(N);
  }

  /* A native dispatcher's handler may take a parameter by const reference where UeApi spells it by value. */
  UE_COMPONENT(UBoxComponent, Box);
  void Bumped(UPrimitiveComponent *Hit, AActor *Other, UPrimitiveComponent *OtherComp, FVector Impulse,
              const FHitResult &Result) {}
  void HookBox() { Box->OnComponentHit.Add(this, &DelegateVar::Bumped); }
};
