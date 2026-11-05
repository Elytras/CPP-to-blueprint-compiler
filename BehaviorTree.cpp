#include "BehaviorTree.h"

#include "Package.h"

#include <functional>

using namespace Uasset;

namespace
{
const FPropertyDef* Find(const std::vector<FPropertyDef>& Tags, const std::string& Name)
{
    for (const FPropertyDef& T : Tags) if (Lower(T.Name) == Lower(Name)) return &T;
    return nullptr;
}

/* The objects a tag points at, as Instanced ids (an array's in order); an asset or null is 0. */
std::vector<int32> Refs(const FPropertyDef* T)
{
    std::vector<int32> Out;
    if (!T) return Out;
    if (T->Default.K == FDefaultValue::Array) for (const FDefaultValue& I : T->Default.Items) Out.push_back(I.Instanced);
    else if (T->Default.K == FDefaultValue::Obj) Out.push_back(T->Default.Instanced);
    return Out;
}

FPropertyDef ObjectTag(const std::string& Name, FIndex Object, int32 Instanced)
{
    FPropertyDef P;
    P.Type = "ObjectProperty";
    P.Name = Name;
    P.ElementSize = 8;
    P.Default.K = FDefaultValue::Obj;
    P.Default.Object = Object;
    P.Default.Instanced = Instanced;
    return P;
}
}

bool CookBehaviorTree(const std::vector<FPropertyDef>& TreeTags, std::vector<FTreeObject>& Objects, std::string* Err)
{
    /* Each node's parent: an Instanced id, 0 for none. Seen marks a node already placed. */
    std::vector<int32> Parent(Objects.size(), 0);
    std::vector<bool> Seen(Objects.size(), false);
    auto Place = [&](int32 Id, int32 Of, const std::string& As) {
        if (Id <= 0 || size_t(Id) > Objects.size()) return true;           // null, or no object of the block
        FTreeObject& O = Objects[size_t(Id - 1)];
        if (Seen[size_t(Id - 1)])
        { *Err = O.Name + " is in the tree twice (again as " + As + "); make each place a node of its own"; return false; }
        for (const char* Kept : { "TreeAsset", "ParentNode" })
            if (Find(O.Tags, Kept))
            { *Err = O.Name + ": " + Kept + " is set from the tree's shape, as the editor sets it; leave it out"; return false; }
        Seen[size_t(Id - 1)] = true;
        Parent[size_t(Id - 1)] = Of;
        return true;
    };
    std::function<bool(int32, int32)> Composite = [&](int32 Id, int32 Of) {
        if (!Place(Id, Of, "a composite")) return false;
        if (Id <= 0 || size_t(Id) > Objects.size()) return true;
        const std::vector<FPropertyDef>& Tags = Objects[size_t(Id - 1)].Tags;
        for (int32 S : Refs(Find(Tags, "Services"))) if (!Place(S, Id, "a service")) return false;
        const FPropertyDef* Children = Find(Tags, "Children");
        if (!Children || Children->Default.K != FDefaultValue::Array) return true;
        int32 K = 0;
        for (const FDefaultValue& C : Children->Default.Items)
        {
            const std::string At = Objects[size_t(Id - 1)].Name + "'s child " + std::to_string(K++);
            const std::vector<FPropertyDef> None;
            const std::vector<FPropertyDef>& M = C.Members ? *C.Members : None;
            const std::vector<int32> Sub = Refs(Find(M, "ChildComposite")), Task = Refs(Find(M, "ChildTask"));
            const bool bSub = !Sub.empty() && Sub[0], bTask = !Task.empty() && Task[0];
            if (bSub == bTask)
            { *Err = At + " has " + (bSub ? "both a ChildComposite and a ChildTask" : "neither a ChildComposite nor a ChildTask")
                     + "; give it one"; return false; }
            for (int32 D : Refs(Find(M, "Decorators"))) if (!Place(D, Id, At + "'s decorator")) return false;
            if (bSub && !Composite(Sub[0], Id)) return false;
            if (bTask)
            {
                if (!Place(Task[0], Id, At)) return false;
                if (size_t(Task[0]) <= Objects.size())
                    for (int32 S : Refs(Find(Objects[size_t(Task[0] - 1)].Tags, "Services")))
                        if (!Place(S, Id, At + "'s service")) return false;
            }
        }
        return true;
    };
    for (int32 D : Refs(Find(TreeTags, "RootDecorators"))) if (!Place(D, 0, "a root decorator")) return false;
    const std::vector<int32> Root = Refs(Find(TreeTags, "RootNode"));
    if (!Root.empty() && !Composite(Root[0], 0)) return false;

    /* The asset is the package's first export (FBlueprintClass::FinishAsset). */
    for (size_t I = 0; I < Objects.size(); ++I)
    {
        if (!Seen[I]) continue;
        Objects[I].Tags.push_back(ObjectTag("TreeAsset", Exp(0), 0));
        if (Parent[I]) Objects[I].Tags.push_back(ObjectTag("ParentNode", Null(), Parent[I]));
    }
    return true;
}
