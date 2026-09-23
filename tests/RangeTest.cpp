#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/RangeTest");

/* Range-for over TArray (in place), TSet and TMap. */
class RangeTest : public AActor {
  TArray<int32> Items;
  TSet<int32> Seen;
  TMap<FName, int32> Scores;

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
    for (auto [Key, Value] : Scores) {
      Value += 10;
      Count++;
      if (Value > Stop) break;
    }
    int32 Total = 0;
    for (const auto [Key, Value] : Scores) Total += Value;
    return Total * 100 + Count;
  }
};
