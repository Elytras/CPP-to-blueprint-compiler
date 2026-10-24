#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CovariantContainers");

/*
A container of objects passes where one of a base class is wanted, as the editor links an Actor array pin to an Object
array one (ArePinTypesCompatible, EdGraphSchema_K2.cpp 4187): the same pointers, so the value reads as a copy of its
source. A by-value or const& parameter (an engine function's too), a member, a local, a struct member and a return
each take one, from a variable, a call's result or a ?:; a map's key and value each are the same type or an upcast.
Pawns is two steps down from UObject. Snapshot empties Actors mid-call: C++ hands it a temporary, so it still counts what
Actors held.
*/
struct FHolder {
  UE_STRUCT;
  TArray<UObject *> Items;
};

class CovariantContainers : public AActor {
public:
  TArray<AActor *>         Actors;
  TArray<APawn *>          Pawns;
  TSet<AActor *>           ActorSet;
  TMap<FName, AActor *>    ByName;
  TMap<AActor *, int32>    Scores;
  TMap<AActor *, APawn *>  Owners;
  TArray<UObject *>        Objs;
  TSet<UObject *>          ObjSet;
  TMap<FName, UObject *>   ObjByName;
  TMap<UObject *, AActor *> ObjOwners;
  FHolder                  Holder;

  int32 CountArr(const TArray<UObject *> &A) { return A.Num(); }
  int32 CountSet(TSet<UObject *> S) { return S.Num(); }
  int32 CountKeys(const TMap<UObject *, int32> &M) { return M.Num(); }
  int32 CountValues(TMap<FName, UObject *> M) { return M.Num(); }
  int32 CountBoth(const TMap<UObject *, AActor *> &M) { return M.Num(); }
  UObject *FirstOf(const TArray<UObject *> &A) { return A[0]; }
  int32 Snapshot(const TArray<UObject *> &A) {
    Actors.Clear();
    return A.Num();
  }

  int32 Calls() {
    return CountArr(Actors) + 10 * CountSet(ActorSet) + 100 * CountKeys(Scores) + 1000 * CountValues(ByName)
         + 10000 * CountBoth(Owners);
  }
  int32 TwoSteps() { return CountArr(Pawns); }
  UObject *Element() { return FirstOf(Actors); }
  int32 Copied() { return Snapshot(Actors); }

  void Stores() {
    Objs = Actors;
    ObjSet = ActorSet;
    ObjByName = ByName;
    ObjOwners = Owners;
    Holder.Items = Pawns;
  }
  int32 Locals() {
    TArray<UObject *>       L = Actors;
    TSet<UObject *>         S(ActorSet);
    TMap<UObject *, int32>  M{Scores};
    return L.Num() + 10 * S.Num() + 100 * M.Num();
  }
  TArray<UObject *> Returned() { return Actors; }

  /* A non-const reference takes no upcast, but writing one into it is a copy like any other. */
  void Fill(TArray<UObject *> &R) { R = Pawns; }
  int32 Filled() {
    TArray<UObject *> L;
    Fill(L);
    return L.Num();
  }
  TArray<APawn *> GetPawns() { return Pawns; }
  int32 FromCall() { return CountArr(GetPawns()); }
  int32 Either(bool bActors) { return CountArr(bActors ? Actors : Objs); }
  FVector Engine() { return UGameplayStatics::GetActorArrayAverageLocation(Pawns); }
};
