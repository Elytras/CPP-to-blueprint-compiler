/*
Blueprint.cpp — assembling a Blueprint class out of the package primitives.

The export order matters and is not arbitrary. The class comes first so that everything
outered to it (its functions, its scene root, its construction script) can name it, and the
loader is told the rest through the create-before-create dependencies the package writer
derives from each row.

The layout mirrors what the editor's cooker produces for the same class, because that is the
only shape known to load: a BlueprintGeneratedClass whose Children lists its functions, whose
FuncMap maps their names, and whose SimpleConstructionScript owns one default scene root.
*/
#include "Blueprint.h"

#include <algorithm>

namespace Uasset
{
FBlueprintClass::FBlueprintClass(FPackage& InPkg, std::string InClassName,
                                 std::string InParentPackage, std::string InParentClass,
                                 bool bInParentIsBlueprint)
    : P(InPkg), ClassName(std::move(InClassName)), ParentPackage(std::move(InParentPackage)),
      ParentClass(std::move(InParentClass)), bParentIsBlueprint(bInParentIsBlueprint)
{
}

/*
Imports a package by path. Native code lives under /Script/<Module>, but a Blueprint lives in
its own /Game asset package — and either way the package OBJECT's class is CoreUObject.Package,
so both spell the same row. Only the CDO of a Blueprint parent differs: its class is the
Blueprint itself, so a /Game path shows up there as a ClassPackage.
*/
FIndex FBlueprintClass::PackageImport(const std::string& PackageName)
{
    const std::string Key = "pkg:" + PackageName;
    auto It = ImportCache.find(Key);
    if (It != ImportCache.end()) return Imp(It->second);

    const int32 Row = P.AddImport({ "/Script/CoreUObject", "Package", Null(), PackageName });
    ImportCache.emplace(Key, Row);
    return Imp(Row);
}

FIndex FBlueprintClass::EngineClass(const std::string& PackageName, const std::string& ClassName_)
{
    const std::string Key = "cls:" + PackageName + "." + ClassName_;
    auto It = ImportCache.find(Key);
    if (It != ImportCache.end()) return Imp(It->second);

    const FIndex Outer = PackageImport(PackageName);
    const int32 Row = P.AddImport({ "/Script/CoreUObject", "Class", Outer, ClassName_ });
    ImportCache.emplace(Key, Row);
    return Imp(Row);
}

FIndex FBlueprintClass::ScriptStruct(const std::string& PackageName, const std::string& StructName)
{
    const std::string Key = "str:" + PackageName + "." + StructName;
    auto It = ImportCache.find(Key);
    if (It != ImportCache.end()) return Imp(It->second);

    const FIndex Outer = PackageImport(PackageName);
    const int32 Row = P.AddImport({ "/Script/CoreUObject", "ScriptStruct", Outer, StructName });
    ImportCache.emplace(Key, Row);
    return Imp(Row);
}

FIndex FBlueprintClass::EngineFunction(const std::string& PackageName,
                                       const std::string& OwningClass,
                                       const std::string& FunctionName)
{
    const std::string Key = "fn:" + PackageName + "." + OwningClass + ":" + FunctionName;
    auto It = ImportCache.find(Key);
    if (It != ImportCache.end()) return Imp(It->second);

    // A function is outered to the class that declares it, which must therefore be imported too.
    const FIndex Owner = EngineClass(PackageName, OwningClass);
    const int32 Row = P.AddImport({ "/Script/CoreUObject", "Function", Owner, FunctionName });
    ImportCache.emplace(Key, Row);

    // Both are reachable from a function body, so both become preload dependencies of one.
    for (FIndex Ref : { Imp(Row), Owner })
        if (std::find(CallImports.begin(), CallImports.end(), Ref.V) == CallImports.end())
            CallImports.push_back(Ref.V);

    return Imp(Row);
}

void FBlueprintClass::AddFunction(const std::string& Name, FIndex Super,
                                  const std::vector<FPropertyDef>& Params,
                                  const std::function<void(FScript&)>& Body)
{
    FFunctionDef Def;
    Def.Name = Name;
    Def.Super = Super;
    Def.Params = Params;
    Functions.push_back(FPending{ Def, Body });
}

void FBlueprintClass::Finish()
{
    const std::string CDOName = "Default__" + ClassName;

    const FIndex BpgcClass = EngineClass("/Script/Engine", "BlueprintGeneratedClass");
    const FIndex ObjectClass = EngineClass("/Script/CoreUObject", "Object");
    const FIndex FunctionClass = EngineClass("/Script/CoreUObject", "Function");
    const FIndex SceneCompClass = EngineClass("/Script/Engine", "SceneComponent");
    const FIndex ScsNodeClass = EngineClass("/Script/Engine", "SCS_Node");
    const FIndex ScsClass = EngineClass("/Script/Engine", "SimpleConstructionScript");

    const FIndex EnginePkg = PackageImport("/Script/Engine");
    const FIndex CorePkg = PackageImport("/Script/CoreUObject");
    const FIndex BpgcCdo = Imp(P.AddImport({ "/Script/Engine", "BlueprintGeneratedClass",
                                             EnginePkg, "Default__BlueprintGeneratedClass" }));
    const FIndex FunctionCdo = Imp(P.AddImport({ "/Script/CoreUObject", "Function",
                                                 CorePkg, "Default__Function" }));
    const FIndex SceneCompCdo = Imp(P.AddImport({ "/Script/Engine", "SceneComponent",
                                                  EnginePkg, "Default__SceneComponent" }));
    const FIndex ScsNodeCdo = Imp(P.AddImport({ "/Script/Engine", "SCS_Node",
                                                EnginePkg, "Default__SCS_Node" }));
    const FIndex ScsCdo = Imp(P.AddImport({ "/Script/Engine", "SimpleConstructionScript",
                                            EnginePkg, "Default__SimpleConstructionScript" }));

    /*
    The parent, and the parent's default object which the CDO uses as its template. For a
    Blueprint parent both live in that Blueprint's own cooked package; for an engine class
    they are ordinary /Script imports.
    */
    FIndex ParentIdx, ParentCdo;
    if (bParentIsBlueprint)
    {
        const FIndex ParentPkgIdx = PackageImport(ParentPackage);
        ParentIdx = Imp(P.AddImport({ "/Script/Engine", "BlueprintGeneratedClass",
                                      ParentPkgIdx, ParentClass }));
        ParentCdo = Imp(P.AddImport({ ParentPackage, ParentClass, ParentPkgIdx,
                                      "Default__" + ParentClass }));
    }
    else
    {
        ParentIdx = EngineClass(ParentPackage, ParentClass);
        ParentCdo = Imp(P.AddImport({ ParentPackage, ParentClass,
                                      PackageImport(ParentPackage), "Default__" + ParentClass }));
    }

    // Row numbers are fixed here so the exports can refer to each other before they exist.
    const int32 RowClass = 0;
    const int32 RowCdo = 1;
    const int32 RowFirstFunction = 2;
    const int32 RowRootTemplate = RowFirstFunction + int32(Functions.size());
    const int32 RowScsNode = RowRootTemplate + 1;
    const int32 RowScs = RowScsNode + 1;
    ClassRow = RowClass;

    /* The class. Its Children and FuncMap are what make its functions reachable. */
    std::vector<std::string> FunctionNames;
    for (const FPending& F : Functions) FunctionNames.push_back(F.Def.Name);
    const int32 NumFunctions = int32(Functions.size());

    FExport Class;
    Class.ClassIndex = BpgcClass;
    Class.SuperIndex = ParentIdx;
    Class.TemplateIndex = BpgcCdo;
    Class.OuterIndex = Null();
    Class.ObjectName = ClassName;
    Class.ObjectFlags = RF_Public | RF_Transactional;
    Class.bIsAsset = true;

    /*
    Preload dependencies, in the shape a cooked BPGC states them (read out of a shipped DRG
    class with dumpedl.py, not inferred).

    The serialize-before-serialize edge onto the parent is the one that matters most: without
    it the loader reaches a subclass that names its parent and finds the parent still waiting
    to be serialized, which aborts the async loading thread outright.
    */
    Class.SerBeforeSer = { ParentIdx.V, ParentCdo.V, Exp(RowScs).V };
    Class.SerBeforeCreate = { BpgcClass.V, BpgcCdo.V };
    Class.CreateBeforeCreate = { ParentIdx.V };
    for (int32 I = 0; I < NumFunctions; ++I)        // Children and FuncMap name these
        Class.CreateBeforeSer.push_back(Exp(RowFirstFunction + I).V);
    Class.Serialize = [=](FArc& Ar) {
        Tag(Ar, "SimpleConstructionScript", "ObjectProperty",
            [=](FArc& V) { V.Idx(Exp(RowScs)); });
        TagEnd(Ar);
        Ar.Bool(false);

        Ar.Idx(ParentIdx);                          // SuperStruct
        Ar.I32(NumFunctions);                       // Children, as an array of UField*
        for (int32 I = 0; I < NumFunctions; ++I) Ar.Idx(Exp(RowFirstFunction + I));
        Ar.I32(0);                                  // ChildProperties: no class variables yet
        Ar.I32(0);                                  // script bytecode size
        Ar.I32(0);                                  // script storage size

        Ar.I32(NumFunctions);                       // FuncMap
        for (int32 I = 0; I < NumFunctions; ++I)
        {
            Ar.Name(FunctionNames[size_t(I)]);
            Ar.Idx(Exp(RowFirstFunction + I));
        }

        Ar.U32(0x00840814);                         // ClassFlags, as the cooker emits for a BPGC
        Ar.Idx(ObjectClass);                        // ClassWithin
        Ar.Name("Engine");                          // ClassConfigName
        Ar.I32(0);                                  // implemented interfaces
        Ar.Idx(Null());                             // ClassGeneratedBy
        Ar.Bool(false);                             // bDeprecatedForceScriptOrder
        Ar.Name("None");
        Ar.Bool(true);                              // bCooked
        Ar.Idx(Exp(RowCdo));                        // ClassDefaultObject
    };
    P.AddExport(std::move(Class));

    FExport Cdo;
    Cdo.ClassIndex = Exp(RowClass);
    Cdo.TemplateIndex = ParentCdo;
    Cdo.ObjectName = CDOName;
    Cdo.ObjectFlags = RF_Public | RF_ClassDefaultObject | RF_ArchetypeObject;
    Cdo.SerBeforeSer = { Exp(RowScsNode).V, Exp(RowRootTemplate).V };
    Cdo.SerBeforeCreate = { Exp(RowClass).V, ParentCdo.V };
    if (bParentIsBlueprint)
        Cdo.CreateBeforeSer = { ParentIdx.V };      // the parent class object, for a BP parent
    Cdo.Serialize = [](FArc& Ar) { TagEnd(Ar); };   // a CDO omits the lazy-object guid
    P.AddExport(std::move(Cdo));

    for (size_t I = 0; I < Functions.size(); ++I)
    {
        std::vector<int32> Refs = CallImports;
        Refs.push_back(Exp(RowFirstFunction + int32(I)).V);   // a body can reference its own export
        AddFunctionExport(P, Functions[I].Def, Exp(RowClass), FunctionClass, FunctionCdo,
                          Functions[I].Body, Refs);
    }

    /*
    A default scene root. An actor can technically live without one, but every cooked actor
    Blueprint has this trio, so the generated class keeps the same shape rather than betting
    on the loader tolerating a rootless actor.
    */
    FExport RootTemplate;
    RootTemplate.ClassIndex = SceneCompClass;
    RootTemplate.TemplateIndex = SceneCompCdo;
    RootTemplate.OuterIndex = Exp(RowClass);
    RootTemplate.ObjectName = "DefaultSceneRoot_GEN_VARIABLE";
    RootTemplate.ObjectFlags = RF_Public | RF_Transactional | RF_ArchetypeObject;
    RootTemplate.SerBeforeSer = { Exp(RowClass).V };
    RootTemplate.SerBeforeCreate = { SceneCompClass.V, SceneCompCdo.V };
    RootTemplate.CreateBeforeCreate = { Exp(RowClass).V };
    RootTemplate.Serialize = [](FArc& Ar) { TagEnd(Ar); Ar.Bool(false); };
    P.AddExport(std::move(RootTemplate));

    FExport ScsNode;
    ScsNode.ClassIndex = ScsNodeClass;
    ScsNode.TemplateIndex = ScsNodeCdo;
    ScsNode.OuterIndex = Exp(RowScs);
    ScsNode.ObjectName = "SCS_Node_0";
    ScsNode.ObjectFlags = RF_Transactional;
    ScsNode.CreateBeforeSer = { Exp(RowRootTemplate).V, SceneCompClass.V };
    ScsNode.SerBeforeCreate = { ScsNodeClass.V, ScsNodeCdo.V };
    ScsNode.CreateBeforeCreate = { Exp(RowScs).V };
    ScsNode.Serialize = [=](FArc& Ar) {
        Tag(Ar, "ComponentClass", "ObjectProperty", [=](FArc& V) { V.Idx(SceneCompClass); });
        Tag(Ar, "ComponentTemplate", "ObjectProperty",
            [=](FArc& V) { V.Idx(Exp(RowRootTemplate)); });
        Tag(Ar, "InternalVariableName", "NameProperty",
            [](FArc& V) { V.Name("DefaultSceneRoot"); });
        TagEnd(Ar);
        Ar.Bool(false);
    };
    P.AddExport(std::move(ScsNode));

    FExport Scs;
    Scs.ClassIndex = ScsClass;
    Scs.TemplateIndex = ScsCdo;
    Scs.OuterIndex = Exp(RowClass);
    Scs.ObjectName = "SimpleConstructionScript_0";
    Scs.ObjectFlags = RF_Transactional;
    Scs.CreateBeforeSer = { Exp(RowScsNode).V };
    Scs.SerBeforeCreate = { ScsClass.V, ScsCdo.V };
    Scs.CreateBeforeCreate = { Exp(RowClass).V };
    Scs.Serialize = [=](FArc& Ar) {
        Tag(Ar, "DefaultSceneRootNode", "ObjectProperty",
            [=](FArc& V) { V.Idx(Exp(RowScsNode)); });
        TagEnd(Ar);
        Ar.Bool(false);
    };
    P.AddExport(std::move(Scs));
}

}   // namespace Uasset
