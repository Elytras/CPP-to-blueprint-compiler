/*
ParticleLod.cpp - a Cascade particle system with two emitters of two LOD levels each.

Each module's LODValidity is the LOD levels that name it, as bits; a LOD level's Level is its place in its emitter's
LODLevels. A module whose distributions AssetGen has no starting values for, left unset, is warned about.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ParticleLod");

UE_ASSET_DEFAULTS(UParticleSystem, P_Lod) {
  UParticleSpriteEmitter *A = UE_INSTANCED(UParticleSpriteEmitter, this, "A");
  UParticleLODLevel *A0 = UE_INSTANCED(UParticleLODLevel, A, "A0");
  UParticleLODLevel *A1 = UE_INSTANCED(UParticleLODLevel, A, "A1");
  UParticleModuleRequired *ReqA = UE_INSTANCED(UParticleModuleRequired, this, "ReqA");     // both of A's levels
  UParticleModuleSpawn *SpawnA = UE_INSTANCED(UParticleModuleSpawn, this, "SpawnA");
  UParticleModuleLifetime *Near = UE_INSTANCED(UParticleModuleLifetime, this, "Near");     // A's level 1 only
  A0->RequiredModule = ReqA;
  A0->SpawnModule = SpawnA;
  A1->RequiredModule = ReqA;
  A1->SpawnModule = SpawnA;
  A1->Modules = {Near};
  A->LODLevels = {A0, A1};

  UParticleSpriteEmitter *B = UE_INSTANCED(UParticleSpriteEmitter, this, "B");
  UParticleLODLevel *B0 = UE_INSTANCED(UParticleLODLevel, B, "B0");
  UParticleLODLevel *B1 = UE_INSTANCED(UParticleLODLevel, B, "B1");
  UParticleModuleRequired *ReqB = UE_INSTANCED(UParticleModuleRequired, this, "ReqB");
  UParticleModuleSpawn *SpawnB0 = UE_INSTANCED(UParticleModuleSpawn, this, "SpawnB0");
  UParticleModuleSpawn *SpawnB1 = UE_INSTANCED(UParticleModuleSpawn, this, "SpawnB1");
  UParticleModuleAcceleration *Fall = UE_INSTANCED(UParticleModuleAcceleration, this, "Fall");   // left unset
  B0->RequiredModule = ReqB;
  B0->SpawnModule = SpawnB0;
  B0->Modules = {Fall, Near};
  B1->RequiredModule = ReqB;
  B1->SpawnModule = SpawnB1;
  B->LODLevels = {B0, B1};

  Emitters = {A, B};
  LODDistances = {0.0f, 1500.0f};
}
