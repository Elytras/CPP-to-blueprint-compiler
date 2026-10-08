// Three spellings a mod author reaches for: a global alias of an enum (`using EModeAlias = EMode;`, left standing where
// a class's or a template's is written out), a trailing return type (`auto F() -> int32`, whose "return part" was
// `auto`), and FString::Len() (written out in Types.h, so no marker named the library function and the call became a
// Context on a string). test_bytecode.py spelling_forms.
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SpellingForms");

enum class ESfMode : uint8 { A, B };
UE_ENUM(ESfMode);
using ESfAlias = ESfMode;
typedef ESfMode ESfTypedef;

class SpellingForms : public AActor {
public:
  FString Mem;
  ESfAlias Mode = ESfMode::B;
  ESfTypedef Other = ESfMode::A;
  ESfAlias Flip(ESfTypedef In) { return In == ESfMode::A ? ESfMode::B : ESfMode::A; }
  auto Twice(const int32 &V) const -> int32 { return V * 2; }
  auto Half(float V) -> float { return V / 2; }
  int32 LenOf(const FString &In) { return In.Len(); }
  int32 LenMem() { return Mem.Len(); }
};
