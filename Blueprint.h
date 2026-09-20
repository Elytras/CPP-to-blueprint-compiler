#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "Package.h"
#include "Script.h"

namespace Uasset
{
class FBlueprintClass
{
public:
    FBlueprintClass(FPackage& InPkg, std::string InClassName,
                    std::string ParentPackage, std::string ParentClass, bool bParentIsBlueprint);

    FIndex PackageImport(const std::string& PackageName);
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
    FIndex AddFunction(const std::string& Name, FIndex Super,
                       const std::vector<FPropertyDef>& Params,
                       const std::function<void(FScript&, FIndex)>& Body,
                       uint32 FunctionFlags = 0);

    /* One ChildProperties entry on the class; no CDO default is written. */
    void AddVariable(const FPropertyDef& Var);

    /* An implemented interface: a UClass::Interfaces entry. Its functions are ordinary AddFunction()s. */
    void AddInterface(FIndex InterfaceClass) { Interfaces.push_back(InterfaceClass); }

    void Finish();

    /* The editor-side stub of this class: <OutDir>/<asset>.uasset, signatures only (Uncooked.h).
       Call after Finish(), while the cooked package still holds the imports the parameters name. */
    bool WriteApi(const std::string& OutDir, std::string* Err) const;

    /* Writes the package as one UserDefinedStruct export whose members are the AddVariable()s. */
    void FinishStruct(const uint32 (&Guid)[4]);

    /* Writes the package as one UserDefinedEnum export: <ClassName>::<Enumerator> = value, then <ClassName>_MAX. */
    void FinishEnum(const std::vector<std::pair<std::string, int64>>& Enumerators);

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

    int32 ClassRow = 0;
};

}   // namespace Uasset
