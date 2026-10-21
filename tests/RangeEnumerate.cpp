#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/RangeEnumerate");

/*
`for (auto& [Item, Index] : Items.Enumerate())` walks the array by index: Item is the element where it lives, so Bump's
writes land in Items and Lift's in each point's Z, and Index counts from 0. `const auto&` reads in place; `auto` copies,
so Copies leaves Items as it was. Find returns from inside the walk; Odd skips with continue; Pairs walks the array
twice, nested, each loop with its own index. Deep walks a member of a struct, Grow walks Items while the body appends to
it: the length is read once, as C++ reads end() once.
*/
struct FBag {
  UE_STRUCT;
  TArray<int32> Inner;
};

class RangeEnumerate : public AActor {
public:
  TArray<int32>   Items;
  TArray<FVector> Points;
  FBag            Bag;

  void Bump() {
    for (auto &[Item, Index] : Items.Enumerate()) Item += Index * 10;
  }
  void Lift() {
    for (auto &[P, I] : Points.Enumerate()) P.Z = I;
  }
  int32 Weighted() {
    int32 S = 0;
    for (const auto &[Item, Index] : Items.Enumerate()) S += Item * Index;
    return S;
  }
  int32 Copies() {
    int32 S = 0;
    for (auto [Item, Index] : Items.Enumerate()) {
      S += Item + Index;
      Item = 0;
    }
    return S;
  }
  int32 Find(int32 X) {
    for (const auto &[Item, Index] : Items.Enumerate())
      if (Item == X) return Index;
    return -1;
  }
  int32 Odd() {
    int32 S = 0;
    for (auto &[Item, Index] : Items.Enumerate()) {
      if (Index % 2 == 0) continue;
      S += Item;
    }
    return S;
  }
  int32 Pairs() {
    int32 N = 0;
    for (const auto &[A, I] : Items.Enumerate())
      for (const auto &[B, J] : Items.Enumerate())
        if (I < J && A == B) ++N;
    return N;
  }
  void Deep() {
    Bag.Inner = Items;
    for (auto &[Item, Index] : Bag.Inner.Enumerate()) Item = Item + Index * 100;
  }
  int32 Grow() {
    int32 Last = -1;
    for (auto &[Item, Index] : Items.Enumerate()) {
      Items.Add(Item);
      Last = Index;
    }
    return Last;
  }
};
