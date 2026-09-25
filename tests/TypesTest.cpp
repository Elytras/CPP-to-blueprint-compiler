#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TypesTest");

/* Cooked as a UserDefinedEnum with these names and values. */
enum class EMood : uint8 { Calm, Angry = 5, Sleepy };
UE_ENUM(EMood);

/* Wider than a Blueprint enum: an EnumProperty over an IntProperty / Int64Property. */
enum class ESpan : int32 { Tiny = -3, Wide = 70000, Huge = 70001, Vast = 70002 };
UE_ENUM(ESpan);
enum class EAge : int64 { Epoch = 0, Eon = 5000000000 };
UE_ENUM(EAge);

/* Constants have no storage in a Blueprint: a use is the value, and a default is whatever the expression comes to. */
constexpr int32 kStep = 3;
constexpr float kHalf = 1 / 2.f;
const int32 kMask = 1 << 4 | kStep;
/* No fixed type: int-sized (or wider when a value needs it), not a byte. */
enum { kCap = 1000, kDebt = -5 };
enum ELoose { LooseBig = 70000 };
enum { kFar = 5000000000 };

/* C++20. consteval: clang runs it and AssetGen reads the answer off the AST, so its body can be anything at all.
   A concept and an `auto` parameter cost nothing either: clang instantiates, AssetGen splices the instantiation. */
consteval int32 Fnv(const char *S) {
  uint32 H = 2166136261u;
  while (*S) H = (H ^ uint32(*S++)) * 16777619u;
  return int32(H & 0x7FFFFFFF);
}
template <class T> concept Number = requires(T A) { A + A; };
inline auto Twice(Number auto V) { return V + V; }

/* A mod's own name for a class: how one that two packages both have (so the headers give it no short name) is
   spelled. It must stay the object reference it is, not pass for a raw pointer. */
using FTarget = AActor;

class TypesTest : public AActor, public ITargetable {
  FTarget *Aimed;
  using FSpot = APawn;        // class scope, as genueapi opens a Blueprint class: `using Leaf = Game::...::Leaf;`
  FSpot *Spotted;
  static constexpr int32 kSeed = Fnv("types");
  int32 Seed = kSeed;
  static constexpr int32 kSlots = kStep * 4;
  int32 Budget = kSlots * 2 + 1;
  float Reach = kHalf * 300 - 25;
  bool Halfway = kHalf;        // a float is true when nonzero: 0.5f is true
  int32 Bits = ~kMask & 0xFF;
  UE_DISPATCHER(OnScored, int32 Points, AActor *By);
  TScriptInterface<IHealth> Health;
  FVector Home;
  FRotator Facing;
  EEndPlayReason LastReason;
  FLinearColor Tint;
  FHitResult Hit;
  TSubclassOf<AActor> Kind;
  TSoftObjectPtr<UTexture2D> Icon;
  UClass* Raw;
  TArray<int32> Scores;
  TArray<FVector> Points = { {1, 2, 3}, FVector(4, 5, 6) };     // struct elements: raw for a natively serialized struct,
  TArray<FFloatInterval> Spans = { {1, 5}, {-2, 2} };            // tags for any other
  TMap<FName, FVector> Spots = { {"home", {7, 8, 9}} };
  TMap<FName, float> Weights;
  TSet<int32> Seen;
  TMap<EMood, FName> MoodNames = UE_ENUM_MAP(EMood);         // one pair per enumerator, filled at build time,
  TMap<FString, EMood> MoodsByName = UE_ENUM_MAP(EMood);      // so the table follows the enum
  EMood Mood = EMood::Angry;
  ESpan Span = ESpan::Wide;
  EAge Age = EAge::Eon;

public:
  int32 ConstSum(int32 N) { return N * kStep + kSlots + kMask; }
  float HalfOf(float V) { return V * kHalf; }
  /* Signed >> floors: -3 >> 1 is -2, where a plain divide by 2 gives -1. */
  int32 ShrBy(int32 X, int32 M) { return M == 1 ? X >> 1 : M == 4 ? X >> 4 : X >> 31; }
  int64 Shr64(int64 X, int32 M) { return M == 1 ? X >> 1 : X >> 63; }
  /* A shift has the promoted LHS type: `int >> 1LL` is int32 math. */
  int32 ShiftByLL(int32 X, int32 M) { if (M == 0) return X << 2LL; if (M == 1) return X >> 1LL; X >>= 3LL; return X; }
  /* A float becomes an integer truncated toward zero, not printed to 6 decimals first (0.99999994f is 0). */
  int32 TruncOf(float X, int32 M) {
    int32 N = X;
    return M == 0 ? (int32)X : M == 1 ? N : static_cast<int32>(X * 2);
  }
  int64 Trunc64Of(float X) { return (int64)X; }
  int32 AnonConst(int32 X, int32 M) {
    int32 V = kCap;
    return M == 0 ? (X > kCap ? kCap : X) : M == 1 ? X + kDebt : M == 2 ? X * LooseBig : M == 3 ? X == LooseBig : V;
  }
  int64 WideConst(int64 X) { return X + kFar; }
  /* A constant float is true when nonzero, not when it truncates to nonzero (0.5f and 0.25f are true). */
  int32 FloatTruth(int32 X, int32 M) {
    return M == 0 ? !kHalf : M == 1 ? kHalf && X > 0 : M == 2 ? (kHalf ? 7 : 9) : (bool)0.25f + 0;
  }

  /* A null interface is EX_NoInterface: EX_NoObject would set half of the 16 bytes. */
  void Forget() {
    Health = nullptr;
    Aimed = nullptr;
  }

  int32 Cpp20(EMood M, int32 N) {
    using enum EMood;
    switch (M) {
    case Calm: return Twice(N);
    case Angry: [[likely]] return N + Fnv("angry");
    case Sleepy: break;
    }
    return kSeed;
  }

  int32 MoodScore(EMood M) {
    switch (M) {
    case EMood::Calm: return 1;
    case EMood::Angry: return 2;
    case EMood::Sleepy: return 3;
    }
    return M == Mood ? 10 : 0;
  }

  int32 SpanScore(ESpan S) {
    switch (S) {
    case ESpan::Tiny: return 1;
    case ESpan::Wide: return 2;
    case ESpan::Huge: return 3;
    case ESpan::Vast: return 4;
    }
    return S == Span ? 10 : 0;
  }

  int32 AgeOf(EAge A) { return A == EAge::Eon ? 1 : A == EAge::Epoch ? 2 : 0; }

  void ReceiveBeginPlay() {
    Home = FVector(1, 2, 3);
    FVector Twice = Home + Home * 2.0f;
    Home.Z = Twice.X + 1;
    Facing = Twice;
    FTransform Placed = Home;
    Tint = FLinearColor(1, 0.5f, 0, 1);
    K2_SetActorLocation(Twice, false, Hit, false);
    if (Home == Twice) LastReason = EEndPlayReason::Quit;
    Kind = AActor::StaticClass();
    Raw = Kind;
    Icon = "/Game/UI/Icons/T_Icon.T_Icon";
    APawn* Pawn = Cast<APawn>(this);
    if (Pawn != nullptr) Raw = APawn::StaticClass();
    Scores.Add(7);
    Scores.Add(Scores.Num());
    Scores[0] = Scores[0] + Scores.Length();
    int32 First = Scores[1];
    Points.Add(Home);
    Home = Points[0];
    Weights.Add("alpha", 0.5f);
    float W = 0;
    if (Weights.Find("alpha", W)) Seen.Add(First);
    if (Seen.Contains(First)) Scores.Clear();
    TArray<FName> Names;
    Weights.Keys(Names);
    TSet<int32> Both;
    Seen.Union(Seen, Both);
    Both.ToArray(Scores);
    Scores.Get(0, First);
    OnScored.Add(this, &TypesTest::HandleScored);
    OnScored.Broadcast(First, this);
    OnScored.Remove(this, &TypesTest::HandleScored);
    OnScored.Clear();
    OnDestroyed.Add(this, &TypesTest::HandleDestroyed);
    if (Pawn != nullptr) Pawn->OnDestroyed.Add(this, &TypesTest::HandleDestroyed);
    UKismetSystemLibrary::K2_SetTimerDelegate({this, &TypesTest::HandleTimer}, 1.0f, false, 0.0f, 0.0f);
    Health = Pawn;
    W = Health->GetHealth();
    Raw = Cast<UClass>(Health.GetObject());
    Tint.A = Pawn->CustomTimeDilation;
    UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(
        FString("Home: ") + Home + ", quit " + FString(LastReason == EEndPlayReason::Quit));
  }

  void ReceiveEndPlay(EEndPlayReason Reason) { LastReason = Reason; }
  void HandleScored(int32 Points, AActor *By) { Scores.Add(Points); }
  void HandleDestroyed(AActor *DestroyedActor) { Kind = UGameplayStatics::GetObjectClass(DestroyedActor); }
  void HandleTimer() { OnScored.Broadcast(1, this); }
  bool GetIsTargetable() const { return true; }
};
