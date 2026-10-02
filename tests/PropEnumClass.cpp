/*
PropEnumClass.cpp - a native `enum class : uint8` (UENUM, ECppForm::EnumClass) wherever a type lands: a variable, a
parameter, a return value, a local, a container's element and key, a UE_STRUCT member (and a UE_STRUCT's empty array,
set and map of it, whose tags hold counts only), a delegate's parameter, an override of a native event, a native
struct's member, and an inherited default.

The editor makes such a pin an EnumProperty over a ByteProperty named UnderlyingType, and a plain or namespaced enum a
ByteProperty with its Enum (Editor/KismetCompiler/Private/KismetCompilerMisc.cpp 1071-1094); the game's cooks carry an
EnumProperty for every enum class. Tags follow the property: an EnumProperty tag naming the enum. EAttachmentRule and
EActorUpdateOverlapsMethod are both `enum class : uint8` in UE 4.27 (EngineTypes.h 57, Actor.h 38); EAttachLocation is
a namespaced enum, which stays a ByteProperty. EAbilityIndex's one property in the game is ABosco's
UsePlayerActivatedAbillity parameter `Index`, an EnumProperty that the SDK spells `Index_0`: its form is read through
that respelling.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PropEnumClass");

struct FRuleSlot {
  UE_STRUCT;
  EAttachmentRule Rule = EAttachmentRule::SnapToTarget;
  int32 Weight = 2;
  TArray<EAttachmentRule> Vis;      // empty: the default instance still tags each one
  TSet<EAttachmentRule> Met;
  TMap<EAttachmentRule, int32> Toll;
};

class PropEnumClass : public AActor {
public:
  EAttachmentRule Rule = EAttachmentRule::KeepWorld;
  EAttachLocation Where = EAttachLocation::SnapToTarget;
  EAbilityIndex Ability = EAbilityIndex::ESecondary;
  TArray<EAttachmentRule> Order = {EAttachmentRule::SnapToTarget, EAttachmentRule::KeepRelative};
  TSet<EAttachmentRule> Seen;
  TMap<EAttachmentRule, int32> Cost = {{EAttachmentRule::KeepWorld, 3}};
  FRuleSlot Slot;
  FCameraShakeDuration Shake = {1.5f, ECameraShakeDurationType::Custom};
  TDelegate<void(EAttachmentRule)> OnRule;

  EAttachmentRule Pick(EAttachmentRule In) { return In; }
  void Take(EAttachmentRule R) { Rule = R; }
  void Arm() { OnRule = {this, &PropEnumClass::Take}; }

  /* Run offline: a local, an array of the enum, a switch on an element, a compare with a variable and a cast. */
  int32 Score(EAttachmentRule In, int32 M) {
    EAttachmentRule Local = M == 0 ? In : EAttachmentRule::SnapToTarget;
    TArray<EAttachmentRule> List = {Local, EAttachmentRule::KeepWorld};
    switch (List[0]) {
    case EAttachmentRule::KeepRelative: return 10 + List.Num();
    case EAttachmentRule::KeepWorld: return 20 + (int32)List[1];
    default: break;
    }
    return Local == Rule ? 100 : (int32)Local;
  }

  /* A map keyed by the enum. */
  int32 Weight(EAttachmentRule K) {
    TMap<EAttachmentRule, int32> W;
    W.Add(EAttachmentRule::KeepWorld, 3);
    W.Add(EAttachmentRule::SnapToTarget, 5);
    int32 V = 0;
    return W.Find(K, V) ? V : -1;
  }

  /* A native struct built with the enum in it, and read back. */
  int32 ShakeType(int32 M) {
    ECameraShakeDurationType T = M == 0 ? ECameraShakeDurationType::Fixed : ECameraShakeDurationType::Infinite;
    FCameraShakeDuration D = {2.0f, T};
    return (int32)D.Type;
  }

  UE_DEFAULTS { UpdateOverlapsMethodDuringLevelStreaming = EActorUpdateOverlapsMethod::AlwaysUpdate; }
};

/* An override of a native event whose parameter is an enum class takes the native's EnumProperty. */
class PropEnumCrystal : public ACoreCorruptionCrystal {
public:
  ECoreCorruptionCrystalState Last;
  void Receive_EnteredState(ECoreCorruptionCrystalState State) { Last = State; }
};
