/*
MaterialInstance.cpp - a material instance (UMaterialInstanceConstant) declared with UE_ASSET_DEFAULTS.

M_Fire is the game's M_FireBulkArmor of EEE as its cook wrote it: the parent, six scalars and two vectors, each
parameter's ParameterInfo and ExpressionGUID as the editor saved them. The suite compares the tags with that cook's.
M_Tex adds a texture parameter.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/MaterialInstance");

UE_ASSET_AT(::UMaterialInstanceConstant, M_UnknownArtifact_debris,
            "/Game/GameElements/Resources/Embedded/UnknownArtifact/M_UnknownArtifact_debris.M_UnknownArtifact_debris");
UE_ASSET_AT(::UTexture2D, T_Noise01, "/Game/Art/Textures/T_Noise01.T_Noise01");

UE_ASSET_DEFAULTS(UMaterialInstanceConstant, M_Fire) {
  Parent = &M_UnknownArtifact_debris;
  ScalarParameterValues = {
      FScalarParameterValue(FMaterialParameterInfo("Diffuse_Fresnel_Power", EMaterialParameterAssociation::GlobalParameter, -1), 1.5f, FGuid(0x70FF5D84, 0x42B2AB59, 0x15C10E88, 0xEBBF52D2)),
      FScalarParameterValue(FMaterialParameterInfo("EmissiveMultiplier", EMaterialParameterAssociation::GlobalParameter, -1), 1000.0f, FGuid(0x3007CAD9, 0x45DD7CEA, 0x6DF06380, 0x3D2C2215)),
      FScalarParameterValue(FMaterialParameterInfo("Metallic", EMaterialParameterAssociation::GlobalParameter, -1), 0.0f, FGuid(0xD7C7E8FB, 0x462990D7, 0x680E22AF, 0xA7C26F69)),
      FScalarParameterValue(FMaterialParameterInfo("NoiseSpeed", EMaterialParameterAssociation::GlobalParameter, -1), -225.0f, FGuid(0xB59E8B93, 0x49E3F8F5, 0x15582E8D, 0xF369B00C)),
      FScalarParameterValue(FMaterialParameterInfo("Subsurface Opacity", EMaterialParameterAssociation::GlobalParameter, -1), 1.0f, FGuid(0xBCBA2146, 0x49AABF7C, 0x7D895BBD, 0x7CD9D03F)),
      FScalarParameterValue(FMaterialParameterInfo("RefractionDepthBias", EMaterialParameterAssociation::GlobalParameter, -1), 0.0f, FGuid())};
  VectorParameterValues = {
      FVectorParameterValue(FMaterialParameterInfo("Diffuse_Edge", EMaterialParameterAssociation::GlobalParameter, -1), FLinearColor(6.0f, 0.653663993f, 0.0f, 1.0f),
                            FGuid(0x74030898, 0x41DEA1E5, 0x5F33ACB5, 0x2A4F6A6B)),
      FVectorParameterValue(FMaterialParameterInfo("Diffuse_Main", EMaterialParameterAssociation::GlobalParameter, -1), FLinearColor(0.0f, 0.0f, 0.0f, 1.0f),
                            FGuid(0xF973C881, 0x40A36105, 0x84F809A1, 0xF509D86E))};
}

UE_ASSET_DEFAULTS(UMaterialInstanceConstant, M_Tex) {
  Parent = &M_UnknownArtifact_debris;
  TextureParameterValues = {FTextureParameterValue(FMaterialParameterInfo("Noise", EMaterialParameterAssociation::GlobalParameter, -1), &T_Noise01, FGuid())};
}
