/*
ParticleEvents.cpp - Cascade event modules: one emitter generates, another spawns where it does.

P_Events: Sparks lists its EventGenerator in Modules only, as a mod may; the LOD level's EventGenerator slot is then
filled as the editor keeps it. Burst receives the generator's collision events by name and spawns 5 particles on each;
its InheritVelocityScale is left unset and starts at (1, 1, 1).
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ParticleEvents");

UE_ASSET_DEFAULTS(UParticleSystem, P_Events) {
  UParticleSpriteEmitter *Sparks = UE_INSTANCED(UParticleSpriteEmitter, this, "Sparks");
  UParticleLODLevel *SL = UE_INSTANCED(UParticleLODLevel, Sparks, "SL");
  UParticleModuleRequired *SReq = UE_INSTANCED(UParticleModuleRequired, this, "SReq");
  UParticleModuleSpawn *SSp = UE_INSTANCED(UParticleModuleSpawn, this, "SSp");
  UParticleModuleCollision *Hit = UE_INSTANCED(UParticleModuleCollision, this, "Hit");
  UParticleModuleEventGenerator *Gen = UE_INSTANCED(UParticleModuleEventGenerator, this, "Gen");
  Gen->Events = {FParticleEvent_GenerateInfo(EParticleEventType::EPET_Collision, 0, 0, false, false, false, false, "SparkHit", {})};
  SL->RequiredModule = SReq;
  SL->SpawnModule = SSp;
  SL->Modules = {Gen, Hit};
  Sparks->LODLevels = {SL};
  UParticleSpriteEmitter *Burst = UE_INSTANCED(UParticleSpriteEmitter, this, "Burst");
  UParticleLODLevel *BL = UE_INSTANCED(UParticleLODLevel, Burst, "BL");
  UParticleModuleRequired *BReq = UE_INSTANCED(UParticleModuleRequired, this, "BReq");
  UParticleModuleSpawn *BSp = UE_INSTANCED(UParticleModuleSpawn, this, "BSp");
  UParticleModuleEventReceiverSpawn *Recv = UE_INSTANCED(UParticleModuleEventReceiverSpawn, this, "Recv");
  Recv->EventGeneratorType = EParticleEventType::EPET_Collision;
  Recv->EventName = "SparkHit";
  UDistributionFloatConstant *Five = UE_INSTANCED(UDistributionFloatConstant, Recv, "Five");
  Five->Constant = 5.0f;
  Recv->SpawnCount.Distribution = Five;
  BL->RequiredModule = BReq;
  BL->SpawnModule = BSp;
  BL->Modules = {Recv};
  Burst->LODLevels = {BL};
  Emitters = {Sparks, Burst};
  LODDistances = {0.0f};
}
