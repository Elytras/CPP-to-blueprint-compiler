/*
TemplateInst.cpp - templates that are not inline, each instantiation a function or a type of its own.

A free function template without `inline` gets one function of BPL_TemplateInst per instantiation a call reaches,
named by its template arguments (Max2__int32, Times__m2). A static member template of a mod class gets one static of
that class per instantiation (Get__int32). A class or struct template of the mod's own is one cooked class or struct
per instantiation (FTiPair__float, ATiHolder__int32). An inline template is still expanded at its call.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TemplateInst");

template <class T> T Max2(T A, T B) { return A > B ? A : B; }
template <class T> T Fact(T N) { return N <= 1 ? T(1) : N * Fact(N - T(1)); } // an instantiation may call itself
template <int32 N> int32 Times(int32 X) { return X * N; }
template <bool B> int32 Pick() { return B ? 1 : 2; }
template <class... T> int32 Count(T... V) { return int32(sizeof...(V)); }
namespace Util {
template <class T> T Twice(T V) { return V + V; }
} // namespace Util
template <class T> inline T Min2(T A, T B) { return A < B ? A : B; } // inline: expanded, no function
enum class ETiMode : uint8 { Calm, Angry };
template <ETiMode M> int32 ModeOf() { // ModeOf__Angry: an enum value by its enumerator
  if constexpr (M == ETiMode::Angry) return 2;
  else return 1;
}

template <class T> struct FTiPair {
  UE_STRUCT;
  T Lo;
  T Hi;
};
template <class T> T Width(const FTiPair<T> &B) { return B.Hi - B.Lo; }
template <ETiMode M> struct FTiTagged {
  UE_STRUCT;
  int32 N;
};

class UTiLib : public UBlueprintFunctionLibrary {
public:
  template <class T> static T Get(T V) { return V + V + V; }
};
template float UTiLib::Get<float>(float); // an explicit instantiation: Get__float, though nothing here calls it

template <class T> class ATiHolder : public AActor {
public:
  T Value;
  T Doubled() { return Value + Value; }
  void ReceiveBeginPlay() { Value = Value + 1; } // the engine calls it by name, so C++ must instantiate it:
};
template class ATiHolder<int32>; // every member of ATiHolder<int32>, as the class is cooked whole

class TemplateInst : public AActor {
public:
  FTiPair<float> Box;
  FTiPair<int32> IntBox;
  FTiTagged<ETiMode::Angry> Tag;
  int32 UseMode() { return ModeOf<ETiMode::Angry>() * 10 + ModeOf<ETiMode::Calm>(); }
  int32 UseMax(int32 A, int32 B) { return Max2(A, B); }
  float UseMaxF(float A, float B) { return Max2(A, B); }
  int32 UseFact(int32 N) { return Fact(N); }
  int32 UseTimes(int32 X) { return Times<3>(X) * 100 + Times<-2>(X); }
  int32 UsePick() { return Pick<true>() * 10 + Pick<false>(); }
  int32 UseCount() { return Count(1, 2.0f, true) * 10 + Count(); }
  int32 UseTwice(int32 V) { return Util::Twice(V); }
  int32 UseMin(int32 A, int32 B) { return Min2(A, B); }
  float UseBox(float Lo, float Hi) {
    Box.Lo = Lo;
    Box.Hi = Hi;
    return Width(Box);
  }
  int32 UseIntBox(int32 A) {
    IntBox.Hi = A;
    return IntBox.Hi + IntBox.Lo;
  }
  int32 UseGet(int32 V) { return UTiLib::Get(V); }
  int32 UseHolder(ATiHolder<int32> *H) { return H->Doubled(); }
};
