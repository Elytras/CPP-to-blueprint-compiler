#include "Blueprint.h"

#include "Uncooked.h"

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

void ScsNodeGuid(const std::string& ClassName, const std::string& ComponentName, uint32 (&Out)[4])
{
    const std::string Seed = ClassName + ".scs." + ComponentName;
    Out[0] = StrCrc32(Seed) | 1u;               // non-zero: a null GUID reads as "unset"
    Out[1] = Strihash(Seed);
    Out[2] = StrCrc32(Seed + "\x01");
    Out[3] = Strihash(Seed + "\x02") | 1u;
}

FIndex FBlueprintClass::Subobject(const std::string& ClassPackage, const std::string& ClassName_,
                                  FIndex Outer, const std::string& ObjectName)
{
    /* An FName compares without case, so `temperature` (UeApi's member, off the object dump) and the grunt's package's
       `Temperature` are one object, which the cook imports once (SavePackage.cpp 3288-3339): the first spelling asked
       for is the row's. */
    const std::string Key = "sub:" + std::to_string(Outer.V) + ":" + Lower(ObjectName);
    auto It = ImportCache.find(Key);
    if (It != ImportCache.end()) return Imp(It->second);

    EngineClass(ClassPackage, ClassName_);      // the linker resolves an import's class through its own import
    const int32 Row = P.AddImport({ ClassPackage, ClassName_, Outer, ObjectName });
    ImportCache.emplace(Key, Row);
    return Imp(Row);
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

/* A component export's end: the tags' None, UObject's HasGuid, then what the class's native Serialize reads. */
static void EndComponent(FArc& Ar, const std::vector<uint8>& NativeTail)
{
    TagEnd(Ar);
    Ar.Bool(false);
    Ar.Raw(NativeTail.data(), NativeTail.size());
}

void FBlueprintClass::AddComponent(const std::string& Name, FIndex ComponentClass, FIndex ComponentCdo,
                                   bool bIsSceneComponent, const std::vector<FPropertyDef>& Defaults,
                                   const std::vector<uint8>& NativeTail)
{
    Components.push_back(FComponent{ Name, ComponentClass, ComponentCdo, bIsSceneComponent, Defaults, NativeTail, {} });
}

void FBlueprintClass::AttachComponent(const std::string& Name, const FAttachment& Attachment)
{
    for (FComponent& C : Components)
        if (C.Name == Name) C.Attachment = Attachment;
}

bool FBlueprintClass::ListsDefaultRoot() const
{
    return bIsActor && !bRootInherited && std::none_of(Components.begin(), Components.end(), [](const FComponent& C) {
        return C.bIsScene && C.Attachment.Parent.empty() && !C.Attachment.bRoot;
    });
}

void FBlueprintClass::AddSubobjectOverride(const std::string& Name, const std::string& Property, FIndex ComponentClass,
                                           const std::vector<FPropertyDef>& Defaults, const std::vector<uint8>& NativeTail)
{
    SubobjectOverrides.push_back(FSubobjectOverride{ Name, Property, ComponentClass, Defaults, NativeTail });
}

void FBlueprintClass::AddComponentOverride(const std::string& Name, FIndex ComponentClass, FIndex ParentTemplate,
                                           FIndex OwnerClass, const uint32 (&AssociatedGuid)[4],
                                           const std::vector<FPropertyDef>& Defaults, const std::vector<uint8>& NativeTail)
{
    FComponentOverride O;
    O.Name = Name;
    O.Class = ComponentClass;
    O.ParentTemplate = ParentTemplate;
    O.OwnerClass = OwnerClass;
    for (int32 I = 0; I < 4; ++I) O.Guid[I] = AssociatedGuid[I];
    O.Defaults = Defaults;
    O.NativeTail = NativeTail;
    ComponentOverrides.push_back(std::move(O));
}

void FBlueprintClass::Finish()
{
    const std::string CDOName = "Default__" + ClassName;

    const FIndex BpgcClass = EngineClass("/Script/Engine", "BlueprintGeneratedClass");
    const bool bWithinObject = WithinPackage == "/Script/CoreUObject" && WithinClass == "Object";
    const FIndex ObjectClass = bWithinObject ? EngineClass("/Script/CoreUObject", "Object") : Null();
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
    /* An overridden default subobject's archetype is the object of its name under its outer's archetype - the parent
       CDO's subobject (GetArchetypeFromRequiredInfo rule 1, UObjectArchetype.cpp 64-83) - and the cook writes it as the
       export's TemplateIndex: Ene_Butterfly's HealthComponent names
       ENE_FlyingCritterBase.Default__ENE_FlyingCritterBase_C:HealthComponent. The loader check()s it is set and fetches
       it serialized (AsyncLoading.cpp 2955, 3191-3193). A native parent's is found in memory; a Blueprint parent's is an
       export of its package, where the cook exports every default subobject and this compiler every one the parent or a
       subclass of it cooked here restates (Cpp.cpp, Generate). */
    std::vector<FIndex> SubobjectArchetypes;
    for (const FSubobjectOverride& O : SubobjectOverrides)
    {
        const FImport* SubClass = P.ImportAt(O.Class);
        SubobjectArchetypes.push_back(Subobject(P.ImportAt(SubClass->Outer)->ObjectName, SubClass->ObjectName, ParentCdo, O.Name));
    }

    const int32 RowClass = 0;
    const int32 RowCdo = 1;
    const int32 RowFirstFunction = 2;
    /* A native parent's overridden subobjects sit right after the functions, before anything the
       actor shape adds, so a non-actor class reaches them too. */
    const int32 RowFirstSubobject = RowFirstFunction + int32(Functions.size());
    const int32 RowRootTemplate = RowFirstSubobject + int32(SubobjectOverrides.size());
    const int32 RowScsNode = RowRootTemplate + 1;
    /* Each UE_COMPONENT takes two rows, archetype then node, so the SCS lands after them all. */
    const int32 RowFirstComponent = RowScsNode + 1;
    const int32 RowScs = RowFirstComponent + 2 * int32(Components.size());
    /* The handler, then one replacement archetype per overridden inherited component. */
    const int32 RowIch = RowScs + 1;
    const int32 RowFirstOverride = RowIch + 1;
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
    /* The CDO the class makes while it is serialized copies each default subobject from its archetype
       (UObjectGlobals.cpp 3822-3859), so a Blueprint parent's is loaded first, as Ene_Butterfly_C lists its parent's. */
    for (const FIndex A : SubobjectArchetypes) Class.SerBeforeSer.push_back(A.V);
    /* UStruct::GetPreloadDependencies (Class.cpp 732-735): the structs and enums the class's variables are typed by,
       which it links against while it is serialized. */
    for (const FPropertyDef& V : Vars) TypeRefs(V, Class.SerBeforeSer);
    /* The rest of UBlueprintGeneratedClass::GetPreloadDependencies (BlueprintGeneratedClass.cpp 1425-1459): Link preloads
       the ubergraph and the CDO's persistent frame is made only from a loaded one (1636, 1358-1375), and a component
       archetype lookup on the class meets RF_NeedLoad on an unloaded handler, a Fatal (858-866). */
    if (UberGraphFunction.V != 0) Class.SerBeforeSer.push_back(UberGraphFunction.V);
    if (!ComponentOverrides.empty()) Class.SerBeforeSer.push_back(Exp(RowIch).V);
    /* Every default subobject a Blueprint parent's CDO exports, restated here or not: the CDO this class makes while it
       is serialized copies each from that export as it stands (UObjectGlobals.cpp 3822-3859), so the cook maps it into
       the linker table and orders it first (SavePackage.cpp 4013-4040). Nested ones as well, named by their path under
       the CDO (Damage:BreakIceBonus_0, an object instanced in WPN_Pickaxe's Damage, which this class's Damage gets a
       copy of): the cook's walk takes every default subobject and archetype under the CDO, at any depth. Each is
       imported under its outer's import. */
    std::function<FIndex(const std::string&)> ParentSubobjectAt = [&](const std::string& Path) -> FIndex {
        const auto It = std::find_if(ParentSubobjects.begin(), ParentSubobjects.end(),
                                     [&](const FParentSubobject& S) { return Lower(S.Name) == Lower(Path); });
        const size_t Colon = Path.rfind(':');
        const FIndex Outer = Colon == std::string::npos ? ParentCdo : ParentSubobjectAt(Path.substr(0, Colon));
        if (It == ParentSubobjects.end() || Outer.V == 0) return Null();
        return Subobject(It->ClassPackage, It->ClassName, Outer, Path.substr(Colon + 1));
    };
    for (const FParentSubobject& S : ParentSubobjects)
        if (const FIndex Sub = ParentSubobjectAt(S.Name); Sub.V != 0) Class.SerBeforeSer.push_back(Sub.V);
    Class.SerBeforeCreate = { BpgcClass.V, BpgcCdo.V };
    Class.CreateBeforeCreate = { ParentIdx.V };
    for (int32 I = 0; I < NumFunctions; ++I)
        Class.CreateBeforeSer.push_back(Exp(RowFirstFunction + I).V);
    for (const FPropertyDef& V : Vars)      // a dispatcher's Extra is a function listed above
        if (V.Extra.V != 0 && std::find(Class.CreateBeforeSer.begin(), Class.CreateBeforeSer.end(), V.Extra.V) == Class.CreateBeforeSer.end())
            Class.CreateBeforeSer.push_back(V.Extra.V);
    for (FIndex I : Interfaces) Class.CreateBeforeSer.push_back(I.V);  // as BP_SentryGun_MoveMarker lists Targetable
    /* ABP_Amber_Depositbox lists its ClassWithin, SkeletalMeshComponent, there too; UObject is listed nowhere. */
    const FIndex Within = bWithinObject ? ObjectClass : EngineClass(WithinPackage, WithinClass);
    if (!bWithinObject) Class.CreateBeforeSer.push_back(Within.V);
    const std::string Config = ConfigName;
    const std::vector<FIndex> ClassInterfaces = Interfaces;
    const std::vector<FPropertyDef> ClassVars = Vars;
    const bool bActor = bIsActor;
    const uint32 Flags = ClassFlags;
    const FIndex UberGraph = UberGraphFunction;
    /* UBlueprintGeneratedClass::GetLifetimeBlueprintReplicationList stops after this many CPF_Net properties: without
       the tag nothing the class declares replicates. Measured on BP_LiftPod: the first tag. */
    const int32 NumOverrides = int32(ComponentOverrides.size());
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
        if (NumOverrides > 0)
            Tag(Ar, "InheritableComponentHandler", "ObjectProperty", [=](FArc& V) { V.Idx(Exp(RowIch)); });
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
        Ar.Idx(Within);                             // ClassWithin
        Ar.Name(Config);                            // ClassConfigName
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
    // AActor and UActorComponent default bCanEverTick to false; the BP compiler sets it on the CDO's
    // tick function - an actor's PrimaryActorTick, a component's PrimaryComponentTick - when ReceiveTick
    // is overridden (KismetCompiler.cpp:4738-4800, SetCanEverTick), else the object loads and never
    // ticks (Actor.cpp:914-925, ActorComponent.cpp:1038-1046). Any other class has no tick function.
    const bool bOverridesTick = (bIsActor || bIsComponent) && std::any_of(Functions.begin(), Functions.end(),
        [](const FPending& F) { return F.Def.Name == "ReceiveTick"; });
    const char* const TickProperty = bIsActor ? "PrimaryActorTick" : "PrimaryComponentTick";
    const char* const TickStruct = bIsActor ? "ActorTickFunction" : "ActorComponentTickFunction";

    const bool bCdoReplicates = bReplicates;
    const std::vector<FPropertyDef> Inherited = CdoDefaults;
    std::vector<std::string> SubobjectNames;
    for (const FSubobjectOverride& O : SubobjectOverrides) SubobjectNames.push_back(O.Property);
    for (size_t I = 0; I < SubobjectOverrides.size(); ++I) Cdo.CreateBeforeSer.push_back(Exp(RowFirstSubobject + int32(I)).V);
    Cdo.Serialize = [=](FArc& Ar) {
        if (bCdoReplicates) TagBool(Ar, "bReplicates", true);
        if (bOverridesTick)
            Tag(Ar, TickProperty, "StructProperty", [](FArc& V) {
                TagBool(V, "bCanEverTick", true);
                TagEnd(V);
            }, TickStruct);
        /* Only initialised members: an absent tag keeps the parent CDO's (zero) value. */
        for (const FPropertyDef& V : ClassVars)
            if (V.Default.K != FDefaultValue::None) WriteDefaultTag(Ar, V);
        /* A property an ancestor declares: UE_DEFAULTS writes it here, where re-declaring the name
           would instead shadow it with a second property of the same name on this class. */
        for (const FPropertyDef& V : Inherited) WriteDefaultTag(Ar, V);
        /* Each overridden native subobject is reachable from the CDO through its property, as
           Default__Ene_Butterfly_C points at its HealthComponent export. */
        for (size_t I = 0; I < SubobjectNames.size(); ++I)
            Tag(Ar, SubobjectNames[I], "ObjectProperty",
                [=](FArc& V) { V.Idx(Exp(RowFirstSubobject + int32(I))); });
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

    for (size_t I = 0; I < SubobjectOverrides.size(); ++I)
    {
        const FSubobjectOverride& O = SubobjectOverrides[I];
        const std::vector<FPropertyDef> Defaults = O.Defaults;
        FExport Sub;
        Sub.ClassIndex = O.Class;
        Sub.TemplateIndex = SubobjectArchetypes[I];
        Sub.OuterIndex = Exp(RowCdo);
        Sub.ObjectName = O.Name;
        Sub.ObjectFlags = RF_Public | RF_Transactional | RF_ArchetypeObject | RF_DefaultSubObject;
        Sub.SerBeforeCreate = { O.Class.V, Sub.TemplateIndex.V };      // as the cook lists class and template
        Sub.CreateBeforeCreate = { Exp(RowCdo).V };
        Sub.Serialize = [Defaults, Tail = O.NativeTail](FArc& Ar) {
            for (const FPropertyDef& V : Defaults) WriteDefaultTag(Ar, V);
            EndComponent(Ar, Tail);
        };
        P.AddExport(std::move(Sub));
    }

    if (!bIsActor) return;

    const FIndex SceneCompClass = EngineClass("/Script/Engine", "SceneComponent");
    const FIndex ScsNodeClass = EngineClass("/Script/Engine", "SCS_Node");
    const FIndex ScsClass = EngineClass("/Script/Engine", "SimpleConstructionScript");
    const FIndex SceneCompCdo = ClassDefaultObject("/Script/Engine", "SceneComponent");
    const FIndex ScsNodeCdo = ClassDefaultObject("/Script/Engine", "SCS_Node");
    const FIndex ScsCdo = ClassDefaultObject("/Script/Engine", "SimpleConstructionScript");

    /* No scene component of the class's own and no root inherited: the DefaultSceneRoot node stays listed, first, as the
       editor keeps it until a scene component can take its place, with no component at all too (40 of the game's
       classes, ENE_EnemySpawner). ExecuteScriptOnActor makes a root of its own only when RootNodes is empty
       (SimpleConstructionScript.cpp 640-703): an actor of movement components alone would end its construction with no
       RootComponent, and one of no components would get a plain SceneComponent, not net addressable as a node's
       component is (SCS_Node.cpp 107, ActorComponent.cpp 1901-1913). An actor that has a root before this SCS runs
       would skip the node (648), and the editor drops it from both lists then (ValidateSceneRootNodes, 1132-1150: a
       native root or scene component, or a scene root node of a parent Blueprint's, GetSceneRootComponentTemplate
       1029-1108), as all 1,885 of the game's SCS classes have it. Listed, it is a node like any other: its own
       VariableGuid is what a subclass's override of it is keyed on, and the components SetupAttachment(RootComponent)
       puts under it, with none of the class's own left to be the root, are its ChildNodes, as the editor keeps a
       component added under it. */
    const bool bKeepDefaultRoot = ListsDefaultRoot();
    uint32 DefaultRootGuid[4];
    ScsNodeGuid(ClassName, "DefaultSceneRoot", DefaultRootGuid);

    /*
    One archetype + one node per UE_COMPONENT, in declaration order. Measured on DRG's Ene_Butterfly:
    the node carries ComponentClass / ComponentTemplate / VariableGuid / InternalVariableName.
    USimpleConstructionScript::ExecuteScriptOnActor walks RootNodes in order, passing a null parent
    for the first scene component, which makes it the actor's root, and USCS_Node::ExecuteNodeOnActor
    attaches each of a node's ChildNodes to it. So every later scene component is a child of the first.
    Not a root node naming it in ParentComponentOrVariableName: the SCS's PostLoad
    (FixupRootNodeParentReferences, cooked builds too) looks such a name up only among native components
    and ancestor Blueprints' nodes, and clears it when the parent is a node of this same SCS.
    SetupAttachment (AttachComponent) moves a component off that root: under another of this class's, as one
    of its ChildNodes, or under an inherited one, as a root node that names it - which is how the game's own
    Blueprints save both (BP_PlayerCharacter's FilmFaceLight on CharacterMesh0, BP_PumpkinFace_Item's
    PointLight on BP_Pumpkin_Item_C's DefaultSceneRoot). The root is then the first scene component left alone.
    One attached to the actor's root (bRoot) goes where a component left alone goes, without being the root itself:
    a root node of no parent under an inherited root, else a child of this class's root, or of the DefaultSceneRoot
    node when none of the class's own is left to be it.
    */
    auto IndexOf = [&](const std::string& Name) {
        return int32(std::find_if(Components.begin(), Components.end(), [&](const FComponent& C) { return C.Name == Name; })
                     - Components.begin());
    };
    const int32 FirstScene = int32(std::find_if(Components.begin(), Components.end(), [](const FComponent& C) {
        return C.bIsScene && C.Attachment.Parent.empty() && !C.Attachment.bRoot;
    }) - Components.begin());
    const int32 UnderDefaultRoot = -2;
    std::vector<int32> ParentOf(Components.size(), -1);        // the node whose ChildNodes list it, -1 for a root node
    for (size_t I = 0; I < Components.size(); ++I)
    {
        const FComponent& C = Components[I];
        if (C.Attachment.bOwn) ParentOf[I] = IndexOf(C.Attachment.Parent);
        else if (C.Attachment.bRoot && !bRootInherited)
            ParentOf[I] = FirstScene < int32(Components.size()) ? FirstScene : UnderDefaultRoot;
        else if (C.bIsScene && C.Attachment.Parent.empty() && !C.Attachment.bRoot && int32(I) != FirstScene) ParentOf[I] = FirstScene;
    }
    std::vector<FIndex> DefaultRootChildren;
    for (size_t I = 0; I < Components.size(); ++I)
        if (ParentOf[I] == UnderDefaultRoot) DefaultRootChildren.push_back(Exp(RowFirstComponent + 2 * int32(I) + 1));

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
    for (const FIndex& Child : DefaultRootChildren) ScsNode.CreateBeforeSer.push_back(Child.V);
    ScsNode.SerBeforeCreate = { ScsNodeClass.V, ScsNodeCdo.V };
    ScsNode.CreateBeforeCreate = { Exp(RowScs).V };
    ScsNode.Serialize = [=](FArc& Ar) {
        Tag(Ar, "ComponentClass", "ObjectProperty", [=](FArc& V) { V.Idx(SceneCompClass); });
        Tag(Ar, "ComponentTemplate", "ObjectProperty",
            [=](FArc& V) { V.Idx(Exp(RowRootTemplate)); });
        if (!DefaultRootChildren.empty())
            Tag(Ar, "ChildNodes", "ArrayProperty", [=](FArc& V) {
                V.I32(int32(DefaultRootChildren.size()));
                for (const FIndex& Child : DefaultRootChildren) V.Idx(Child);
            }, "ObjectProperty");
        if (bKeepDefaultRoot)
            Tag(Ar, "VariableGuid", "StructProperty", [=](FArc& V) { V.Raw(DefaultRootGuid, 16); }, "Guid");
        Tag(Ar, "InternalVariableName", "NameProperty",
            [](FArc& V) { V.Name("DefaultSceneRoot"); });
        TagEnd(Ar);
        Ar.Bool(false);
    };
    P.AddExport(std::move(ScsNode));

    for (size_t I = 0; I < Components.size(); ++I)
    {
        const FComponent& C = Components[I];
        const int32 RowTemplate = RowFirstComponent + 2 * int32(I);
        const std::vector<FPropertyDef> Defaults = C.Defaults;

        FExport Template;
        Template.ClassIndex = C.Class;
        Template.TemplateIndex = C.Cdo;
        Template.OuterIndex = Exp(RowClass);
        Template.ObjectName = C.Name + "_GEN_VARIABLE";
        Template.ObjectFlags = RF_Public | RF_Transactional | RF_ArchetypeObject;
        Template.SerBeforeSer = { Exp(RowClass).V };
        Template.SerBeforeCreate = { C.Class.V, C.Cdo.V };
        Template.CreateBeforeCreate = { Exp(RowClass).V };
        Template.Serialize = [Defaults, Tail = C.NativeTail](FArc& Ar) {
            for (const FPropertyDef& V : Defaults) WriteDefaultTag(Ar, V);
            EndComponent(Ar, Tail);
        };
        P.AddExport(std::move(Template));

        uint32 NodeGuid[4];
        ScsNodeGuid(ClassName, C.Name, NodeGuid);
        std::vector<FIndex> Children;
        for (size_t J = 0; J < Components.size(); ++J)
            if (ParentOf[J] == int32(I)) Children.push_back(Exp(RowFirstComponent + 2 * int32(J) + 1));
        const FIndex CompClass = C.Class;
        const std::string VarName = C.Name;
        const FAttachment At = C.Attachment;

        FExport Node;
        Node.ClassIndex = ScsNodeClass;
        Node.TemplateIndex = ScsNodeCdo;
        Node.OuterIndex = Exp(RowScs);
        Node.ObjectName = "SCS_Node_" + std::to_string(I + 1);
        Node.ObjectFlags = RF_Transactional;
        Node.CreateBeforeSer = { Exp(RowTemplate).V, CompClass.V };
        for (const FIndex& Child : Children) Node.CreateBeforeSer.push_back(Child.V);
        Node.SerBeforeCreate = { ScsNodeClass.V, ScsNodeCdo.V };
        Node.CreateBeforeCreate = { Exp(RowScs).V };
        Node.Serialize = [=](FArc& Ar) {
            Tag(Ar, "ComponentClass", "ObjectProperty", [=](FArc& V) { V.Idx(CompClass); });
            Tag(Ar, "ComponentTemplate", "ObjectProperty", [=](FArc& V) { V.Idx(Exp(RowTemplate)); });
            /* USCS_Node's own order (SCS_Node.h 43-61), as the game's nodes keep it. */
            if (!At.Socket.empty()) Tag(Ar, "AttachToName", "NameProperty", [=](FArc& V) { V.Name(At.Socket); });
            if (!At.bOwn && !At.Parent.empty())
            {
                Tag(Ar, "ParentComponentOrVariableName", "NameProperty", [=](FArc& V) { V.Name(At.Parent); });
                if (At.bNative) TagBool(Ar, "bIsParentComponentNative", true);
                else Tag(Ar, "ParentComponentOwnerClassName", "NameProperty", [=](FArc& V) { V.Name(At.OwnerClass); });
            }
            if (!Children.empty())
                Tag(Ar, "ChildNodes", "ArrayProperty", [=](FArc& V) {
                    V.I32(int32(Children.size()));
                    for (const FIndex& Child : Children) V.Idx(Child);
                }, "ObjectProperty");
            Tag(Ar, "VariableGuid", "StructProperty", [=](FArc& V) { V.Raw(NodeGuid, 16); }, "Guid");
            Tag(Ar, "InternalVariableName", "NameProperty", [=](FArc& V) { V.Name(VarName); });
            TagEnd(Ar);
            Ar.Bool(false);
        };
        P.AddExport(std::move(Node));
    }

    FExport Scs;
    Scs.ClassIndex = ScsClass;
    Scs.TemplateIndex = ScsCdo;
    Scs.OuterIndex = Exp(RowClass);
    Scs.ObjectName = "SimpleConstructionScript_0";
    Scs.ObjectFlags = RF_Transactional;
    Scs.CreateBeforeSer = { Exp(RowScsNode).V };
    for (size_t I = 0; I < Components.size(); ++I)
        Scs.CreateBeforeSer.push_back(Exp(RowFirstComponent + 2 * int32(I) + 1).V);
    Scs.SerBeforeCreate = { ScsClass.V, ScsCdo.V };
    Scs.CreateBeforeCreate = { Exp(RowClass).V };
    std::vector<FIndex> Roots, All;            // Roots: every node no other node lists as a child
    if (bKeepDefaultRoot) { Roots.push_back(Exp(RowScsNode)); All.push_back(Exp(RowScsNode)); }
    for (size_t I = 0; I < Components.size(); ++I)
    {
        if (ParentOf[I] == -1) Roots.push_back(Exp(RowFirstComponent + 2 * int32(I) + 1));
        All.push_back(Exp(RowFirstComponent + 2 * int32(I) + 1));
    }
    Scs.Serialize = [=](FArc& Ar) {
        /* DefaultSceneRoot stays declared but drops out of both lists once a component can be the
           root, or the actor has one already, exactly as Ene_Butterfly saves it (while neither, it is in
           both: bKeepDefaultRoot). The lists are absent only when both are empty: a subclass's SCS
           with no component, below an inherited root. */
        if (!All.empty())
        {
            Tag(Ar, "RootNodes", "ArrayProperty", [=](FArc& V) {
                V.I32(int32(Roots.size()));
                for (const FIndex& Root : Roots) V.Idx(Root);
            }, "ObjectProperty");
            Tag(Ar, "AllNodes", "ArrayProperty", [=](FArc& V) {
                V.I32(int32(All.size()));
                for (const FIndex& Node : All) V.Idx(Node);
            }, "ObjectProperty");
        }
        Tag(Ar, "DefaultSceneRootNode", "ObjectProperty",
            [=](FArc& V) { V.Idx(Exp(RowScsNode)); });
        TagEnd(Ar);
        Ar.Bool(false);
    };
    P.AddExport(std::move(Scs));

    if (ComponentOverrides.empty()) return;

    /*
    The editor's "override an inherited component's defaults". UInheritableComponentHandler holds one
    record per overridden component; USCS_Node::GetActualComponentTemplate walks this class and its
    supers asking each handler for the parent node's FComponentKey, which matches on OwnerClass and
    AssociatedGuid only. Each record's template is a component archetyped on the PARENT's template, so
    its tags are exactly the deltas. CookedComponentInstancingData is left unwritten: the fast path
    only runs when bHasValidCookedData, and ExecuteNodeOnActor falls back to the template object.
    */
    const FIndex IchClass = EngineClass("/Script/Engine", "InheritableComponentHandler");
    const FIndex IchCdo = ClassDefaultObject("/Script/Engine", "InheritableComponentHandler");
    const std::vector<FComponentOverride> Overrides = ComponentOverrides;

    FExport Ich;
    Ich.ClassIndex = IchClass;
    Ich.TemplateIndex = IchCdo;
    Ich.OuterIndex = Exp(RowClass);
    Ich.ObjectName = "InheritableComponentHandler_0";
    Ich.ObjectFlags = RF_Public | RF_Transactional;
    Ich.SerBeforeCreate = { IchClass.V, IchCdo.V };
    Ich.CreateBeforeCreate = { Exp(RowClass).V };
    for (size_t I = 0; I < Overrides.size(); ++I)
        Ich.CreateBeforeSer.push_back(Exp(RowFirstOverride + int32(I)).V);
    Ich.Serialize = [=](FArc& Ar) {
        Tag(Ar, "Records", "ArrayProperty", [=](FArc& V) {
            /* An array of structs carries one inner FPropertyTag between the count and the elements,
               whose Size is their byte count - a zero there runs the loader off the end. */
            FArc Elements(V.Owner());
            for (size_t I = 0; I < Overrides.size(); ++I)
            {
                const FComponentOverride& O = Overrides[I];
                Tag(Elements, "ComponentClass", "ObjectProperty", [=](FArc& E) { E.Idx(O.Class); });
                Tag(Elements, "ComponentTemplate", "ObjectProperty",
                    [=](FArc& E) { E.Idx(Exp(RowFirstOverride + int32(I))); });
                Tag(Elements, "ComponentKey", "StructProperty", [=](FArc& E) {
                    Tag(E, "OwnerClass", "ObjectProperty", [=](FArc& K) { K.Idx(O.OwnerClass); });
                    Tag(E, "SCSVariableName", "NameProperty", [=](FArc& K) { K.Name(O.Name); });
                    Tag(E, "AssociatedGuid", "StructProperty", [=](FArc& K) { K.Raw(O.Guid, 16); }, "Guid");
                    TagEnd(E);
                }, "ComponentKey");
                TagEnd(Elements);
            }

            V.I32(int32(Overrides.size()));
            V.Name("Records");
            V.Name("StructProperty");
            V.I32(int32(Elements.B.size()));
            V.I32(0);                       // ArrayIndex
            V.Name("ComponentOverrideRecord");
            for (int32 I = 0; I < 4; ++I) V.U32(0);      // StructGuid
            V.U8(0);                        // HasPropertyGuid
            V.Append(Elements);
        }, "StructProperty");
        TagEnd(Ar);
        Ar.Bool(false);
    };
    P.AddExport(std::move(Ich));

    for (const FComponentOverride& O : Overrides)
    {
        const std::vector<FPropertyDef> Defaults = O.Defaults;
        FExport Template;
        Template.ClassIndex = O.Class;
        Template.TemplateIndex = O.ParentTemplate;
        Template.OuterIndex = Exp(RowClass);
        Template.ObjectName = O.Name + "_GEN_VARIABLE";
        Template.ObjectFlags = RF_Public | RF_ArchetypeObject | RF_InheritableComponentTemplate;
        Template.SerBeforeCreate = { O.Class.V, O.ParentTemplate.V };
        Template.CreateBeforeCreate = { Exp(RowClass).V };
        Template.Serialize = [Defaults, Tail = O.NativeTail](FArc& Ar) {
            for (const FPropertyDef& V : Defaults) WriteDefaultTag(Ar, V);
            EndComponent(Ar, Tail);
        };
        P.AddExport(std::move(Template));
    }
}

/* Measured on DRG's MM_ResourceInfo: Guid tag, empty UStruct body, StructFlags 0, then the default
   instance as a tag per member. */
bool FBlueprintClass::WriteApi(const std::string& OutDir, std::string* Err) const
{
    FApiClass Api;
    /* ClassName is the runtime "<Asset>_C"; the asset it belongs to drops that suffix. */
    Api.AssetName = ClassName.size() > 2 && ClassName.compare(ClassName.size() - 2, 2, "_C") == 0
                  ? ClassName.substr(0, ClassName.size() - 2) : ClassName;
    Api.PackageName = P.Name().substr(0, P.Name().rfind('/'));
    Api.ParentPackage = ParentPackage;
    Api.ParentClass = ParentClass;
    for (const FPending& Fn : Functions)
        Api.Functions.push_back({ Fn.Def.Name, Fn.Def.Params, Fn.Def.FunctionFlags });
    Api.Categories = ApiCategory;
    /* Only what the outside can see. The rest - the UberGraphFrame pointer above all - is
       compiler plumbing that would show up as a broken variable in the editor. */
    for (const FPropertyDef& Var : Vars)
        if ((Var.PropertyFlags & CPF_BlueprintVisible) && !Var.bApiHidden) Api.Variables.push_back(Var);
    return WriteApiAsset(Api, P, OutDir, Err);
}

bool FBlueprintClass::WriteApiStruct(const std::string& OutDir, const uint32 (&Guid)[4], std::string* Err) const
{
    FApiStruct Api;
    Api.StructName = ClassName;
    Api.PackageName = P.Name();          // the cooked struct's own package path, reused for the stub
    Api.Members = Vars;
    for (int32 I = 0; I < 4; ++I) Api.Guid[I] = Guid[I];
    return WriteApiStructAsset(Api, P, OutDir, Err);
}

bool FBlueprintClass::WriteApiEnum(const std::string& OutDir,
                                   const std::vector<std::pair<std::string, int64>>& Enumerators,
                                   std::string* Err) const
{
    FApiEnum Api;
    Api.EnumName = ClassName;
    Api.PackageName = P.Name();
    Api.Entries = Enumerators;
    return WriteApiEnumAsset(Api, OutDir, Err);
}

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
    for (const FPropertyDef& V : Vars) TypeRefs(V, S.SerBeforeSer);     // what its members link against (Class.cpp 732-735)

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
