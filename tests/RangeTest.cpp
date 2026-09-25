#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/RangeTest");

/* Range-for over TArray (in place), TSet and TMap. */
class RangeTest : public AActor {
  TArray<int32> Items;
  TSet<int32> Seen;
  TMap<FName, int32> Scores;
  TMap<FName, FIntPoint> Spots;
  TMap<FName, RangeTest *> Peers;
  int32 Hits;
  RangeTest *Near;
  RangeTest *Far;
  RangeTest *Cur;
  int32 Picks;

public:
  int32 SumArray() {
    int32 Sum = 0;
    for (int32 X : Items) Sum += X;
    return Sum;
  }

  /* C++20: the init-statement runs once, before the loop. */
  int32 SumScaled() {
    int32 Sum = 0;
    for (int32 Scale = 3; int32 X : Items) Sum += X * Scale;
    return Sum;
  }

  int32 DoubleInPlace() {
    for (int32 &X : Items) {
      if (X < 0) continue;
      X *= 2;
      if (X > 100) break;
    }
    int32 Sum = 0;
    for (const int32 &X : Items) Sum = Sum * 3 + X;
    return Sum;
  }

  int32 CopyDoesNotWrite() {
    for (int32 X : Items) X = 0;
    return Items.Num();
  }

  int32 NestedPairs() {
    int32 Hits = 0;
    for (int32 A : Items)
      for (int32 B : Items)
        if (A < B) Hits++;
    return Hits;
  }

  int32 SumSet() {
    int32 Sum = 0;
    for (int32 X : Seen) {
      Sum += X;
    }
    return Sum;
  }

  int32 BumpScores(int32 Stop) {
    int32 Count = 0;
    for (auto &[Key, Value] : Scores) {
      Value += 10;
      Count++;
      if (Value > Stop) break;
    }
    for (auto [Key, Value] : Scores) Value += 1000; // a copy: the map keeps its values
    int32 Total = 0;
    for (const auto &[Key, Value] : Scores) Total += Value;
    return Total * 100 + Count;
  }

  /* A return from inside a reference loop, or from a loop nested in it, still writes back the value it changed. */
  int32 BumpScoresUntil(int32 Stop) {
    for (auto &[Key, Value] : Scores) {
      Value += 10;
      for (int32 I = 0; I < 2; I++)
        if (Value + I > Stop) return Value * 10 + I;
    }
    return -1;
  }

  void CapScores(int32 Cap) {
    for (auto &[Key, Value] : Scores) {
      Value += 1;
      if (Value > Cap) {
        Value = Cap;
        return;
      }
    }
  }

  /* The same from an inline body: its return leaves the body, not the caller. */
  inline int32 BumpScoresInline(int32 Stop) {
    for (auto &[Key, Value] : Scores) {
      Value += 1;
      if (Value > Stop) return Value;
    }
    return -1;
  }
  int32 BumpScoresInlined(int32 Stop) { return BumpScoresInline(Stop) * 2 + 1; }

  /* Value is the element itself: a body that also reaches Scores (a read, a store, a nested walk, a method that
     reads it) sees each write to Value at once, and its own stores are not undone. */
  int32 ReadScoresThrough() {
    int32 Total = 0;
    for (auto &[Key, Value] : Scores) {
      Value = 5;
      Total += Scores[Key];
    }
    return Total;
  }
  void StoreScoresThrough() {
    for (auto &[Key, Value] : Scores) {
      Value += 1;
      Scores[Key] = Value * 10;
    }
  }
  void NestScores() {
    for (auto &[Key, Value] : Scores) {
      Value += 1;
      for (auto &[Key2, Value2] : Scores) {
        Value2 += 10;
        if (Key2 == Key) break;
      }
    }
  }
  int32 TotalScores() {
    int32 Total = 0;
    for (const auto &[Key, Value] : Scores) Total += Value;
    return Total;
  }
  int32 SumScoresViaMethod() {
    int32 Total = 0;
    for (auto &[Key, Value] : Scores) {
      Value = 7;
      Total += TotalScores();
    }
    return Total;
  }
  /* A body that only reads the map and V keeps the copy; one that writes V (a field of it too) and reads the map
     sees the element. A method called on another object reaches that object's map. */
  int32 SpotCount() { return Spots.Num(); }
  int32 WeighSpots() {
    int32 Sum = 0;
    for (const auto &[Key, Spot] : Spots) Sum += Spot.X * SpotCount() + Spot.Y;
    return Sum;
  }
  int32 NudgeSpots() {
    int32 Sum = 0;
    for (auto &[Key, Spot] : Spots) {
      Spot.X += 1;
      Sum += Spot.X * SpotCount() + Spots[Key].X;
    }
    return Sum;
  }
  void BumpAllScores() {
    for (auto &[Key, Value] : Scores) Value += 100;
  }
  int32 BumpThroughNear() {
    int32 Total = 0;
    for (auto &[Key, Value] : Near->Scores) {
      Value += 1;
      Near->BumpAllScores();
      Total += Value;
    }
    return Total;
  }

  /* `Spot.X += 1` writes the value itself, so it goes back to the map; `Spot->X` would write an object instead. */
  int32 ShiftSpots() {
    for (auto &[Key, Spot] : Spots) Spot.X += 1;
    int32 Sum = 0;
    for (const auto &[Key, Spot] : Spots) Sum += Spot.X * 10 + Spot.Y;
    return Sum;
  }

  /* `Peer->Hits += 1` writes the object; the pointer the map holds is unchanged, so nothing goes back. */
  void PokePeers() {
    for (auto &[Key, Peer] : Peers) Peer->Hits += 1;
  }

  /* The range is bound once: PickPeer() runs one time, and the whole walk is over Near's containers. */
  RangeTest *PickPeer() {
    Picks++;
    return Picks == 1 ? Near : Far;
  }
  int32 SumPicked() {
    int32 Sum = 0;
    for (int32 X : PickPeer()->Items) Sum = Sum * 10 + X;
    return Sum * 100 + Picks;
  }
  void DoublePicked() {
    for (int32 &X : PickPeer()->Items) X *= 2;
  }
  void BumpPicked() {
    for (auto &[Key, Value] : PickPeer()->Scores) Value += 1;
  }

  /* Reseating the pointer in the body does not move the walk: the range stays Cur's containers at loop entry. */
  int32 SumReseat() {
    int32 Sum = 0;
    for (int32 X : Cur->Items) {
      Cur = Far;
      Sum = Sum * 10 + X;
    }
    RangeTest *P = Near;
    for (int32 X : P->Items) {
      P = Far;
      Sum = Sum * 10 + X;
    }
    return Sum;
  }
  void DoubleReseat() {
    for (int32 &X : Cur->Items) {
      Cur = Far;
      X *= 2;
    }
  }
  void BumpReseat() {
    for (auto &[Key, Value] : Cur->Scores) {
      Cur = Far;
      Value += 1;
    }
  }
};
