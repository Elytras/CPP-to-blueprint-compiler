#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/NestedTest");

/* Nested containers: the inner container is the Value of a generated wrapper struct. */
class NestedTest : public AActor {
  TMap<FName, TArray<FName>> Groups;
  TArray<TArray<int32>> Grid;

public:
  int32 Build() {
    TArray<FName> Members;
    Members.Add(FName("a"));
    Members.Add(FName("b"));
    Groups.Add(FName("first"), Members);

    TArray<FName> Found;
    int32 Total = 0;
    if (Groups.Find(FName("first"), Found)) Total = Found.Length();
    TArray<FName> Again = Groups[FName("first")];
    Total = Total + Again.Length();

    TArray<int32> Row;
    Row.Add(1);
    Grid.Add(Row);
    Grid.Add(Row);
    Grid[1].Add(5);
    Grid[0][0] = 7;
    Grid[1] = Row;
    for (const TArray<int32>& R : Grid) Total = Total + R.Length();
    return Total + Grid[0][0];
  }

  TMap<FName, int32> Counts;
  /* Map[Key]: a read is Find (0 for a missing key), a store is Add, `+=` / `++` both. */
  int32 MapIndex(int32 Seed) {
    Counts[FName("a")] = Seed;
    Counts[FName("a")] += 2;
    TMap<int32, int32> Local;
    Local[7]++;
    Local[3] = Counts[FName("a")] * 10;
    return Local[3] + Local[7] + Local[99];
  }
};
