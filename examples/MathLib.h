/* MathLib.h: a function library that several mods share.

   MathLib.cpp cooks the library into a small mod of its own, and any other mod that includes this header calls it,
   as LibraryUser.cpp does. The library has no actor and starts nothing: it only runs when a mod calls it. */
#pragma once
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

/* The editor's Blueprint Function Library. UE_CLASS pins it to one owner. A source cooks the class only when this
   path is the one it would give the class itself: its UE_MOD_PACKAGE, then any namespace folders, then the class
   name. Only MathLib.cpp matches, and every other source that includes this header imports MathLib_C instead. */
class MathLib : public UBlueprintFunctionLibrary {
public:
  UE_CLASS("/Game/_AssetGenExamples/MathLib/MathLib", "MathLib_C");

  /* Part as a whole percentage of Whole, or 0 when Whole is not positive. In the editor it is a pure node. */
  UE_PURE static int32 Percent(float Part, float Whole);

  /* How many metres the local player's pawn is from Where. A caller that leaves WorldContextObject out passes
     itself, the way the editor wires the hidden world-context pin. */
  static int32 MetresFromPlayer(FVector Where, UObject *WorldContextObject = nullptr);

  /* Inline, so each call pastes this body in. The cooked class has no ToMetres, and a Blueprint made in the editor
     cannot call it: only C++ that includes this header can. */
  static inline float ToMetres(float Cm) { return Cm / 100.0f; }
};
