/*
ParticleCurves.cpp - curve distributions, baked into lookup tables as the cook bakes them.

One module per case, each raw distribution a curve: a linear ramp (two entries), a lone point away from 0 (TimeBias),
an empty curve (zero), a uniform curve (min and max per entry), auto tangents the editor would have set (the source
writes them 0), clamped auto tangents, steps (no smaller table is close enough: all 128 samples kept), a looped
curve, and a uniform vector curve with extremes. A BeamNoise module leaves NoiseScale unset: the editor starts it as
a ConstantCurve without keys.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ParticleCurves");

UE_ASSET_DEFAULTS(UParticleSystem, P_Curves) {
  UParticleSpriteEmitter *E = UE_INSTANCED(UParticleSpriteEmitter, this, "E");
  UParticleLODLevel *L = UE_INSTANCED(UParticleLODLevel, E, "L");
  UParticleModuleRequired *Req = UE_INSTANCED(UParticleModuleRequired, this, "Req");

  UParticleModuleSpawn *Sp = UE_INSTANCED(UParticleModuleSpawn, this, "Sp");
  UDistributionFloatConstantCurve *Ramp = UE_INSTANCED(UDistributionFloatConstantCurve, Sp, "Ramp");
  Ramp->ConstantCurve.Points = {{0.0f, 0.0f, 0.0f, 0.0f, EInterpCurveMode::CIM_Linear},
                                {1.0f, 10.0f, 0.0f, 0.0f, EInterpCurveMode::CIM_Linear}};
  Sp->Rate.Distribution = Ramp;
  UDistributionFloatConstantCurve *Lone = UE_INSTANCED(UDistributionFloatConstantCurve, Sp, "Lone");
  Lone->ConstantCurve.Points = {{0.5f, 3.0f, 0.0f, 0.0f, EInterpCurveMode::CIM_Linear}};
  Sp->RateScale.Distribution = Lone;
  UDistributionFloatConstantCurve *Empty = UE_INSTANCED(UDistributionFloatConstantCurve, Sp, "Empty");
  Sp->BurstScale.Distribution = Empty;

  UParticleModuleLifetime *Life = UE_INSTANCED(UParticleModuleLifetime, this, "Life");
  UDistributionFloatUniformCurve *Span = UE_INSTANCED(UDistributionFloatUniformCurve, Life, "Span");
  Span->ConstantCurve.Points = {{0.0f, FVector2D(1.0f, 2.0f), FVector2D(), FVector2D(), EInterpCurveMode::CIM_Linear},
                                {2.0f, FVector2D(3.0f, 6.0f), FVector2D(), FVector2D(), EInterpCurveMode::CIM_Linear}};
  Life->LifeTime.Distribution = Span;

  UParticleModuleSize *Sz = UE_INSTANCED(UParticleModuleSize, this, "Sz");
  UDistributionVectorConstantCurve *Hump = UE_INSTANCED(UDistributionVectorConstantCurve, Sz, "Hump");
  Hump->ConstantCurve.Points = {{0.0f, FVector(0, 0, 0), FVector(), FVector(), EInterpCurveMode::CIM_CurveAuto},
                                {0.5f, FVector(10, 20, 10), FVector(), FVector(), EInterpCurveMode::CIM_CurveAuto},
                                {1.0f, FVector(0, 40, 30), FVector(), FVector(), EInterpCurveMode::CIM_CurveAuto}};
  Hump->LockedAxes = EDistributionVectorLockFlags::EDVLF_XY;
  Sz->StartSize.Distribution = Hump;

  UParticleModuleVelocity *Vel = UE_INSTANCED(UParticleModuleVelocity, this, "Vel");
  UDistributionVectorUniformCurve *Kick = UE_INSTANCED(UDistributionVectorUniformCurve, Vel, "Kick");
  Kick->ConstantCurve.Points = {{0.5f, FTwoVectors(FVector(1, 2, 3), FVector(4, 5, 6)), FTwoVectors(), FTwoVectors(),
                                 EInterpCurveMode::CIM_Linear}};
  Kick->bUseExtremes = true;
  Vel->StartVelocity.Distribution = Kick;
  UDistributionFloatConstantCurve *Clamped = UE_INSTANCED(UDistributionFloatConstantCurve, Vel, "Clamped");
  Clamped->ConstantCurve.Points = {{0.0f, 0.0f, 0.0f, 0.0f, EInterpCurveMode::CIM_CurveAutoClamped},
                                   {1.0f, 1.0f, 0.0f, 0.0f, EInterpCurveMode::CIM_CurveAutoClamped},
                                   {2.0f, 4.0f, 0.0f, 0.0f, EInterpCurveMode::CIM_CurveAutoClamped},
                                   {3.0f, 6.0f, 0.0f, 0.0f, EInterpCurveMode::CIM_CurveAutoClamped}};
  Vel->StartVelocityRadial.Distribution = Clamped;

  UParticleModuleColor *Col = UE_INSTANCED(UParticleModuleColor, this, "Col");
  UDistributionVectorConstantCurve *Steps = UE_INSTANCED(UDistributionVectorConstantCurve, Col, "Steps");
  Steps->ConstantCurve.Points = {{0.0f, FVector(1, 0, 0), FVector(), FVector(), EInterpCurveMode::CIM_Constant},
                                 {1.0f, FVector(0, 1, 0), FVector(), FVector(), EInterpCurveMode::CIM_Constant},
                                 {2.0f, FVector(0, 0, 1), FVector(), FVector(), EInterpCurveMode::CIM_Constant}};
  Col->StartColor.Distribution = Steps;
  UDistributionFloatConstantCurve *Loop = UE_INSTANCED(UDistributionFloatConstantCurve, Col, "Loop");
  Loop->ConstantCurve.Points = {{0.0f, 0.0f, 0.0f, 0.0f, EInterpCurveMode::CIM_CurveAuto},
                                {0.5f, 1.0f, 0.0f, 0.0f, EInterpCurveMode::CIM_CurveAuto},
                                {1.0f, 0.5f, 0.0f, 0.0f, EInterpCurveMode::CIM_CurveAuto}};
  Loop->ConstantCurve.bIsLooped = true;
  Loop->ConstantCurve.LoopKeyOffset = 1.0f;
  Col->StartAlpha.Distribution = Loop;

  L->RequiredModule = Req;
  L->SpawnModule = Sp;
  UParticleModuleBeamNoise *Noise = UE_INSTANCED(UParticleModuleBeamNoise, this, "Noise");   // NoiseScale unset

  L->Modules = {Life, Sz, Vel, Col, Noise};
  E->LODLevels = {L};
  Emitters = {E};
  LODDistances = {0.0f};
}
