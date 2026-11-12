#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TmplStaticClass");

/* `T::StaticClass()` in a template's body: REFERENCE.md#staticclass-on-the.t-staticclass. C++ finds the StaticClass of
   the engine parent for every T, so the class meant is the one the template was instantiated with. */
class Lexer : public UObject {
public:
  int32 N = 0;
};
class Parser : public UObject {
public:
  int32 N = 0;
};

template <class T> UClass *ClassOfT() { return T::StaticClass(); }
template <class A, class B> inline UClass *Second() { return B::StaticClass(); }
template <class T> struct TPick {
  int32 Unused;
  UClass *Get() const { return T::StaticClass(); }
};
constexpr TPick<Parser> kPick = {0};

class TmplStaticClass : public AActor {
public:
  template <class T> UClass *Mine() { return T::StaticClass(); }
  template <class T> UClass *Both(bool bFirst) { return bFirst ? T::StaticClass() : Mine<Parser>(); } // an inner one

  UClass *Direct() { return Lexer::StaticClass(); }
  UClass *Free() { return ClassOfT<Lexer>(); }
  UClass *FreeOther() { return ClassOfT<Parser>(); }
  UClass *Member() { return Mine<Lexer>(); }
  UClass *MemberOther() { return Mine<Parser>(); }
  UClass *Pair() { return Second<Lexer, Parser>(); }
  UClass *PairSwapped() { return Second<Parser, Lexer>(); }
  UClass *Nested(bool bFirst) { return Both<Lexer>(bFirst); }
  UClass *OfStruct() { return kPick.Get(); }
  UClass *Engine() { return ClassOfT<AActor>(); }
};
