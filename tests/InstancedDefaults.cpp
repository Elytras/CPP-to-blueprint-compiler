#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"
#include "UeApi/Game/STE_Spider_Shooter_Acid_C.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/InstancedDefaults");

/*
Objects instanced in a class's defaults, the editor's Instanced values. Each is an export under the object whose
property points at it, the property's tag its index. A new one stands on its class's CDO; one under another (Impulse,
under Burn) is that object's own.
*/
class InstancedDefaults : public UStatusEffect {
  UE_DEFAULTS {
    UDotStatusEffectItem *Burn = UE_INSTANCED(UDotStatusEffectItem, this, "Burn");
    Burn->damageAmount = FRandRange{2.0f, 3.0f};
    Burn->DamageImpulseOverride = UE_INSTANCED(UDamageImpulse, Burn, "Impulse");
    StatusEffects = {Burn, UE_INSTANCED(UStatChangeStatusEffectItem, this, "Slow")};
  }
};

/* Named as the base's, the kid's Burn is its own copy of the base's: archetyped on it, so the damage the kid leaves
   alone is still the base's. */
class InstancedDefaultsKid : public InstancedDefaults {
  UE_DEFAULTS {
    UDotStatusEffectItem *Burn = UE_INSTANCED(UDotStatusEffectItem, this, "Burn");
    Burn->OverrideDamageImpulse = true;
    StatusEffects = {Burn};
  }
};

/* A game Blueprint's: STE_Spider_Shooter_Acid_C's DotStatusEffectItem_0, which UeApi lists under its CDO. An
   object under a game Blueprint's component template needs a UeApi from genueapi 4, so test_bytecode.py compiles
   that case itself (instanced_defaults). */
class InstancedDefaultsAcid : public Game::Enemies::Spider::Shooter::STE_Spider_Shooter_Acid_C {
  UE_DEFAULTS {
    UDotStatusEffectItem *Acid = UE_INSTANCED(UDotStatusEffectItem, this, "DotStatusEffectItem_0");
    Acid->damageAmount = FRandRange{9.0f, 9.0f};
    StatusEffects = {Acid};
  }
};
