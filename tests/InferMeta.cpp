#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/InferMeta");

/*
What a mod's own types no longer have to say. A plain struct, or a `: uint8` enum, that a reflected position names is
cooked as UE_STRUCT / UE_ENUM would cook it; one only C++ reads stays plain. A component is a pointer member the
constructor sets with CreateDefaultSubobject, an instanced default a NewObject with a name, and a const method that
returns a value is BlueprintPure.
*/
struct FInferScore {
  int32 Points;
  float Mult = 2.0f;
};

struct FInferBox {          // used only through InferMeta's Box, and it makes FInferScore used too
  FInferScore Inner;
  int32 Tag;
};

struct FInferCppOnly {      // only a constexpr reads it: no asset
  int32 X;
};
constexpr FInferCppOnly kInferTable = {5};

enum class EInferRank : uint8 { Low, Mid, High };
enum class EInferUnused : uint8 { A, B };

class InferMeta : public AActor {
  FInferBox Box;
  USceneComponent *Pivot = CreateDefaultSubobject<USceneComponent>("Pivot");
  UStaticMeshComponent *Mesh;

public:
  InferMeta() {
    Mesh = CreateDefaultSubobject<UStaticMeshComponent>("Mesh");
    Mesh->SetupAttachment(Pivot);
    Mesh->bVisible = false;
  }

  int32 RankValue(EInferRank R) { return R == EInferRank::High ? 10 : R == EInferRank::Mid ? 5 : 1; }
  int32 BoxPoints(int32 P) {
    FInferBox B = {.Inner = {.Points = P}, .Tag = 5};
    return B.Inner.Points + B.Tag;
  }
  int32 Doubled(int32 P) const { return P * 2; }
  void Touch() const {}
  UE_PURE static int32 Tripled(int32 P) { return P * 3; }
};

class InferMetaKid : public InferMeta {
public:
  int32 Doubled(int32 P) const { return P * 4; }      // an override keeps its parent's flags
};

class InferMetaEffect : public UStatusEffect {
public:
  InferMetaEffect() {
    UDotStatusEffectItem *Burn = NewObject<UDotStatusEffectItem>(this, "Burn");
    Burn->damageAmount = FRandRange{2.0f, 3.0f};
    StatusEffects = {Burn, NewObject<UStatChangeStatusEffectItem>(this, "Slow")};
  }
};
