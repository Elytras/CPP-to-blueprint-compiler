#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ConstForms");

/*
Constants C++ knows when it compiles, written in forms other than a literal, and a key made from a name: `FKey("F5")`
and its spellings (any one-member struct's constructor in parentheses), a shift by a named constant, a type spelled through a local alias, `__is_same` as a value,
`sizeof...` of a pack and UE_ENUM_MAP in a function body.
*/
enum class ETint : uint8 { Pale, Warm = 4, Deep };
UE_ENUM(ETint);

constexpr int32 kShift = 3;

template <class... T> inline int32 CountOf(T... V) { return int32(sizeof...(V)) * 10 + int32(sizeof...(T)); }
template <class A, class B> inline bool SameAs() { return __is_same(A, B); }
template <int32 S> inline int32 ShiftBy(int32 X) { return X << S; }
inline int32 ShiftParm(int32 X, int32 S) { return X >> S; }
inline int32 TableSize(TMap<ETint, FName> T) { return T.Num(); }

class ConstForms : public AActor {
public:
  FName KeyCall() { return FKey("F5").KeyName; }
  FName KeyOfName() { return FKey(FName("F6")).KeyName; }
  FName KeyBraced() {
    FKey K{FName("F7")};
    return K.KeyName;
  }
  FName KeyCopyList() {
    FKey K = {"F8"};
    return K.KeyName;
  }
  FName KeyRuntime(FString S) { return FKey(FName(S)).KeyName; }
  FName KeyFromString(FString S) { return FKey(S).KeyName; }
  int32 Frame(int32 V) { return FFrameNumber(V + 1).Value + FFrameNumber(7).Value; }
  FKey Held = FKey("LeftShift");
  FName KeyTypo() { return FKey("LeftShfit").KeyName; }
  FName KeyMember() { return Held.KeyName; }

  int32 ShiftNamed(int32 X) { return X << kShift; }
  int32 ShiftParen(int32 X) { return X >> (2); }
  int32 ShiftUpdate(int32 X) {
    X <<= kShift;
    X >>= (int32)ETint::Warm;
    return X;
  }
  int32 ShiftForms(int32 X) {
    const int32 Local = 1;
    return (X << sizeof(int32)) + (X << (int32)ETint::Warm) + ShiftBy<2>(X) + ShiftParm(X * 64, 3) + (X << Local);
  }

  int32 Alias(int32 X) {
    using T = int32;
    typedef float F;
    T A = X;
    F B = 0.5f;
    return A + int32(B * 4.0f);
  }
  int32 AliasNested(int32 X) {
    using T = int32;
    int32 Sum = 0;
    {
      using U = TArray<T>;
      U Items = {X, X + 1};
      for (T I : Items) Sum += I;
    }
    T After = Sum;
    return After;
  }

  bool SameValue() {
    bool B = __is_same(int32, int);
    return B;
  }
  int32 SameIf() {
    if (__is_same(float, int32)) return 1;
    return 2;
  }
  int32 SameTernary() { return SameAs<FName, FName>() ? 3 : 4; }

  int32 PackCount() { return CountOf(1, 2.0f, FName("A")) + CountOf(); }

  FName TintName(ETint M) {
    TMap<ETint, FName> Names = UE_ENUM_MAP(ETint);
    return Names[M];
  }
  int32 TintTable() { return TableSize(UE_ENUM_MAP(ETint)); }
  int32 TintCount() {
    TMap<FString, ETint> ByName = UE_ENUM_MAP(ETint);
    return ByName.Num() * 100 + int32(ByName["Deep"]);
  }
};
