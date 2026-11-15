#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/MapArrays");

/* A store to an element of an array a map holds: REFERENCE.md#tmap. Map_Find copies the value out, so each of these
   changes the copy and stores the whole value back under its key. */
struct FBag {
  TArray<int32> Items;
  int32 Count;
};

inline void Grow(int32 &V, int32 By) { V += By; }

class MapArrays : public AActor {
public:
  TMap<int32, TArray<int32>> Lists;
  TMap<int32, FBag> Table;
  int32 Calls = 0;

  int32 Key() { return ++Calls; } // 1 on the first call

  void Seed() {
    Calls = 0;
    Lists.Clear();
    TArray<int32> A;
    A.Add(10);
    A.Add(20);
    A.Add(30);
    Lists.Add(1, A);
    TArray<int32> B;
    B.Add(5);
    Lists.Add(2, B);
    Table.Clear();
    FBag Bag;
    Bag.Items = A;
    Bag.Count = 3;
    Table.Add(7, Bag);
  }

  int32 Store(int32 V) {
    Seed();
    Lists[1][0] = V;
    return Lists[1][0] * 1000 + Lists[1][1] + Lists[1].Num() * 100000;
  }
  int32 StoreMember(int32 V) {
    Seed();
    Table[7].Items[2] = V;
    return Table[7].Items[2] * 100 + Table[7].Items[0] + Table[7].Count * 100000;
  }
  int32 Update(int32 V) {
    Seed();
    Lists[1][1] += V;
    ++Lists[1][2];
    Lists[2][0]--;
    return Lists[1][1] * 10000 + Lists[1][2] * 100 + Lists[2][0];
  }
  int32 Bound(int32 V) {
    Seed();
    Grow(Lists[1][0], V);
    return Lists[1][0];
  }
  int32 Pairs(int32 V) { // L is each key's array
    Seed();
    for (auto &[K, L] : Lists)
      L[0] = K * V;
    return Lists[1][0] * 100 + Lists[2][0];
  }
  int32 Others(int32 V) { // the other key and the other elements are as they were
    Seed();
    Lists[1][1] = V;
    return Lists[2][0] * 10000 + Lists[1][0] * 100 + Lists[1][2];
  }
  int32 KeyOnce(int32 V) { // the key is found once: Key() runs once
    Seed();
    Lists[Key()][0] = V;
    return Lists[1][0] * 10 + Calls;
  }

  /* A reference local to a map's value, or to a place in it: another name for the place, its key found once. */
  TMap<int32, int32> Scores;
  int32 Named(int32 V) {
    Seed();
    auto &L = Lists[1];
    L[0] = V;
    L[1] = V + 1;
    L.Add(V + 2);
    return Lists[1][0] * 10000 + Lists[1][1] * 100 + Lists[1][3] + L.Num() * 1000000;
  }
  int32 NamedBag(int32 V) {
    Seed();
    FBag &B = Table[7];
    B.Count += V;
    B.Items[0] = V;
    return Table[7].Count * 1000 + Table[7].Items[0];
  }
  int32 NamedElem(int32 V) {
    Seed();
    int32 &X = Lists[1][2];
    X += V;
    X += V;
    return Lists[1][2];
  }
  int32 NamedPinned(int32 V) { // the key is read where L is declared
    Seed();
    int32 K = 1;
    auto &L = Lists[K];
    K = 2;
    L[0] = V;
    return Lists[1][0] * 100 + Lists[2][0];
  }
  int32 NamedScore(int32 V) { // the value itself
    Scores.Clear();
    Scores.Add(4, 10);
    int32 &R = Scores[4];
    R += V;
    R = R * 2;
    return Scores[4];
  }
  int32 NamedRead(int32 V) { // a reference sees a later write
    Seed();
    const auto &L = Lists[1];
    Lists[1][0] = V;
    return L[0];
  }
  int32 NamedKeyOnce(int32 V) {
    Seed();
    auto &L = Lists[Key()];
    L[0] = V;
    L[1] = V;
    return Calls * 1000 + Lists[1][0] + Lists[1][1];
  }

  /* A range-for by reference over an array a map holds: the walk is over the copy, stored back when the loop is left. */
  int32 Walk(int32 V) {
    Seed();
    for (int32 &X : Lists[1])
      X *= V;
    return Lists[1][0] + Lists[1][1] + Lists[1][2];
  }
  int32 WalkMember(int32 V) {
    Seed();
    for (auto &X : Table[7].Items)
      X += V;
    return Table[7].Items[0] * 10000 + Table[7].Items[1] * 100 + Table[7].Items[2];
  }
  int32 WalkReturn(int32 V) { // a return in the body leaves through the store
    Seed();
    for (int32 &X : Lists[1]) {
      X = V;
      if (X == V)
        return Lists[1][0];
    }
    return -1;
  }
  int32 WalkBreak(int32 V) {
    Seed();
    for (int32 &X : Lists[1]) {
      X += V;
      break;
    }
    return Lists[1][0] * 100 + Lists[1][1];
  }
  int32 WalkRead() { // only read: no copy is stored
    Seed();
    int32 Sum = 0;
    for (int32 &X : Lists[1])
      Sum += X;
    for (const int32 &X : Lists[2])
      Sum += X;
    return Sum;
  }
};
