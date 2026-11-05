/*
ParticleStarts.cpp - starting values for distributions a mod leaves unset.

P_Starts9 has exactly nine objects, so the first starting distribution appended for its Spawn module grows the object
list past its capacity (a 1.5x vector: 1, 2, 3, 4, 6, 9); each of Spawn's three must still be filled. P_Sensible
leaves a Light and a Lifetime module wholly unset: they get what Cascade's editor gives a new module, InitializeDefaults
then SetToSensibleDefaults (ParticleModules.cpp), not InitializeDefaults' zeros.

P_Every leaves every module of it unset: a base class's start (a Sphere's VelocityScale is PrimitiveBase's), the
curves SetToSensibleDefaults makes (ColorOverLife, SubUV under SubUVMovie), an event receiver, a white StartColor,
and the bools it sets against the class default (Required's bUseLegacyEmitterTime, an acceleration's
bAlwaysInWorldSpace). Acc2 sets bAlwaysInWorldSpace false itself, which stays.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ParticleStarts");

UE_ASSET_DEFAULTS(UParticleSystem, P_Starts9) {
  UParticleSpriteEmitter *E = UE_INSTANCED(UParticleSpriteEmitter, this, "E");
  UParticleLODLevel *L = UE_INSTANCED(UParticleLODLevel, E, "L");
  UParticleModuleSpawn *Sp = UE_INSTANCED(UParticleModuleSpawn, this, "Sp");   // all three distributions unset
  UParticleModuleRequired *Req = UE_INSTANCED(UParticleModuleRequired, this, "Req");
  UDistributionFloatConstant *D = UE_INSTANCED(UDistributionFloatConstant, Req, "D");
  D->Constant = 0.0f;
  Req->SpawnRate.Distribution = D;
  UParticleModuleLifetime *Life = UE_INSTANCED(UParticleModuleLifetime, this, "Life");
  UDistributionFloatConstant *D2 = UE_INSTANCED(UDistributionFloatConstant, Life, "D2");
  D2->Constant = 1.0f;
  Life->LifeTime.Distribution = D2;
  UParticleModuleSize *Sz = UE_INSTANCED(UParticleModuleSize, this, "Sz");
  UDistributionVectorConstant *D3 = UE_INSTANCED(UDistributionVectorConstant, Sz, "D3");
  D3->Constant = FVector(5.0f, 5.0f, 5.0f);
  Sz->StartSize.Distribution = D3;
  L->RequiredModule = Req;
  L->SpawnModule = Sp;
  L->Modules = {Life, Sz};
  E->LODLevels = {L};
  Emitters = {E};
  LODDistances = {0.0f};
}

UE_ASSET_DEFAULTS(UParticleSystem, P_Sensible) {
  UParticleSpriteEmitter *E = UE_INSTANCED(UParticleSpriteEmitter, this, "E");
  UParticleLODLevel *L = UE_INSTANCED(UParticleLODLevel, E, "L");
  UParticleModuleRequired *Req = UE_INSTANCED(UParticleModuleRequired, this, "Req");
  UParticleModuleSpawn *Sp = UE_INSTANCED(UParticleModuleSpawn, this, "Sp");
  UParticleModuleLifetime *Life = UE_INSTANCED(UParticleModuleLifetime, this, "Life");   // unset
  UParticleModuleLight *Light = UE_INSTANCED(UParticleModuleLight, this, "Light");        // unset
  L->RequiredModule = Req;
  L->SpawnModule = Sp;
  L->Modules = {Life, Light};
  E->LODLevels = {L};
  Emitters = {E};
  LODDistances = {0.0f};
}

UE_ASSET_DEFAULTS(UParticleSystem, P_Every) {
  UParticleSpriteEmitter *E = UE_INSTANCED(UParticleSpriteEmitter, this, "E");
  UParticleLODLevel *L = UE_INSTANCED(UParticleLODLevel, E, "L");
  UParticleModuleRequired *Req = UE_INSTANCED(UParticleModuleRequired, this, "Req");
  UParticleModuleSpawn *Sp = UE_INSTANCED(UParticleModuleSpawn, this, "Sp");
  UParticleModuleLocationPrimitiveSphere *Sphere = UE_INSTANCED(UParticleModuleLocationPrimitiveSphere, this, "Sphere");
  UParticleModuleColor *Col = UE_INSTANCED(UParticleModuleColor, this, "Col");
  UParticleModuleColorOverLife *Fade = UE_INSTANCED(UParticleModuleColorOverLife, this, "Fade");
  UParticleModuleSubUVMovie *Movie = UE_INSTANCED(UParticleModuleSubUVMovie, this, "Movie");
  UParticleModuleEventReceiverSpawn *Recv = UE_INSTANCED(UParticleModuleEventReceiverSpawn, this, "Recv");
  UParticleModuleAccelerationDrag *Acc = UE_INSTANCED(UParticleModuleAccelerationDrag, this, "Acc");
  UParticleModuleAcceleration *Acc2 = UE_INSTANCED(UParticleModuleAcceleration, this, "Acc2");
  Acc2->bAlwaysInWorldSpace = false;
  L->RequiredModule = Req;
  L->SpawnModule = Sp;
  L->Modules = {Sphere, Col, Fade, Movie, Recv, Acc, Acc2};
  E->LODLevels = {L};
  Emitters = {E};
  LODDistances = {0.0f};
}
