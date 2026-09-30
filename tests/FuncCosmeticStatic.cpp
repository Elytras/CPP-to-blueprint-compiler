#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncCosmeticStatic");

/*
Engine statics with a routing mark: ApplyDamage is BlueprintAuthorityOnly, PlaySound2D BlueprintCosmetic. The editor
calls them with EX_FinalFunction on the class default object, so CallFunction's callspace check skips ApplyDamage on a
client and PlaySound2D on a dedicated server (ScriptCore.cpp:975-1034). EX_CallMath (execCallMath) and the Local*
opcodes skip that check: both would run on every machine.
*/
class FuncCosmeticStatic : public AActor {
public:
  void Hit(class AActor *A) { UGameplayStatics::ApplyDamage(A, 1.f, nullptr, this, nullptr); }
  void Beep() { UGameplayStatics::PlaySound2D(this, nullptr, 1.f, 1.f, 0.f, nullptr, nullptr, true); }
};
