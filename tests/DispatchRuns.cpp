/* DispatchRuns: event dispatchers as the VM runs them. Add binds a method of `this` by name (once per object and name,
   as AddUnique does), Remove takes one binding off, Clear takes them all, and Broadcast runs every bound handler with
   its arguments - a struct by value and by const reference among them - on the object the binding holds. A binding
   on another object's dispatcher runs when that object broadcasts. DispatchKid binds its parent's handler and its own
   to the parent's dispatchers: the names resolve up the class chain. */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DispatchRuns");

struct FDispatchNote {
  UE_STRUCT;
  int32 Points;
  FName Tag;
};

class DispatchRuns : public AActor {
public:
  UE_DISPATCHER(OnScore, int32 Points, AActor *By);
  UE_DISPATCHER(OnNote, FDispatchNote Note, const FDispatchNote &Seen);
  UE_DISPATCHER(OnPing);
  int32 Total = 0;
  int32 Pings = 0;
  FName LastTag;
  AActor *LastBy = nullptr;
  DispatchRuns *Other = nullptr;

  void HandleScore(int32 Points, AActor *By) {
    Total += Points;
    LastBy = By;
  }
  void HandleDouble(int32 Points, AActor *By) { Total += Points * 2; }
  void HandleNote(FDispatchNote Note, const FDispatchNote &Seen) {
    Total += Note.Points * 100 + Seen.Points;
    LastTag = Note.Tag;
  }
  void HandlePing() { Pings += 1; }

  void AddBoth() {
    OnScore.Add(this, &DispatchRuns::HandleScore);
    OnScore.Add(this, &DispatchRuns::HandleDouble);
  }
  void AddTwice() {
    OnScore.Add(this, &DispatchRuns::HandleScore);
    OnScore.Add(this, &DispatchRuns::HandleScore);
  }
  void RemoveScore() { OnScore.Remove(this, &DispatchRuns::HandleScore); }
  void ClearScore() { OnScore.Clear(); }
  void Fire(int32 P) { OnScore.Broadcast(P, this); }
  void FireNote(int32 P) {
    FDispatchNote N;
    N.Points = P;
    N.Tag = "hit";
    OnNote.Add(this, &DispatchRuns::HandleNote);
    OnNote.Broadcast(N, N);
  }
  void PingTwice() {
    OnPing.Add(this, &DispatchRuns::HandlePing);
    OnPing.Broadcast();
    OnPing.Broadcast();
  }
  void FireEmpty() { OnPing.Broadcast(); }
  void HookOther() { Other->OnScore.Add(this, &DispatchRuns::HandleScore); }
  void FireOther(int32 P) { Other->OnScore.Broadcast(P, this); }

  /* Timers: by event (a delegate value) and by name (a zero-parameter function of the class). */
  void Tock() { Pings += 100; }
  void ArmByEvent() { UKismetSystemLibrary::K2_SetTimerDelegate({this, &DispatchRuns::Tock}, 0.5f, true, 0.0f, 0.0f); }
  void ArmByName() { UKismetSystemLibrary::K2_SetTimer(this, "Tock", 1.0f, false, 0.0f, 0.0f); }
};

class DispatchKid : public DispatchRuns {
public:
  void KidPing() { Pings += 10; }
  void Hook() {
    OnScore.Add(this, &DispatchRuns::HandleDouble);
    OnPing.Add(this, &DispatchKid::KidPing);
  }
};
