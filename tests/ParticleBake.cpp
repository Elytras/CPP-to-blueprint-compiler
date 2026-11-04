/*
ParticleBake.cpp - a Cascade particle system, cooked as DRG's cooker writes one.

Each raw distribution a module points at a Constant or Uniform distribution object is baked into its lookup table
(FRawDistribution::Table), the object itself left out of the package; a raw distribution the mod leaves unset gets
its module's starting value, as Cascade's editor gives a new module (InitializeDefaults), baked the same way.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ParticleBake");

UE_ASSET_DEFAULTS(UParticleSystem, P_Bake) {
  UParticleSpriteEmitter *Sparks = UE_INSTANCED(UParticleSpriteEmitter, this, "Sparks");
  UParticleLODLevel *Lod = UE_INSTANCED(UParticleLODLevel, Sparks, "LOD0");

  UParticleModuleRequired *Required = UE_INSTANCED(UParticleModuleRequired, this, "Required");
  UParticleModuleSpawn *Spawn = UE_INSTANCED(UParticleModuleSpawn, this, "Spawn");
  Spawn->BurstList = {FParticleBurst(4, -1, 0.0f)};
  UDistributionFloatConstant *Three = UE_INSTANCED(UDistributionFloatConstant, Spawn, "Three");   // shared below
  Three->Constant = 3.0f;
  Spawn->Rate.Distribution = Three;
  Spawn->RateScale.Distribution = Three;

  UParticleModuleColor *Color = UE_INSTANCED(UParticleModuleColor, this, "Color");
  UDistributionVectorConstant *Tint = UE_INSTANCED(UDistributionVectorConstant, Color, "Tint");
  Tint->Constant = FVector(0.0f, 41.67f, 500.0f);
  Color->StartColor.Distribution = Tint;
  UDistributionFloatConstant *Alpha = UE_INSTANCED(UDistributionFloatConstant, Color, "Alpha");
  Alpha->Constant = 1.0f;
  Color->StartAlpha.Distribution = Alpha;

  UParticleModuleLifetime *Life = UE_INSTANCED(UParticleModuleLifetime, this, "Life");
  UDistributionFloatUniform *Span = UE_INSTANCED(UDistributionFloatUniform, Life, "Span");
  Span->Min = 0.5f;
  Span->Max = 1.15f;
  Life->LifeTime.Distribution = Span;

  UParticleModuleSize *Size = UE_INSTANCED(UParticleModuleSize, this, "Size");
  UDistributionVectorUniform *Grow = UE_INSTANCED(UDistributionVectorUniform, Size, "Grow");
  Grow->Min = FVector(1.0f, 1.0f, 1.0f);
  Grow->Max = FVector(3.0f, 3.0f, 3.0f);
  Size->StartSize.Distribution = Grow;

  Lod->RequiredModule = Required;
  Lod->SpawnModule = Spawn;
  Lod->Modules = {Color, Life, Size};
  Sparks->LODLevels = {Lod};
  Emitters = {Sparks};
  LODDistances = {0.0f};
}
