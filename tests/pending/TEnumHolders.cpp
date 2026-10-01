#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TEnumHolders");

/*
A type that holds a TEnum<E> is not a TEnum<E>: a TArray of them has its own methods, and a dispatcher with a TEnum<E>
parameter its Add and Broadcast. Only TEnum<E> itself has Name() and String().
*/
enum class EThPick : uint8 { Zero, One, Two, Three };
UE_ENUM(EThPick);

class TEnumHolders : public AActor {
public:
  UE_DISPATCHER(OnPick, TEnum<EThPick> P, int32 N);
  TArray<TEnum<EThPick>> Items = {EThPick::One, EThPick::Three};
  int32 Seen;

  void Handle(TEnum<EThPick> P, int32 N) { Seen = (int32)(EThPick)P * 10 + N; }
  int32 Fire(int32 N) {
    OnPick.Add(this, &TEnumHolders::Handle);
    OnPick.Broadcast(EThPick::Two, N);
    return Seen;
  }
  int32 Count(int32 M) {
    Items.Add(EThPick::Two);
    return Items.Num() * 10 + (Items.Contains(EThPick::Three) ? 1 : 0) + M;
  }
  int32 Size(const TArray<TEnum<EThPick>>& A) { return A.Num(); }
  int32 Local(int32 M) {
    TArray<TEnum<EThPick>> A = {EThPick::One, EThPick::Two};
    return A.Num() + Size(A) * 10 + A.Find(EThPick::Two) * 100 + M;
  }
  FName NameOf() { return Items[1].Name(); }
};
