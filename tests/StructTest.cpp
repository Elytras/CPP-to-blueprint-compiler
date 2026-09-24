#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/StructTest");

struct FStats {
  UE_STRUCT;
  int32 Kills;
  float Time;
  bool Alive;
  class UObject *Owner;
};

struct FNested {
  UE_STRUCT;
  FStats Inner;
  int64 Stamp;
};

enum class EMood : uint8 { Happy, Sad, Angry };
UE_ENUM(EMood);

struct FMoody {
  UE_STRUCT;
  EMood Mood;
  int32 Level;
};

class StructTest : public AActor {
  FStats Stats;
  FNested Nested;
  FMoody Moody;

public:
  /*
  Make Struct, the editor's node: a braced or designated value is a default-constructed temp plus one member store
  per value given, so what the braces leave out keeps the struct's own default. FHitResult is a struct
  EX_StructConst cannot write whole, which is why `FHitResult()` takes the same road.
  */
  int32 MakeLocal(int32 K) {
    FStats S = { .Kills = K, .Alive = true };
    FNested N = { .Inner = { .Kills = K * 2 }, .Stamp = 9 };
    return S.Kills + N.Inner.Kills * 10 + (S.Alive ? 1000 : 0);
  }

  int32 KillsOf(FStats S) { return S.Kills; }
  int32 MakeArgument(int32 K) { return KillsOf({ .Kills = K }) + KillsOf(FStats{}) + KillsOf({ .Kills = K + 1 }); }

  /* Made again each time round: what one pass wrote into the variable does not survive into the next. */
  int32 MakeInLoop(int32 Rounds) {
    int32 Sum = 0;
    for (int32 I = 0; I < Rounds; I++) {
      FStats S = { .Kills = 1 };
      Sum += S.Kills + (S.Alive ? 100 : 0);
      S.Alive = true;
    }
    return Sum;
  }

  float MakeNative(float D) {
    FHitResult Hit = { .Time = 0.5f, .Distance = D };
    FHitResult Blank = FHitResult();
    return Hit.Distance + Hit.Time + Blank.Distance;
  }

  void ReceiveBeginPlay() {
    Stats.Kills = 3;
    Stats.Alive = true;
    Stats.Owner = this;
    FStats Local;
    Local.Time = 1.5f;
    Local.Kills = Stats.Kills + 1;
    Stats = Local;
    Nested.Inner = Stats;
    Nested.Inner.Kills = Nested.Inner.Kills + Local.Kills;
    Moody.Mood = EMood::Happy;
    Moody.Level = 2;
    if (Stats.Alive)
      UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage("StructTest: struct members round-trip");
  }
};
