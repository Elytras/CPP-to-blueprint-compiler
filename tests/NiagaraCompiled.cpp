/*
NiagaraCompiled.cpp - a Niagara system's EmitterCompiledData, which the editor works out from the compiled scripts and
UNiagaraSystem::Serialize writes after the system's tags (AssetGen/Niagara.cpp).

NS_Two: Sparks' particle scripts hold an attribute of each register kind (float, vector, colour, int pair, halves), its
spawn script repeating the update script's Age; the system scripts hold three spawn infos named "Sparks." (one spelled
"sparks.", one listed twice) and Glow's. Glow asks for persistent IDs itself, Sparks through its Particles.ID; Glow's
Rand is a seed triple (three ints). Old is a disabled GPU emitter, which the cook leaves out: its compiled data keeps its
name, sim target and persistent IDs.

NS_Off: a disabled GPU emitter's compiled GPU script (empty, as the editor leaves a disabled emitter's), left out with it,
so its shader maps are not needed.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/NiagaraCompiled");

UE_ASSET_DEFAULTS(UNiagaraSystem, NS_Two) {
  UNiagaraEmitter *Sparks = UE_INSTANCED(UNiagaraEmitter, this, "Sparks");
  Sparks->UniqueEmitterName = "Sparks";
  UNiagaraScript *SU = UE_INSTANCED(UNiagaraScript, Sparks, "SU");
  SU->Usage = ENiagaraScriptUsage::ParticleUpdateScript;
  SU->CachedScriptVM.Attributes = {FNiagaraVariable("Age", FNiagaraTypeDefinition(StaticStructOf<FNiagaraFloat>(), 2), {}),
                                   FNiagaraVariable("Color", FNiagaraTypeDefinition(StaticStructOf<FLinearColor>(), 2), {})};
  SU->CachedScriptVM.LastCompileStatus = ENiagaraScriptCompileStatus::NCS_UpToDate;
  Sparks->UpdateScriptProps.Script = SU;
  UNiagaraScript *SS = UE_INSTANCED(UNiagaraScript, Sparks, "SS");
  SS->Usage = ENiagaraScriptUsage::ParticleSpawnScript;
  SS->CachedScriptVM.Attributes = {FNiagaraVariable("Age", FNiagaraTypeDefinition(StaticStructOf<FNiagaraFloat>(), 2), {}),
                                   FNiagaraVariable("Position", FNiagaraTypeDefinition(StaticStructOf<FVector>(), 2), {}),
                                   FNiagaraVariable("Particles.ID", FNiagaraTypeDefinition(StaticStructOf<FNiagaraID>(), 2), {}),
                                   FNiagaraVariable("Pad", FNiagaraTypeDefinition(StaticStructOf<FNiagaraHalfVector2>(), 2), {})};
  SS->CachedScriptVM.LastCompileStatus = ENiagaraScriptCompileStatus::NCS_UpToDate;
  Sparks->SpawnScriptProps.Script = SS;

  UNiagaraEmitter *Glow = UE_INSTANCED(UNiagaraEmitter, this, "Glow");
  Glow->UniqueEmitterName = "Glow";
  Glow->bRequiresPersistentIDs = true;
  UNiagaraScript *GU = UE_INSTANCED(UNiagaraScript, Glow, "GU");
  GU->Usage = ENiagaraScriptUsage::ParticleUpdateScript;
  GU->CachedScriptVM.Attributes = {FNiagaraVariable("Age", FNiagaraTypeDefinition(StaticStructOf<FNiagaraFloat>(), 2), {}),
                                   FNiagaraVariable("Rand", FNiagaraTypeDefinition(StaticStructOf<FNiagaraRandInfo>(), 2), {})};
  GU->CachedScriptVM.LastCompileStatus = ENiagaraScriptCompileStatus::NCS_UpToDate;
  Glow->UpdateScriptProps.Script = GU;

  UNiagaraScript *Spawn = UE_INSTANCED(UNiagaraScript, this, "Spawn");
  Spawn->Usage = ENiagaraScriptUsage::SystemSpawnScript;
  Spawn->CachedScriptVM.Attributes = {FNiagaraVariable("Sparks.Burst", FNiagaraTypeDefinition(StaticStructOf<FNiagaraSpawnInfo>(), 2), {}),
                                      FNiagaraVariable("Glow.Rate", FNiagaraTypeDefinition(StaticStructOf<FNiagaraFloat>(), 2), {}),
                                      FNiagaraVariable("Glow.Spawn", FNiagaraTypeDefinition(StaticStructOf<FNiagaraSpawnInfo>(), 2), {})};
  Spawn->CachedScriptVM.LastCompileStatus = ENiagaraScriptCompileStatus::NCS_UpToDate;
  UNiagaraScript *Update = UE_INSTANCED(UNiagaraScript, this, "Update");
  Update->Usage = ENiagaraScriptUsage::SystemUpdateScript;
  Update->CachedScriptVM.Attributes = {FNiagaraVariable("Sparks.Rate", FNiagaraTypeDefinition(StaticStructOf<FNiagaraSpawnInfo>(), 2), {}),
                                       FNiagaraVariable("Sparks.Burst", FNiagaraTypeDefinition(StaticStructOf<FNiagaraSpawnInfo>(), 2), {}),
                                       FNiagaraVariable("sparks.Late", FNiagaraTypeDefinition(StaticStructOf<FNiagaraSpawnInfo>(), 2), {})};
  Update->CachedScriptVM.LastCompileStatus = ENiagaraScriptCompileStatus::NCS_UpToDate;
  SystemSpawnScript = Spawn;
  SystemUpdateScript = Update;
  UNiagaraEmitter *Old = UE_INSTANCED(UNiagaraEmitter, this, "Old");
  Old->UniqueEmitterName = "Old";
  Old->SimTarget = ENiagaraSimTarget::GPUComputeSim;
  Old->bRequiresPersistentIDs = true;
  EmitterHandles = {FNiagaraEmitterHandle(FGuid(1, 2, 3, 4), "IdSparks", true, "Sparks", Sparks),
                    FNiagaraEmitterHandle(FGuid(5, 6, 7, 8), "IdGlow", true, "Glow", Glow),
                    FNiagaraEmitterHandle(FGuid(9, 10, 11, 12), "IdOld", false, "Old", Old)};
}

UE_ASSET_DEFAULTS(UNiagaraSystem, NS_Off) {
  UNiagaraEmitter *Off = UE_INSTANCED(UNiagaraEmitter, this, "Off");
  Off->UniqueEmitterName = "Off";
  Off->SimTarget = ENiagaraSimTarget::GPUComputeSim;
  UNiagaraScript *G = UE_INSTANCED(UNiagaraScript, Off, "G");
  G->Usage = ENiagaraScriptUsage::ParticleGPUComputeScript;
  G->CachedScriptVM.LastCompileStatus = ENiagaraScriptCompileStatus::NCS_UpToDate;
  Off->GPUComputeScript = G;
  UNiagaraScript *Spawn = UE_INSTANCED(UNiagaraScript, this, "Spawn");
  Spawn->CachedScriptVM.LastCompileStatus = ENiagaraScriptCompileStatus::NCS_UpToDate;
  UNiagaraScript *Update = UE_INSTANCED(UNiagaraScript, this, "Update");
  Update->CachedScriptVM.LastCompileStatus = ENiagaraScriptCompileStatus::NCS_UpToDate;
  SystemSpawnScript = Spawn;
  SystemUpdateScript = Update;
  EmitterHandles = {FNiagaraEmitterHandle(FGuid(1, 2, 3, 4), "IdOff", false, "Off", Off)};
}
