#pragma once
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "Package.h"
#include "Script.h"

namespace Uasset
{
/* The VariableGuid an SCS node gets, by class and component name. An override record has to name the
   parent node's GUID exactly (FComponentKey::Match compares only OwnerClass and AssociatedGuid), and
   both sides derive it from the same seed rather than reading the parent asset back. */
void ScsNodeGuid(const std::string& ClassName, const std::string& ComponentName, uint32 (&Out)[4]);

class FBlueprintClass
{
public:
    FBlueprintClass(FPackage& InPkg, std::string InClassName,
                    std::string ParentPackage, std::string ParentClass, bool bParentIsBlueprint);

    FIndex PackageImport(const std::string& PackageName);
    /* A subobject of some other class, e.g. an inherited component's <Name>_GEN_VARIABLE archetype:
       an import whose outer is that class's import rather than a package. */
    FIndex Subobject(const std::string& ClassPackage, const std::string& ClassName_, FIndex Outer,
                     const std::string& ObjectName);
    FIndex EngineClass(const std::string& PackageName, const std::string& ClassName);
    FIndex ScriptStruct(const std::string& PackageName, const std::string& StructName);
    FIndex Enum(const std::string& PackageName, const std::string& EnumName);
    FIndex EngineFunction(const std::string& PackageName, const std::string& OwningClass,
                          const std::string& FunctionName);

    /* Same import as EngineClass, but also recorded as a load dependency of every function body. */
    FIndex PropertyOwner(const std::string& PackageName, const std::string& ClassName);

    /* The CDO as an EX_Context literal; recorded as a load dependency like a called function. */
    FIndex ClassDefaultObject(const std::string& PackageName, const std::string& ClassName);

    /* An asset in another package (a data asset instance), as an object literal or a default's value. Recorded as a
       load dependency of every function body, like the CDO. */
    FIndex Asset(const std::string& ClassPackage, const std::string& ClassName_,
                 const std::string& AssetPackage, const std::string& AssetName);

    /* Only an actor may have the SCS trio: USimpleConstructionScript casts the owner CDO to AActor. */
    void SetIsActor(bool bValue) { bIsActor = bValue; }

    /* A cooked class stores the FULL EClassFlags set, mostly inherited from the native parent. */
    void SetClassFlags(uint32 Flags) { ClassFlags = Flags; }

    /* Replication: the CDO's bReplicates, set when the class replicates a variable or declares an RPC. The class's
       NumReplicatedProperties tag is counted from the CPF_Net variables. */
    void SetReplicates(bool bValue) { bReplicates = bValue; }
    /* ExecuteUbergraph_<Class>: written as the class's UberGraphFunction tag. */
    void SetUberGraphFunction(FIndex Function) { UberGraphFunction = Function; }

    /*
    `Super` is the engine UFunction being overridden, or null for a new method. Body receives the
    function's own export index (the FFieldPath owner for its params), filled in by Finish().
    Returns that export index.
    */
    /* UE_CATEGORY: a function's or variable's editor category. Editor metadata, so only the API stub carries it. */
    std::map<std::string, std::string> ApiCategory;

    FIndex AddFunction(const std::string& Name, FIndex Super,
                       const std::vector<FPropertyDef>& Params,
                       const std::function<void(FScript&, FIndex)>& Body,
                       uint32 FunctionFlags = 0);

    /* One ChildProperties entry on the class; no CDO default is written. */
    void AddVariable(const FPropertyDef& Var);

    /* An implemented interface: a UClass::Interfaces entry. Its functions are ordinary AddFunction()s. */
    void AddInterface(FIndex InterfaceClass) { Interfaces.push_back(InterfaceClass); }

    /*
    One UE_COMPONENT: an SCS node plus the <Name>_GEN_VARIABLE archetype it instantiates. `Defaults`
    are written as tagged properties on that archetype, so they delta against the component CDO -
    the editor's per-component defaults, not the actor CDO's. The class variable of the same name is
    an ordinary AddVariable(); USCS_Node::ExecuteNodeOnActor assigns the instance to it by name.
    */
    void AddComponent(const std::string& Name, FIndex ComponentClass, FIndex ComponentCdo,
                      bool bIsSceneComponent, const std::vector<FPropertyDef>& Defaults);

    /*
    An inherited component's defaults: one UInheritableComponentHandler record, which is how the
    editor stores a child class's override of a parent's SCS component. The template is a fresh
    component export archetyped on the parent's, so `Defaults` are its deltas.
    USCS_Node::GetActualComponentTemplate finds it by FComponentKey, which matches on OwnerClass and
    AssociatedGuid alone - so the GUID must be the parent SCS node's VariableGuid.
    */
    void AddComponentOverride(const std::string& Name, FIndex ComponentClass, FIndex ParentTemplate,
                              FIndex OwnerClass, const uint32 (&AssociatedGuid)[4],
                              const std::vector<FPropertyDef>& Defaults);

    /*
    An override of a NATIVE parent's default subobject - its components, which are not SCS nodes and
    so have nothing to do with the handler above. Measured on Ene_Butterfly: one export named exactly
    as the subobject, outered to THIS class's CDO, flags Public|Transactional|ArchetypeObject|
    DefaultSubObject and a null template (the engine resolves the archetype through the parent CDO's
    subobject of the same name), plus an ObjectProperty tag of that name on the CDO.
    */
    void AddSubobjectOverride(const std::string& Name, FIndex ComponentClass,
                              const std::vector<FPropertyDef>& Defaults);

    /* A tag on this class's CDO for a property an ancestor declares, which a member initializer
       cannot express: declaring the name again would shadow it with a second property. */
    void AddCdoDefault(const FPropertyDef& Var) { CdoDefaults.push_back(Var); }

    void Finish();

    /* The editor-side stub of this class: <OutDir>/<asset>.uasset, signatures only (Uncooked.h).
       Call after Finish(), while the cooked package still holds the imports the parameters name. */
    bool WriteApi(const std::string& OutDir, std::string* Err) const;

    /* Writes the package as one UserDefinedStruct export whose members are the AddVariable()s. */
    void FinishStruct(const uint32 (&Guid)[4]);

    /* The editor-side stub of this struct: an uncooked UserDefinedStruct with the same GUID-suffixed
       member names. Same Guid as FinishStruct so the cooked and editor assets share identity. */
    bool WriteApiStruct(const std::string& OutDir, const uint32 (&Guid)[4], std::string* Err) const;

    /* Writes the package as one UserDefinedEnum export: <ClassName>::<Enumerator> = value, then <ClassName>_MAX. */
    void FinishEnum(const std::vector<std::pair<std::string, int64>>& Enumerators);

    /* The editor-side stub of this enum: an uncooked UserDefinedEnum with the same "<Enum>::<Entry>" names. */
    bool WriteApiEnum(const std::string& OutDir, const std::vector<std::pair<std::string, int64>>& Enumerators,
                      std::string* Err) const;

    /* Writes the package as one instance of Class (a data asset): a tag per AddVariable(), each holding its Default. */
    void FinishAsset(FIndex Class, FIndex ClassCdo);

    FIndex ClassIndex() const { return Exp(ClassRow); }

private:
    struct FPending
    {
        FFunctionDef Def;
        std::function<void(FScript&, FIndex)> Body;
    };

    FPackage& P;
    std::string ClassName;
    std::string ParentPackage, ParentClass;
    bool bParentIsBlueprint = false;
    bool bIsActor = true;
    bool bReplicates = false;
    FIndex UberGraphFunction;
    uint32 ClassFlags = 0x00840814;

    std::unordered_map<std::string, int32> ImportCache;

    /*
    Union of every import a body may reach; each function export declares all of them as
    create-before-serialize edges. Under-declaring crashes the async loader.
    */
    std::vector<int32> CallImports;
    std::vector<FPending> Functions;
    std::vector<FPropertyDef> Vars;
    std::vector<FIndex> Interfaces;

    struct FComponent
    {
        std::string Name;
        FIndex Class, Cdo;
        bool bIsScene = false;
        std::vector<FPropertyDef> Defaults;
    };
    std::vector<FComponent> Components;

    struct FComponentOverride
    {
        std::string Name;
        FIndex Class, ParentTemplate, OwnerClass;
        uint32 Guid[4] = { 0, 0, 0, 0 };
        std::vector<FPropertyDef> Defaults;
    };
    std::vector<FComponentOverride> ComponentOverrides;
    std::vector<FPropertyDef> CdoDefaults;

    struct FSubobjectOverride
    {
        std::string Name;
        FIndex Class;
        std::vector<FPropertyDef> Defaults;
    };
    std::vector<FSubobjectOverride> SubobjectOverrides;

    int32 ClassRow = 0;
};

}   // namespace Uasset
