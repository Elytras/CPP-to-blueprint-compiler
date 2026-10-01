#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DerivedLiteral");

/*
Whole-struct literals of native structs with a super or a Transient member. execStructConst steps one expression
into each property of the struct's PropertyLink - its own properties first, then its super's - skipping Transient
ones (ScriptCore.cpp 3376-3405; Class.cpp 944-982). So FLightmassDirectionalLightSettings (own LightSourceAngle;
super IndirectLightingSaturation, ShadowExponent, bUseAreaShadowsForStationaryLight) takes 4.5 first; a
FMaterialAttributesInput has three members to write (its own PropertyConnectedBitmask is Transient, given 0 here: a
value other than zero would make the literal a Make Struct), and an FTimerHandle none (Handle is Transient).
*/
class DerivedLiteral : public AActor {
public:
  FTimerHandle Handle;

  float Angle() {
    FLightmassDirectionalLightSettings S = FLightmassDirectionalLightSettings(1.5f, 2.5f, true, 4.5f);
    return S.LightSourceAngle;
  }
  int32 Output() {
    FMaterialAttributesInput S = FMaterialAttributesInput(3, FName("In"), FName("Ex"), 0);
    return S.OutputIndex;
  }
  void ResetHandle() { Handle = FTimerHandle(); }
};
