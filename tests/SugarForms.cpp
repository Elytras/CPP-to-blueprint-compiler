#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SugarForms");

/* Spellings a hand-written mod reaches for: GetComponent(s)ByClass<T>(), a class tested as a bool, `==` on vectors and
   rotators, a range over a computed value, `?:` over two chars or two string literals of one length, and FTransform's
   rotator constructors. */
class SugarForms : public AActor {
  TSubclassOf<AActor> Cls;
  TArray<int32> Items;
  TSet<int32> Seen;
  int32 Gets;

public:
  /* Get Components by Class: its own and another actor's, the array typed T*. */
  int32 CountMeshes(AActor *Other) {
    TArray<UStaticMeshComponent *> Own = GetComponentsByClass<UStaticMeshComponent>();
    int32 N = Own.Num() * 100;
    for (UStaticMeshComponent *C : Other->GetComponentsByClass<UStaticMeshComponent>())
      if (C) N += 1;
    return N;
  }

  /* Get Component by Class, typed T*: its own and another actor's. */
  int32 FindMesh(AActor *Other) {
    UStaticMeshComponent *Own = GetComponentByClass<UStaticMeshComponent>();
    return (Own ? 100 : 0) + (Other->GetComponentByClass<UStaticMeshComponent>() ? 1 : 0);
  }

  /* `if (Cls)` is Is Valid Class. */
  int32 ClassSet() {
    int32 N = Cls ? 100 : 200;
    if (Cls) N += 1;
    if (!Cls) N += 10;
    return N;
  }

  /* `==` and `!=` compare exactly, as UE C++'s FVector operators do; `V == 0` fills a vector with the int. */
  int32 VecCompare(FVector A, FVector B) { return (A == B ? 1 : 0) + (A != B ? 10 : 0) + (A == 0 ? 100 : 0); }
  FVector Twos() {
    FVector V = 2;
    return V;
  }
  bool RotSame(FRotator A, FRotator B) { return A == B; }

  /* A computed range is evaluated once, and a `T&` loop writes the copy, not Items. */
  TArray<int32> GetItems() {
    Gets++;
    return Items;
  }
  TSet<int32> GetSeen() {
    Gets++;
    return Seen;
  }
  int32 SumGot() {
    int32 S = 0;
    for (int32 X : GetItems()) S = S * 10 + X;
    return S * 100 + Gets;
  }
  int32 BumpGot() {
    int32 S = 0;
    for (int32 &X : GetItems()) {
      X += 1;
      S += X;
    }
    return S;
  }
  int32 SumSeen() {
    int32 S = 0;
    for (int32 X : GetSeen()) S += X;
    return S * 100 + Gets;
  }

  /* UE's FTransform(Rotation, Translation[, Scale]) and Make Transform's order (Location, Rotation, Scale): both are
     Make Transform, the scale 1 when left out. */
  FTransform PlaceUe(FRotator R, FVector T, FVector S) { return FTransform(R, T, S); }
  FTransform PlaceUeUnit(FRotator R, FVector T) { return FTransform(R, T); }
  FTransform PlaceMake(FVector L, FRotator R, FVector S) { return FTransform(L, R, S); }

  /* `?:` over two chars is their int code; over two string literals of one length, an FString. */
  int32 PickChar(bool B) { return B ? 'y' : 'n'; }
  FString PickSign(bool B) { return FString("x") + (B ? "+" : "-"); }
};
