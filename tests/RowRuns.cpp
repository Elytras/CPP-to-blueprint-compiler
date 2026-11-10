#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "../include/Intrin.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/RowRuns");

/*
One small function per REFERENCE.md row whose answer is a number, a string or a flag the suite can run (test_bytecode.py,
row_runs): each returns what the row says it does, and the suite's own Python computes the same from the inputs.
*/
constexpr int32 kStep = 4;
constexpr int32 kSlots = 3;
constexpr int32 kShift = 4;
constexpr int32 kLim = 5;

inline int32 ReadyBit(int32 Item) {
  if (Item > 3) return 1;
  return 0;
}

class RowRuns : public AActor {
public:
  int32 Count = 0;

  // Number literals and conversions.
  float HalfLiteral() {
    float Y = 0.5;
    return Y + 0.25;
  }
  float WidenToFloat(int32 N) {
    float Ratio = N;
    return Ratio / 4;
  }
  int64 WidenToInt64(int32 N) {
    int64 Big = N;
    return Big * 4294967296;
  }
  int32 ByteWraps(int32 Start) {
    uint8 Charges = Start;
    Charges += 200;
    return Charges;
  }
  float IntOverFloat(int32 A, float B) { return A / B; }

  // Strings.
  int32 EmptyLen() {
    FString S = FString();
    return UKismetStringLibrary::Len(S);
  }
  int32 WideLen() {
    FString Wide = L"wide";
    return UKismetStringLibrary::Len(Wide);
  }
  bool IsGold(FString S) { return S == "gold"; }
  bool NamesDiffer(FName A, FName B) { return A != B; }
  int32 StrLen(FString S) { return UKismetStringLibrary::Len(S); }

  // Constants and folding.
  bool InRangeConst() {
    bool Ready = kStep > 2 && kStep < 10;
    return Ready;
  }
  int32 ShiftConst() { return 1 << kShift; }
  int32 FoldMix(int32 N) { return N * kStep + kSlots * 2 + (1 << 4 | kStep); }
  int32 FoldShortCircuit(int32 X) {
    if (false && X) return 1;
    if (true || X) return 2;
    return 3;
  }
  int32 FoldLeft(int32 N) { return N * 3 + 8 + 19; }
  int32 FoldIdentity(int32 X) { return (X + 0) + (X - 0) + (X * 1) + (X / 1) + (X | 0) + (X ^ 0); }
  int32 DivZero(int32 X) { return X / 0; }
  int32 ConstLocal() {
    const int32 Three = 3;
    return Three * 2;
  }
  int32 ConstBranch(int32 N) {
    int32 Sum = 0;
    for (int32 I = 0; I < N; I++) {
      const int32 Lim = kLim;
      if (Lim > 3) break;
      Sum += 1;
    }
    return Sum;
  }
  int32 WhileFalse(int32 X) {
    int32 R = X;
    while (false) R = 99;
    return R;
  }
  int32 NegGreater(int32 Ammo) {
    if (!(Ammo > 0)) return 1;
    return 2;
  }
  int32 NegEqual(int32 A, int32 B) {
    if (!(A == B)) return 1;
    return 2;
  }
  int32 NegStrEqual(FString S, FString T) {
    if (!(S == T)) return 1;
    return 2;
  }
  bool NegStrValue(FString S, FString T) { return !(S == T); }
  int32 NegNeg(bool X) {
    if (!!X) return 1;
    return 2;
  }
  int32 NegElse(bool X) {
    int32 R = 0;
    if (!X) R = 10; else R = 20;
    return R;
  }
  int32 DropsUnused(int32 A) {
    UKismetMathLibrary::Abs_Int(A);
    int32 Unused = A * 3;
    return A + 1;
  }
  int32 KeepsUsed(int32 A) { return UKismetMathLibrary::Abs_Int(A); }
  int32 InlineBoolJump(int32 Item) {
    if (ReadyBit(Item)) return 10;
    return 20;
  }

  // Arithmetic and bitwise.
  int32 DivByZero(int32 Hits, int32 Shots) { return Hits / Shots; }
  int32 UnaryPlus(int32 X) { return +X; }
  int32 BitMix(int32 A, int32 B) { return (A & 0xFF) | (B ^ 1); }

  // Conditions and flow.
  int32 FloatTruth(float F) {
    if (F) return 1;
    return 0;
  }
  int32 ByteTruth(uint8 B) {
    if (B) return 1;
    return 0;
  }
  int32 Int64Truth(int64 I) {
    if (I) return 1;
    return 0;
  }
  int32 Compute() {
    Count += 1;
    return Count;
  }
  int32 VoidCall() {
    (void)Compute();
    return Count;
  }
  int32 DoOnce(int32 X) {
    int32 R = 0;
    do {
      R = 1;
      if (X > 0) break;
      R = 2;
    } while (false);
    return R;
  }
  int32 CondDecl(int32 V) {
    if (int32 Rest = V % 3) return Rest + 100;
    return 0;
  }
  int32 AliasMember(int32 By) {
    int32 &Alias = Count;
    Alias += By;
    return Count;
  }
  int32 AliasLocal(int32 By) {
    int32 Local = 1;
    int32 &Alias = Local;
    Alias += By;
    return Local;
  }

  // Methods and parameters.
  int32 GetCharges() const { return Count; }
  UE_PURE static int32 Clamp01(int32 V) {
    if (V < 0) return 0;
    if (V > 1) return 1;
    return V;
  }
  auto Half(int32 V) { return V / 2; }
  int32 ByVal(TArray<int32> A) {
    A.Add(1);
    return A.Num();
  }
  int32 CallByVal() {
    TArray<int32> L;
    L.Add(5);
    ByVal(L);
    return L.Num();
  }

  // Containers.
  int32 AddIndex() {
    TArray<int32> Scores;
    Scores.Add(5);
    return Scores.Add(7);
  }
  int32 RemoveAt() {
    TArray<int32> Items = {5, 6, 7};
    Items.Remove(1);
    return Items[0] * 100 + Items[1] * 10 + Items.Num();
  }
  int32 RemoveEqual() {
    TArray<int32> Items = {2, 5, 2, 7};
    Items.RemoveItem(2);
    return Items[0] * 100 + Items[1] * 10 + Items.Num();
  }
  int32 InsertAt() {
    TArray<int32> Items = {1, 2};
    Items.Insert(9, 0);
    return Items[0] * 100 + Items[1] * 10 + Items[2];
  }
  int32 SetAt() {
    TArray<int32> Items = {1, 2};
    Items.Set(0, 9, true);
    return Items[0] * 10 + Items[1];
  }
  bool SameElements(int32 X) {
    TArray<int32> A = {1, 2, X};
    TArray<int32> B = {1, 2, 3};
    return A.Identical(B);
  }

  // Memory.
  int32 SizeOfVector() { return sizeof(FVector); }
  int32 SizeOfDeref(FVector *P) { return sizeof(*P); }
  int32 AlignOfTransform() { return alignof(FTransform); }
  int64 Read64At(int64 A) { return __Read64__(A); }
  float ReadFloatAt(int64 A) { return __ReadFloat__(A); }
  int32 ReadByteAt(int64 A) { return __ReadByte__(A); }
  int32 Read32Via(int32 *P) { return __Read32__(P); }
  int64 PtrDiff(int32 *P, int32 *Q) { return P - Q; }
  int64 StepAll(int32 *P, int32 N) {
    P += N;
    ++P;
    P++;
    P -= 1;
    --P;
    P--;
    return (int64)P;
  }
  int64 PostStep(int32 *P) {
    int32 *Q = P++;
    return (int64)Q * 1000 + (int64)P - (int64)Q;
  }
  int64 AddrIndexed(int32 *P, int32 I) { return (int64)&P[I]; }
  int64 AddrDeref(int32 *P) { return (int64)&*P; }
  int32 CountToZero(int32 *Q) {
    int32 N = 0;
    while (*Q != 0) {
      ++Q;
      N += 1;
    }
    return N;
  }
  int32 NonNull(int32 *P) {
    if (P) return 1;
    return 0;
  }
  void BumpRef(int32 &V) { V += 1; }
  int32 BumpThrough(int32 *P) {
    BumpRef(*P);
    return *P;
  }
  int32 PtrCastRead(int64 Addr) {
    int32 *P = __PtrCast__<int32 *>(Addr);
    return *P;
  }
  int32 PtrCastRef(int64 Addr) {
    int32 &R = __PtrCast__<int32 &>(Addr);
    R += 1;
    return R;
  }
  int64 PtrCastAddr(int32 *P) {
    int32 &R = *P;
    return __PtrCast__<int64>(R);
  }
};
