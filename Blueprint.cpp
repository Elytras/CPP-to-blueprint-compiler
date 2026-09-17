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

    // A /Game class is a BlueprintGeneratedClass, not a Class; the wrong row fails at load, not write.
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
        // A CDO's class is the class it defaults, not "Class".
        const int32 Row = P.AddImport({ PackageName, ClassName_, PackageImport(PackageName),
                                        "Default__" + ClassName_ });
        It = ImportCache.emplace(Key, Row).first;
    }
    const FIndex Idx = Imp(It->second);
    if (std::find(CallImports.begin(), CallImports.end(), Idx.V) == CallImports.end())
        CallImports.push_back(Idx.V);
    return Idx;
}

FIndex FBlueprintClass::Asset(const std::string& ClassPackage, const std::string& ClassName_,
                              const std::string& AssetPackage, const std::string& AssetName)
{
    const std::string Key = "asset:" + AssetPackage + "." + AssetName;
    auto It = ImportCache.find(Key);
    if (It != ImportCache.end()) return Imp(It->second);

    EngineClass(ClassPackage, ClassName_);      // the linker resolves an import's class through its own import
    const int32 Row = P.AddImport({ ClassPackage, ClassName_, PackageImport(AssetPackage), AssetName });
    ImportCache.emplace(Key, Row);
    CallImports.push_back(Imp(Row).V);
    return Imp(Row);
}

FIndex FBlueprintClass::ScriptStruct(const std::string& PackageName, const std::string& StructName)
{
    const std::string Key = "str:" + PackageName + "." + StructName;
    auto It = ImportCache.find(Key);
    if (It != ImportCache.end()) return Imp(It->second);

    // A /Game struct is a UserDefinedStruct; its bytecode references need it created first.
    const bool bBlueprint = PackageName.compare(0, 6, "/Game/") == 0;
    const FIndex Outer = PackageImport(PackageName);
    const int32 Row = P.AddImport({ bBlueprint ? "/Script/Engine" : "/Script/CoreUObject",
                                    bBlueprint ? "UserDefinedStruct" : "ScriptStruct",
                                    Outer, StructName });
    ImportCache.emplace(Key, Row);
    if (bBlueprint) CallImports.push_back(Imp(Row).V);
    return Imp(Row);
}

FIndex FBlueprintClass::Enum(const std::string& PackageName, const std::string& EnumName)
{
    const std::string Key = "enum:" + PackageName + "." + EnumName;
    auto It = ImportCache.find(Key);
    if (It != ImportCache.end()) return Imp(It->second);

    const bool bBlueprint = PackageName.compare(0, 6, "/Game/") == 0;
    const FIndex Outer = PackageImport(PackageName);
    const int32 Row = P.AddImport({ bBlueprint ? "/Script/Engine" : "/Script/CoreUObject",
                                    bBlueprint ? "UserDefinedEnum" : "Enum",
                                    Outer, EnumName });
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

    const FIndex Owner = EngineClass(PackageName, OwningClass);
    const int32 Row = P.AddImport({ "/Script/CoreUObject", "Function", Owner, FunctionName });
    ImportCache.emplace(Key, Row);

    for (FIndex Ref : { Imp(Row), Owner })
        if (std::find(CallImports.begin(), CallImports.end(), Ref.V) == CallImports.end())
            CallImports.push_back(Ref.V);

    return Imp(Row);
}

FIndex FBlueprintClass::AddFunction(const std::string& Name, FIndex Super,
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
    return Exp(2 + int32(Functions.size()) - 1);    // Finish() writes functions from row 2, in order
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

    const FIndex ParentIdx = EngineClass(ParentPackage, ParentClass);
    const FIndex ParentCdo = Imp(P.AddImport({ ParentPackage, ParentClass,
                                               PackageImport(ParentPackage),
                                               "Default__" + ParentClass }));

    const int32 RowClass = 0;
    const int32 RowCdo = 1;
    const int32 RowFirstFunction = 2;
    const int32 RowRootTemplate = RowFirstFunction + int32(Functions.size());
    const int32 RowScsNode = RowRootTemplate + 1;
    const int32 RowScs = RowScsNode + 1;
    const FIndex ScsIdx = bIsActor ? Exp(RowScs) : Null();
    ClassRow = RowClass;

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

    // Preload edges as a shipped DRG BPGC states them (dumpedl.py). Missing the parent
    // serialize-before-serialize edge aborts the async loading thread.
    Class.SerBeforeSer = { ParentIdx.V, ParentCdo.V };
    if (bIsActor) Class.SerBeforeSer.push_back(ScsIdx.V);
    Class.SerBeforeCreate = { BpgcClass.V, BpgcCdo.V };
    Class.CreateBeforeCreate = { ParentIdx.V };
    for (int32 I = 0; I < NumFunctions; ++I)
        Class.CreateBeforeSer.push_back(Exp(RowFirstFunction + I).V);
    for (const FPropertyDef& V : Vars)      // a dispatcher's Extra is a function listed above
        if (V.Extra.V != 0 && std::find(Class.CreateBeforeSer.begin(), Class.CreateBeforeSer.end(), V.Extra.V) == Class.CreateBeforeSer.end())
            Class.CreateBeforeSer.push_back(V.Extra.V);
    for (FIndex I : Interfaces) Class.CreateBeforeSer.push_back(I.V);  // as BP_SentryGun_MoveMarker lists Targetable
    const std::vector<FIndex> ClassInterfaces = Interfaces;
    const std::vector<FPropertyDef> ClassVars = Vars;
    const bool bActor = bIsActor;
    const uint32 Flags = ClassFlags;
    const FIndex UberGraph = UberGraphFunction;
    /* UBlueprintGeneratedClass::GetLifetimeBlueprintReplicationList stops after this many CPF_Net properties: without
       the tag nothing the class declares replicates. Measured on BP_LiftPod: the first tag. */
    const int32 NumReplicated = int32(std::count_if(Vars.begin(), Vars.end(),
                                                    [](const FPropertyDef& V) { return (V.PropertyFlags & CPF_Net) != 0; }));
    Class.Serialize = [=](FArc& Ar) {
        if (NumReplicated > 0)
            Tag(Ar, "NumReplicatedProperties", "IntProperty", [=](FArc& V) { V.I32(NumReplicated); });
        if (bActor)
            Tag(Ar, "SimpleConstructionScript", "ObjectProperty",
                [=](FArc& V) { V.Idx(ScsIdx); });
        /* Measured on BP_LiftPod, after SimpleConstructionScript. UBlueprintGeneratedClass::Link then finds the
           UberGraphFrame property by name. */
        if (UberGraph.V != 0)
            Tag(Ar, "UberGraphFunction", "ObjectProperty", [=](FArc& V) { V.Idx(UberGraph); });
        TagEnd(Ar);
        Ar.Bool(false);

        Ar.Idx(ParentIdx);                          // SuperStruct
        Ar.I32(NumFunctions);                       // Children
        for (int32 I = 0; I < NumFunctions; ++I) Ar.Idx(Exp(RowFirstFunction + I));
        Ar.I32(int32(ClassVars.size()));            // ChildProperties
        for (const FPropertyDef& V : ClassVars) WriteProperty(Ar, V);
        Ar.I32(0);                                  // script bytecode size
        Ar.I32(0);                                  // script storage size

        Ar.I32(NumFunctions);                       // FuncMap
        for (int32 I = 0; I < NumFunctions; ++I)
        {
            Ar.Name(FunctionNames[size_t(I)]);
            Ar.Idx(Exp(RowFirstFunction + I));
        }

        Ar.U32(Flags);                              // ClassFlags
        Ar.Idx(ObjectClass);                        // ClassWithin
        Ar.Name("Engine");                          // ClassConfigName
        Ar.Idx(Null());                             // ClassGeneratedBy
        Ar.I32(int32(ClassInterfaces.size()));      // Interfaces: class, PointerOffset, bImplementedByK2
        for (FIndex I : ClassInterfaces) { Ar.Idx(I); Ar.I32(0); Ar.Bool(true); }
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
        Cdo.CreateBeforeSer = { ParentIdx.V };
    for (const FPropertyDef& V : Vars) DefaultRefs(V.Default, Cdo.CreateBeforeSer);     // as ED_Spider_Grunt lists its EnemyID
    // AActor defaults bCanEverTick to false; the BP compiler sets it on the CDO when ReceiveTick
    // is overridden (KismetCompiler.cpp, SetCanEverTick), else the actor loads and never ticks.
    const bool bOverridesTick = std::any_of(Functions.begin(), Functions.end(),
        [](const FPending& F) { return F.Def.Name == "ReceiveTick"; });

    const bool bCdoReplicates = bReplicates;
    Cdo.Serialize = [bOverridesTick, ClassVars, bCdoReplicates](FArc& Ar) {
        if (bCdoReplicates) TagBool(Ar, "bReplicates", true);
        if (bOverridesTick)
            Tag(Ar, "PrimaryActorTick", "StructProperty", [](FArc& V) {
                TagBool(V, "bCanEverTick", true);
                TagEnd(V);
            }, "ActorTickFunction");
        /* Only initialised members: an absent tag keeps the parent CDO's (zero) value. */
        for (const FPropertyDef& V : ClassVars)
            if (V.Default.K != FDefaultValue::None) WriteDefaultTag(Ar, V);
        TagEnd(Ar);                                 // a CDO omits the lazy-object guid
    };
    P.AddExport(std::move(Cdo));

    for (size_t I = 0; I < Functions.size(); ++I)
    {
        std::vector<int32> Refs = CallImports;
        const FIndex SelfExp = Exp(RowFirstFunction + int32(I));
        Refs.push_back(SelfExp.V);
        for (const FPropertyDef& Prop : Functions[I].Def.Params)
            if (Prop.Extra.V != 0 && std::find(Refs.begin(), Refs.end(), Prop.Extra.V) == Refs.end())
                Refs.push_back(Prop.Extra.V);
        const auto& Body = Functions[I].Body;
        AddFunctionExport(P, Functions[I].Def, Exp(RowClass), FunctionClass, FunctionCdo,
                          [Body, SelfExp](FScript& S) { Body(S, SelfExp); }, Refs);
    }

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

/* Measured on DRG's MM_ResourceInfo: Guid tag, empty UStruct body, StructFlags 0, then the default
   instance as a tag per member. */
void FBlueprintClass::FinishStruct(const uint32 (&Guid)[4])
{
    const FIndex UdsClass = EngineClass("/Script/Engine", "UserDefinedStruct");
    const FIndex UdsCdo = ClassDefaultObject("/Script/Engine", "UserDefinedStruct");
    ClassRow = 0;

    FExport S;
    S.ClassIndex = UdsClass;
    S.TemplateIndex = UdsCdo;
    S.ObjectName = ClassName;
    S.ObjectFlags = RF_Public | RF_Standalone | RF_Transactional;
    S.bIsAsset = true;
    S.SerBeforeCreate = { UdsClass.V, UdsCdo.V };
    for (const FPropertyDef& V : Vars)
        if (V.Extra.V != 0)
            S.CreateBeforeSer.push_back(V.Extra.V);

    uint32 G[4] = { Guid[0], Guid[1], Guid[2], Guid[3] };
    const std::vector<FPropertyDef> Members = Vars;
    S.Serialize = [=](FArc& Ar) {
        Tag(Ar, "Guid", "StructProperty", [=](FArc& V) { V.Guid(G); }, "Guid");
        TagEnd(Ar);
        Ar.Bool(false);

        Ar.Idx(Null());                             // SuperStruct
        Ar.I32(0);                                  // Children
        Ar.I32(int32(Members.size()));              // ChildProperties
        for (const FPropertyDef& M : Members) WriteProperty(Ar, M);
        Ar.I32(0);                                  // script bytecode size
        Ar.I32(0);                                  // script storage size
        Ar.U32(0);                                  // StructFlags

        for (const FPropertyDef& M : Members) WriteDefaultTag(Ar, M);
        TagEnd(Ar);
    };
    P.AddExport(std::move(S));
}

/* Measured on DRG's ED_Spider_Grunt: one Public | Standalone | Transactional export, the class and its CDO
   as serialize-before-create edges, only the tags that differ from the CDO, then the lazy-object guid flag. */
void FBlueprintClass::FinishAsset(FIndex Class, FIndex ClassCdo)
{
    ClassRow = 0;

    FExport A;
    A.ClassIndex = Class;
    A.TemplateIndex = ClassCdo;
    A.ObjectName = ClassName;
    A.ObjectFlags = RF_Public | RF_Standalone | RF_Transactional;
    A.bIsAsset = true;
    A.SerBeforeCreate = { Class.V, ClassCdo.V };
    for (const FPropertyDef& V : Vars)
    {
        if (V.Extra.V != 0) A.CreateBeforeSer.push_back(V.Extra.V);      // a user-defined enum / struct the tag names
        DefaultRefs(V.Default, A.CreateBeforeSer);
    }

    const std::vector<FPropertyDef> Set = Vars;
    A.Serialize = [=](FArc& Ar) {
        for (const FPropertyDef& V : Set) WriteDefaultTag(Ar, V);
        TagEnd(Ar);
        Ar.Bool(false);
    };
    P.AddExport(std::move(A));
}

/* Measured on DRG's ENU_TextCase: the tags (the editor's DisplayNameMap), no GUID, then UEnum::Names as
   (FName, int64) pairs ending in the _MAX entry, then CppForm 1 (Namespaced). Nothing outside WITH_EDITOR
   reads the names' shape, so they are the C++ ones rather than the editor's NewEnumeratorN.
   ponytail: no DisplayNameMap; UEnum::GetDisplayNameTextByIndex falls back to the enumerator's own name. */
void FBlueprintClass::FinishEnum(const std::vector<std::pair<std::string, int64>>& Enumerators)
{
    const FIndex EnumClass = EngineClass("/Script/Engine", "UserDefinedEnum");
    const FIndex EnumCdo = ClassDefaultObject("/Script/Engine", "UserDefinedEnum");
    ClassRow = 0;

    FExport E;
    E.ClassIndex = EnumClass;
    E.TemplateIndex = EnumCdo;
    E.ObjectName = ClassName;
    E.ObjectFlags = RF_Public | RF_Standalone | RF_Transactional;
    E.bIsAsset = true;
    E.SerBeforeCreate = { EnumClass.V, EnumCdo.V };

    int64 Max = 0;
    for (const auto& En : Enumerators) Max = std::max(Max, En.second + 1);
    const std::string Prefix = ClassName;
    const auto Names = Enumerators;
    E.Serialize = [=](FArc& Ar) {
        TagEnd(Ar);
        Ar.Bool(false);
        Ar.I32(int32(Names.size() + 1));
        for (const auto& [Name, Value] : Names)
        {
            Ar.Name(Prefix + "::" + Name);
            Ar.I64(Value);
        }
        Ar.Name(Prefix + "::" + Prefix + "_MAX");
        Ar.I64(Max);
        Ar.U8(1);                                   // ECppForm::Namespaced
    };
    P.AddExport(std::move(E));
}

}   // namespace Uasset
