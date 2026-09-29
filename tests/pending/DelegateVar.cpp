/* DelegateVar: a single-cast delegate kept in a variable - a class variable and a local - then handed to a timer.
   Each is a DelegateProperty whose tail names its SignatureFunction (Script.cpp: "unimplemented tail -
   DelegateProperty"), and what reaches K2_SetTimerDelegate is OnTimer bound on this object. */
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
};
