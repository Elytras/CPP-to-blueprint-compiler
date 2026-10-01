#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/OpndCallspace");

/*
A static library function marked BlueprintAuthorityOnly (ApplyDamage) or BlueprintCosmetic (PlaySound2D) must run only
where the engine's callspace says: CallFunction asks GetFunctionCallspace for such a function, and on the library's
CDO that is UEngine::GetGlobalFunctionCallspace, which absorbs an authority-only call on a client and a cosmetic one on
a dedicated server (ScriptCore.cpp 975-1030, BlueprintFunctionLibrary.cpp 20, UnrealEngine.cpp 14837). EX_CallMath runs
the thunk straight on the CDO and never asks (ScriptCore.cpp 937-958), which is why the editor calls these with a
context on Default__GameplayStatics instead (KismetCompilerVMBackend.cpp 1255-1258). Abs has no callspace flag.
*/
class OpndCallspace : public AActor {
public:
  float Damage(AActor *Target) { return UGameplayStatics::ApplyDamage(Target, 2.0f, nullptr, this, nullptr); }
  void  Sound() { UGameplayStatics::PlaySound2D(nullptr, 0.5f, 1.0f, 0.0f, nullptr, nullptr, false); }
  float Plain(float X) { return UKismetMathLibrary::Abs(X); }
};
