// A type spelled from the global namespace: `::EMode M;`, `TArray<::FPair>`, `::int32`. clang prints the `::` into the
// type, which no table knows ("unimplemented property M: ::EMode"); ExpandAliases drops the one that starts a name
// (test_bytecode.py global_scope_types).
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/GlobalScope");

enum class EGsMode : uint8 { A, B, C };
UE_ENUM(EGsMode);

struct FGsPair {
  UE_STRUCT;
  int32 X = 1;
  float Y = 2.0f;
};

class GlobalScope : public AActor {
public:
  ::EGsMode M = EGsMode::B;
  ::FGsPair P;
  ::TArray<::FGsPair> L;
  ::int32 N = 3;
  ::FString S;
  ::AActor *Other = nullptr;
  ::TMap<::FString, ::EGsMode> Names;
  ::TSet<::int32> Seen;
  ::TSubclassOf<::AActor> Cls;
  ::EGsMode Pick(::EGsMode In) { return In; }
  ::int32 Sum(::FGsPair In) { return In.X + N; }
  ::int32 Twice(::int32 In) { return In * N; }
  ::int32 Body() { ::int32 A = (::int32)2.5f; ::FVector V = ::FVector(1.f, 4.f, 3.f); return A + static_cast<::int32>(V.Y); }
  ::EGsMode Which(::int32 I) { return I > 1 ? ::EGsMode::C : ::EGsMode::A; }
  float Absolute(float X) { return ::UKismetMathLibrary::Abs(X); }
  ::int32 Count() { L.Add(P); L.Add(P); return L.Num(); }
};
