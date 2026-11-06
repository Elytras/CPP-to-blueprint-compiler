/*
BehaviorTree.cpp - a blackboard and a behaviour tree, written as the tree reads: each composite, then a block holding
its subtree, closed by its Children list. Each child names its decorators; each key selector names its key.

AssetGen keeps on each node what the editor keeps: TreeAsset, the tree, and ParentNode, the composite that holds it
(none on the root and on the tree's RootDecorators).
*/
#include "UeApi/Types.h"

#include "UeApi/AIModule.h"
#include "UeApi/Engine.h"
#include "UeApi/FSD.h"
#include "UeApi/Game/UTSK_FindRandomPointFrom_WithinHeight.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/BehaviorTree");

/* A native enum the blackboard's enum key is of, by its UEnum's path. */
UE_ASSET_AT(::UEnum, EWoodLouseState_Enum, "/Script/FSD.EWoodLouseState");

UE_ASSET_DEFAULTS(UBlackboardData, BB_Test) {
  auto *Target = UE_INSTANCED(UBlackboardKeyType_Object, this, "BlackboardKeyType_Object_0");
  Target->BaseClass = AActor::StaticClass();
  auto *State = UE_INSTANCED(UBlackboardKeyType_Enum, this, "BlackboardKeyType_Enum_0");
  State->EnumType = &EWoodLouseState_Enum;
  State->EnumName = "EWoodLouseState";
  Keys = {{"Target", Target, false},
          {"Destination", UE_INSTANCED(UBlackboardKeyType_Vector, this, "BlackboardKeyType_Vector_0"), false},
          {"IsStunned", UE_INSTANCED(UBlackboardKeyType_Bool, this, "BlackboardKeyType_Bool_0"), false},
          {"State", State, false}};
}

UE_ASSET_DEFAULTS(UBehaviorTree, BT_Test) {
  BlackboardAsset = &BB_Test;
  auto *Alive = UE_INSTANCED(UBTDecorator_Blackboard, this, "Alive");
  Alive->BlackboardKey.SelectedKeyName = "Target";
  RootDecorators = {Alive};

  auto *Root = UE_INSTANCED(UBTComposite_Selector, this, "Root");
  RootNode = Root;
  Root->Services = {UE_INSTANCED(UBTService_DefaultFocus, this, "Focus")};
  {
    auto *Stunned = UE_INSTANCED(UBTDecorator_Blackboard, this, "Stunned");
    Stunned->BlackboardKey.SelectedKeyName = "IsStunned";
    Stunned->FlowAbortMode = EBTFlowAbortMode::LowerPriority;
    auto *Recover = UE_INSTANCED(UBTTask_Wait, this, "Recover");
    Recover->WaitTime = 1.5f;

    auto *Roam = UE_INSTANCED(UBTComposite_Sequence, this, "Roam");
    Roam->NodeName = "Roam about";
    {
      auto *Pick = UE_INSTANCED(::Game::AI::Tasks::UTSK_FindRandomPointFrom_WithinHeight, this, "Pick");
      Pick->NewLocationKey.SelectedKeyName = "Destination";
      auto *Go = UE_INSTANCED(UBTTask_MoveToTarget, this, "Go");
      Go->BlackboardKey.SelectedKeyName = "Destination";
      Go->Services = {UE_INSTANCED(UBTService_DefaultFocus, this, "Watch")};
      Roam->Children = {{nullptr, Pick, {}, {}}, {nullptr, Go, {}, {}}};
    }
    auto *Near = UE_INSTANCED(UBTDecorator_InRange, this, "Near");
    Near->BlackboardKey.SelectedKeyName = "Destination";
    auto *Far = UE_INSTANCED(UBTDecorator_InRange, this, "Far");
    Far->bInverseCondition = true;
    Root->Children = {{nullptr, Recover, {Stunned}, {}},
                      {Roam, nullptr, {Near, Far}, {{EBTDecoratorLogic::Or, 2}, {EBTDecoratorLogic::Test, 0}, {EBTDecoratorLogic::Test, 1}}}};
  }
}
