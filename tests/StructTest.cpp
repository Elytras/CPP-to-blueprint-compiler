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
  TArray<int32> Counts;
  TArray<FStats> Many;
  TSet<int32> Seen;
  TSet<int32> Fresh;
  TMap<int32, int32> Scores;

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

  /* A local copy handed to what writes through a reference: the variable it was copied from keeps its value. */
  void SetKills(int32 K, FStats &S) { S.Kills = K; }
  void AddTo(int32 &V, int32 By) { V = V + By; }
  int32 CopyToRef(int32 K) { FStats T = Stats; SetKills(K, T); return Stats.Kills; }
  int32 MemberCopyToRef(int32 K) { int32 T = Stats.Kills; AddTo(T, K); return Stats.Kills; }
  int32 ArgCopyToRef(int32 K) { int32 T = K; AddTo(T, 100); return K; }
  int32 ArrayCopyAdd(int32 K) { TArray<int32> T = Counts; T.Add(K); return Counts.Num(); }
  int32 ArrayGetIntoCopy(int32 K) { int32 T = Stats.Kills; Counts.Get(0, T); return Stats.Kills; }
  /* A container function's out value, and a source it reads in place while writing another argument. */
  int32 MapFindIntoCopy(int32 K) { int32 T = K; Scores.Find(1, T); return K; }
  int32 ArrayAppendCopy(int32 K) { TArray<int32> T = Counts; Counts.Append(T); return Counts.Num() + K; }
  int32 SetUnionCopy(int32 K) { TSet<int32> T = Seen; Fresh.Union(T, Seen); return Seen.Num() + K; }
  int32 RangeCopyToRef(int32 K) {
    for (FStats S : Many) SetKills(K, S);
    return Many[0].Kills;
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
