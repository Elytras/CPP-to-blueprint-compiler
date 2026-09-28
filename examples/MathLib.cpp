/* MathLib.cpp: the MathLib mod, the one source that cooks the library declared in MathLib.h.

   Its UE_MOD_PACKAGE plus the class name is the path in MathLib's UE_CLASS, so compiling it writes MathLib.uasset
   with the two functions below. The mod does nothing in game by itself; LibraryUser calls it. */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

#include "MathLib.h"

UE_MOD_PACKAGE("/Game/_AssetGenExamples/MathLib");

int32 MathLib::Percent(float Part, float Whole) {
  if (Whole <= 0.0f)
    return 0;
  return UKismetMathLibrary::Round(Part / Whole * 100.0f);
}

/* GetPlayerPawn leaves its world context out, so here it gets WorldContextObject, which is the caller itself when
   the caller leaves it out too. Without the parameter it would get the class default object, which has no world. */
int32 MathLib::MetresFromPlayer(FVector Where, UObject *WorldContextObject) {
  APawn *Pawn = UGameplayStatics::GetPlayerPawn(0);
  if (!Pawn)
    return 0;
  float Cm = UKismetMathLibrary::Vector_Distance(Pawn->K2_GetActorLocation(), Where);
  return UKismetMathLibrary::Round(ToMetres(Cm));
}
