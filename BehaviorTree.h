#pragma once
/*
Behaviour trees: what the behaviour tree editor keeps on a UBehaviorTree's nodes beside what the mod sets, done on the
tags a UE_ASSET_DEFAULTS block lowered (Cpp.cpp, FinishAssetDefaults' caller), before they are written.

Measured on DRG's 76 cooked trees (3053 nodes, btdecomp.py): every node carries TreeAsset, the tree, and ParentNode, the
composite that holds it. That composite is the parent of each child (task or composite) in its Children, of each child's
decorators, of its own Services and of a child task's Services. The root composite and the tree's RootDecorators have
none. The editor sets both on every save (BehaviorTreeGraph's CreateBTFromGraph); the engine sets them again when it
loads the tree (UBehaviorTreeManager::LoadTree), so they are written for a package equal to the editor's, not for the
game's sake.

Refused, as the editor never makes them and the engine would misbehave on them:
  - a child with neither ChildComposite nor ChildTask, or with both;
  - a node the tree holds twice, which would have two parents.
*/
#include "Script.h"

#include <string>
#include <vector>


struct FTreeObject
{
    std::string Name;
    std::vector<Uasset::FPropertyDef> Tags;     // an FDefaultValue::Instanced id is an index into the list + 1
};

/* Adds TreeAsset and ParentNode to each node in Objects, reached from the tree's RootNode and RootDecorators in
   TreeTags. A node that sets either itself is refused: the tree decides them. False with Err on a refusal. */
bool CookBehaviorTree(const std::vector<Uasset::FPropertyDef>& TreeTags, std::vector<FTreeObject>& Objects, std::string* Err);
