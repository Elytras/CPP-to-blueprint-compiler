/*
FreeTest.cpp - free functions that are not inline.

A Blueprint has no free functions. Each one a function calls is a static of the one function library the compiler
generates for the source, BPL_FreeTest, called on its default object. Like a function of the editor's function library,
it gets a hidden world context the caller fills in with its self. A template without `inline` is one function per
instantiation, named by its arguments: Max2__int32.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FreeTest");

int32 Twice(int32 V) { return V * 2; }
int32 Fact(int32 N) { return N <= 1 ? 1 : N * Fact(N - 1); } // a Blueprint function may call itself
int32 Later(int32 V);                                        // a call before the definition names this declaration
namespace Util {
int32 Clamp(int32 V, int32 Lo = 0, int32 Hi = 10) { return V < Lo ? Lo : V > Hi ? Hi : V; }
int32 Twice(int32 V) { return ::Twice(V) + 1; } // Util__Twice beside Twice
} // namespace Util
inline int32 Pick(int32 V) { return V + 1; } // expanded; the bool overload is the library's
int32        Pick(bool B) { return B ? 100 : 200; }
/* No WorldContext parameter: the hidden one is what GetPlayerPawn gets, the caller's self. */
APawn *FirstPawn() { return UGameplayStatics::GetPlayerPawn(0); }
template <class T> T Max2(T A, T B) { return A > B ? A : B; } // Max2__int32, the instantiation UseMax calls
constexpr int32 Sq(int32 X) { return X * X; } // not run at build time: a library function like any other
/* A world context it declares replaces the hidden one. */
APawn *TheirPawn(UObject *WorldContextObject = nullptr) { return UGameplayStatics::GetPlayerPawn(WorldContextObject, 0); }

class FreeTest : public AActor {
public:
  int32 UseTwice(int32 V) { return Twice(V) + Util::Twice(V); }
  int32 UseFact(int32 N) { return Fact(N); }
  int32 UseLater(int32 V) { return Later(V); }
  int32 UseDefaults(int32 V) { return Util::Clamp(V) * 100 + Util::Clamp(V, 3, 5); }
  int32 UsePick(int32 V) { return Pick(V) + Pick(true); }
  APawn *UsePawn() { return FirstPawn(); }
  int32 UseMax(int32 A, int32 B) { return Max2(A, B); }
  int32 UseSq(int32 V) { return Sq(V); }
  APawn *UseTheirs(AActor *Other) { return TheirPawn(Other); }
  APawn *UseTheirsDefault() { return TheirPawn(); }
  static int32 FromStatic(int32 V) { return Twice(V); }
};

int32 Later(int32 V) { return Util::Clamp(V) + Twice(V); }
