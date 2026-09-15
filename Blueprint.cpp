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

    /*
    An import row names the class OF the object it imports, and a class living under /Game is a
    BlueprintGeneratedClass rather than a UClass. Getting this wrong does not fail the write; it
    fails the load, when the linker looks for an object of the class the row claims.
    */
    const bool bBlueprint = PackageName.compare(0, 6, "/Game/") == 0;
    const FIndex Outer = PackageImport(PackageName);
    const int32 Row = P.AddImport({ bBlueprint ? "/Script/Engine" : "/Script/CoreUObject",
                                    bBlueprint ? "BlueprintGeneratedClass" : "Class",
                                    Outer, ClassName_ });
    ImportCache.emplace(Key, Row);
    return Imp(Row);
}

FIndex FBlueprintClass::PropertyOwner(const std::string& PackageName, const std::string& ClassName_)
{
    const FIndex Idx = EngineClass(PackageName, ClassName_);
    if (std::find(CallImports.begin(), CallImports.end(), Idx.V) == CallImports.end())
        CallImports.push_back(Idx.V);
    return Idx;
}

FIndex FBlueprintClass::ClassDefaultObject(const std::string& PackageName,
                                           const std::string& ClassName_)
{
    const std::string Key = "cdo:" + PackageName + "." + ClassName_;
    auto It = ImportCache.find(Key);
    if (It == ImportCache.end())
    {
        // A CDO's own class is the class it defaults, so the row names that rather than "Class".
        const int32 Row = P.AddImport({ PackageName, ClassName_, PackageImport(PackageName),
                                        "Default__" + ClassName_ });
        It = ImportCache.emplace(Key, Row).first;
    }
    const FIndex Idx = Imp(It->second);
    if (std::find(CallImports.begin(), CallImports.end(), Idx.V) == CallImports.end())
        CallImports.push_back(Idx.V);
    return Idx;
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
                                  const std::function<void(FScript&, FIndex)>& Body,
                                  uint32 FunctionFlags)
{
    FFunctionDef Def;
    Def.Name = Name;
    Def.Super = Super;
    Def.Params = Params;
    if (FunctionFlags != 0) Def.FunctionFlags = FunctionFlags;
    Functions.push_back(FPending{ Def, Body });
}

void FBlueprintClass::AddVariable(const FPropertyDef& Var)
{
    Vars.push_back(Var);
}

void FBlueprintClass::Finish()
{
    const std::string CDOName = "Default__" + ClassName;

    const FIndex BpgcClass = EngineClass("/Script/Engine", "BlueprintGeneratedClass");
    const FIndex ObjectClass = EngineClass("/Script/CoreUObject", "Object");
    const FIndex FunctionClass = EngineClass("/Script/CoreUObject", "Function");
    const FIndex EnginePkg = PackageImport("/Script/Engine");
    const FIndex CorePkg = PackageImport("/Script/CoreUObject");
    const FIndex BpgcCdo = Imp(P.AddImport({ "/Script/Engine", "BlueprintGeneratedClass",
                                             EnginePkg, "Default__BlueprintGeneratedClass" }));
    const FIndex FunctionCdo = Imp(P.AddImport({ "/Script/CoreUObject", "Function",
                                                 CorePkg, "Default__Function" }));

    /*
    The parent, and the parent's default object which the CDO uses as its template. For a
    Blueprint parent both live in that Blueprint's own cooked package; for an engine class
    they are ordinary /Script imports.
    */
    const FIndex ParentIdx = EngineClass(ParentPackage, ParentClass);
    const FIndex ParentCdo = Imp(P.AddImport({ ParentPackage, ParentClass,
                                               PackageImport(ParentPackage),
                                               "Default__" + ParentClass }));

    // Row numbers are fixed here so the exports can refer to each other before they exist.
    const int32 RowClass = 0;
    const int32 RowCdo = 1;
    const int32 RowFirstFunction = 2;
    const int32 RowRootTemplate = RowFirstFunction + int32(Functions.size());
    const int32 RowScsNode = RowRootTemplate + 1;
    const int32 RowScs = RowScsNode + 1;
    const FIndex ScsIdx = bIsActor ? Exp(RowScs) : Null();      // null for a class with no SCS
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
    Class.SerBeforeSer = { ParentIdx.V, ParentCdo.V };
    if (bIsActor) Class.SerBeforeSer.push_back(ScsIdx.V);
    Class.SerBeforeCreate = { BpgcClass.V, BpgcCdo.V };
    Class.CreateBeforeCreate = { ParentIdx.V };
    for (int32 I = 0; I < NumFunctions; ++I)        // Children and FuncMap name these
        Class.CreateBeforeSer.push_back(Exp(RowFirstFunction + I).V);
    /* A variable's own type is serialized with it, so whatever it points at has to exist first. */
    for (const FPropertyDef& V : Vars)
        if (V.Extra.V != 0)
            Class.CreateBeforeSer.push_back(V.Extra.V);
    const std::vector<FPropertyDef> ClassVars = Vars;
    const bool bActor = bIsActor;
    const uint32 Flags = ClassFlags;
    Class.Serialize = [=](FArc& Ar) {
        if (bActor)
            Tag(Ar, "SimpleConstructionScript", "ObjectProperty",
                [=](FArc& V) { V.Idx(ScsIdx); });
        TagEnd(Ar);
        Ar.Bool(false);

        Ar.Idx(ParentIdx);                          // SuperStruct
        Ar.I32(NumFunctions);                       // Children, as an array of UField*
        for (int32 I = 0; I < NumFunctions; ++I) Ar.Idx(Exp(RowFirstFunction + I));
        Ar.I32(int32(ClassVars.size()));            // ChildProperties: the class variables
        for (const FPropertyDef& V : ClassVars) WriteProperty(Ar, V);
        Ar.I32(0);                                  // script bytecode size
        Ar.I32(0);                                  // script storage size

        Ar.I32(NumFunctions);                       // FuncMap
        for (int32 I = 0; I < NumFunctions; ++I)
        {
            Ar.Name(FunctionNames[size_t(I)]);
            Ar.Idx(Exp(RowFirstFunction + I));
        }

        Ar.U32(Flags);                              // ClassFlags, the full cooked set
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
    if (bIsActor) Cdo.SerBeforeSer = { Exp(RowScsNode).V, Exp(RowRootTemplate).V };
    Cdo.SerBeforeCreate = { Exp(RowClass).V, ParentCdo.V };
    if (bParentIsBlueprint)
        Cdo.CreateBeforeSer = { ParentIdx.V };      // the parent class object, for a BP parent
    /*
    AActor's constructor leaves PrimaryActorTick.bCanEverTick false, so an actor that overrides
    ReceiveTick and says nothing else loads fine and never ticks. The Blueprint compiler sets the
    flag on the CDO in exactly this case (KismetCompiler.cpp, SetCanEverTick), and a shipped
    class shows it: Autosprint's name table carries PrimaryActorTick / ActorTickFunction /
    bCanEverTick precisely because it has a tick event.
    */
    const bool bOverridesTick = std::any_of(Functions.begin(), Functions.end(),
        [](const FPending& F) { return F.Def.Name == "ReceiveTick"; });

    Cdo.Serialize = [bOverridesTick](FArc& Ar) {
        if (bOverridesTick)
            Tag(Ar, "PrimaryActorTick", "StructProperty", [](FArc& V) {
                TagBool(V, "bCanEverTick", true);
                TagEnd(V);
            }, "ActorTickFunction");
        TagEnd(Ar);                                 // a CDO omits the lazy-object guid
    };
    P.AddExport(std::move(Cdo));

    for (size_t I = 0; I < Functions.size(); ++I)
    {
        std::vector<int32> Refs = CallImports;
        const FIndex SelfExp = Exp(RowFirstFunction + int32(I));
        Refs.push_back(SelfExp.V);              // a body can reference its own export
        const auto& Body = Functions[I].Body;
        AddFunctionExport(P, Functions[I].Def, Exp(RowClass), FunctionClass, FunctionCdo,
                          [Body, SelfExp](FScript& S) { Body(S, SelfExp); }, Refs);
    }

    /*
    A default scene root. An actor can technically live without one, but every cooked actor
    Blueprint has this trio, so the generated class keeps the same shape rather than betting
    on the loader tolerating a rootless actor. A class that is not an actor gets none of it -
    USimpleConstructionScript reaches its owner's CDO as an AActor, which for anything else is
    a reinterpret of an unrelated object.
    */
    if (!bIsActor) return;

    const FIndex SceneCompClass = EngineClass("/Script/Engine", "SceneComponent");
    const FIndex ScsNodeClass = EngineClass("/Script/Engine", "SCS_Node");
    const FIndex ScsClass = EngineClass("/Script/Engine", "SimpleConstructionScript");
    const FIndex SceneCompCdo = ClassDefaultObject("/Script/Engine", "SceneComponent");
    const FIndex ScsNodeCdo = ClassDefaultObject("/Script/Engine", "SCS_Node");
    const FIndex ScsCdo = ClassDefaultObject("/Script/Engine", "SimpleConstructionScript");

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
