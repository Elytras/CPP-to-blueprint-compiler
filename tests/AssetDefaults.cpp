/*
AssetDefaults.cpp - UE_ASSET_DEFAULTS: an asset whose defaults are statements, so it can hold objects of its own.

UE_ASSET_DEFAULTS(Type, Name) { ... } declares the asset Name, cooked at <mod package>/<namespace folders>/<Name> as a
braced asset is, and its block sets the asset's members as a class's UE_DEFAULTS sets its own: `this` is the asset.
UE_INSTANCED makes an object under the asset, or under another such object, and a default points at it. A particle
system's emitters, LOD levels and modules are such objects; so are a behaviour tree's nodes.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetDefaults");

class UHolder : public UPrimaryDataAsset {
public:
  UObject *Inner = nullptr;
  TArray<UObject *> Items;
  float Scale = 1;
};

UE_ASSET_DEFAULTS(UHolder, AD_Held) {
  UDotStatusEffectItem *A = UE_INSTANCED(UDotStatusEffectItem, this, "A");
  A->OverrideDamageImpulse = true;
  A->DamageImpulseOverride = UE_INSTANCED(UDamageImpulse, A, "Impulse");
  Items = {A, UE_INSTANCED(UStatChangeStatusEffectItem, this, "B")};
  Inner = A;
  Scale = 2.5f;
}

namespace Folder {
UE_ASSET_DEFAULTS(UHolder, AD_Deep) { Scale = 4.0f; }
}

class AssetDefaults : public AActor {
public:
  UHolder *Picked = &AD_Held;
  UHolder *Deep = &Folder::AD_Deep;
};
