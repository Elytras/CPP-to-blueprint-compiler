#include "Blueprint.h"

#include "Uncooked.h"

#include <algorithm>
#include <array>

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

std::string TimelineGuid(const std::string& ClassName, const std::string& Timeline, uint32 (&Out)[4])
{
    ScsNodeGuid(ClassName, "timeline." + Timeline, Out);
    char Hex[33];
    snprintf(Hex, sizeof(Hex), "%08X%08X%08X%08X", Out[0], Out[1], Out[2], Out[3]);   // FGuid::ToString, EGuidFormats::Digits
    return Hex;
}

/* An array of structs as the cook writes one: the count, one inner FPropertyTag whose Size is all the elements' bytes,
   then the elements, each written whole (no defaults to leave a field out against). */
static void StructArray(FArc& V, const std::string& Name, const std::string& StructName, int32 Count, const FArc& Elements)
{
    V.I32(Count);
    V.Name(Name);
    V.Name("StructProperty");
    V.I32(int32(Elements.B.size()));
    V.I32(0);                       // ArrayIndex
    V.Name(StructName);
    for (int32 I = 0; I < 4; ++I) V.U32(0);      // StructGuid
    V.U8(0);                        // HasPropertyGuid
    V.Append(Elements);
}

/* FRichCurve::AutoSetTangents (RichCurve.cpp 623-711) at the editor's tension 0, over one channel of keys sorted by
   time, which the curve editor runs after every edit: a cubic key's Auto tangents. Engine evaluation reads the stored
   tangents, so they are what the curve plays. Out holds each key's arrive and leave tangent. */
static void AutoTangents(const std::vector<FBlueprintClass::FTimelineKey>& Keys, int32 Channel,
                         std::vector<std::pair<float, float>>& Out)
{
    const int32 N = int32(Keys.size());
    Out.assign(size_t(N), { 0.0f, 0.0f });
    auto V = [&](int32 K) { return Keys[size_t(K)].Values[Channel]; };
    for (int32 K = 0; K < N; ++K)
    {
        const uint8 Interp = Keys[size_t(K)].Interp;
        float Arrive = 0.0f, Leave = 0.0f;
        int32 Prev = K, Cur = K, Next = K;
        bool bCompute = false;
        if (K == 0)
        {
            if (N > 1 && Interp == 2) { Next = 1; Cur = 1; bCompute = true; }
        }
        else if (K < N - 1)
        {
            Prev = K - 1;
            if (Interp == 2) { Next = K + 1; bCompute = true; }
            /* Linear and constant keys keep the zero tangents a new key starts with. */
        }
        else if (Interp == 2) { Prev = K - 1; bCompute = true; }
        if (bCompute)
        {
            /* ComputeCurveTangent: (1 - Tension) * ((P - PrevP) + (NextP - P)) / max(KINDA_SMALL_NUMBER, NextT - PrevT). */
            const float Tangent = 1.0f * ((V(Cur) - V(Prev)) + (V(Next) - V(Cur)));
            Arrive = Tangent / std::max(1.e-4f, Keys[size_t(Next)].Time - Keys[size_t(Prev)].Time);
            Leave = Arrive;
        }
        Out[size_t(K)] = { Arrive, Leave };
    }
}

/* One channel as an FRichCurve value: its Keys, each FRichCurveKey in its native 27 bytes (RichCurve.cpp 160-180), and
   nothing else, as the cook leaves DefaultValue and the extrapolations at their defaults. */
static void RichCurve(FArc& Ar, const std::vector<FBlueprintClass::FTimelineKey>& Keys, int32 Channel)
{
    std::vector<std::pair<float, float>> Tangents;
    AutoTangents(Keys, Channel, Tangents);
    Tag(Ar, "Keys", "ArrayProperty", [&](FArc& V) {
        FArc Elements(V.Owner());
        for (size_t K = 0; K < Keys.size(); ++K)
        {
            const float Fields[6] = { Keys[K].Time, Keys[K].Values[Channel], Tangents[K].first, 0.0f, Tangents[K].second, 0.0f };
            Elements.U8(Keys[K].Interp);
            Elements.U8(0);                 // TangentMode RCTM_Auto
            Elements.U8(0);                 // TangentWeightMode RCTWM_WeightedNone
            Elements.Raw(Fields, sizeof(Fields));     // Time, Value, ArriveTangent, ArriveTangentWeight, LeaveTangent, LeaveTangentWeight
        }
        StructArray(V, "Keys", "RichCurveKey", int32(Keys.size()), Elements);
    }, "StructProperty");
    TagEnd(Ar);
}

/* A curve object's body: UCurveFloat's FloatCurve; UCurveVector's FloatCurves[3] and UCurveLinearColor's FloatCurves[4],
   one tag per channel; an event curve's flag. A track's own curve and a curve asset (Hologram_FadeIn) alike. */
static void CurveBody(FArc& Ar, const FBlueprintClass::FTimelineTrack& Track)
{
    const int32 Channels = Track.Kind == 'V' ? 3 : Track.Kind == 'C' ? 4 : 1;
    for (int32 Ch = 0; Ch < Channels; ++Ch)
        Tag(Ar, Channels == 1 ? "FloatCurve" : "FloatCurves", "StructProperty",
            [&](FArc& V) { RichCurve(V, Track.Keys, Ch); }, "RichCurve", Ch);
    if (Track.Kind == 'E') TagBool(Ar, "bIsEventCurve", true);
    TagEnd(Ar);
    Ar.Bool(false);
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

    // A /Game class is a BlueprintGeneratedClass or the kind GameClassKinds gives, not a Class; the wrong row fails at
    // load, not write.
    const bool bBlueprint = PackageName.compare(0, 6, "/Game/") == 0;
    std::pair<std::string, std::string> Kind = bBlueprint ? std::make_pair(std::string("/Script/Engine"), std::string("BlueprintGeneratedClass"))
                                                          : std::make_pair(std::string("/Script/CoreUObject"), std::string("Class"));
    if (bBlueprint && GameClassKinds)
        if (auto K = GameClassKinds->find(PackageName + "." + ClassName_); K != GameClassKinds->end()) Kind = K->second;
    const FIndex Outer = PackageImport(PackageName);
    const int32 Row = P.AddImport({ Kind.first, Kind.second, Outer, ClassName_ });
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

void FBlueprintClass::SetComponentGuid(const std::string& Name, const uint32 (&Guid)[4])
{
    for (FComponent& C : Components)
        if (C.Name == Name) { C.bGuid = true; std::copy(std::begin(Guid), std::end(Guid), C.Guid); }
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

void FBlueprintClass::AddWidget(const std::string& Name, FIndex Class, FIndex Cdo, const std::vector<FPropertyDef>& Defaults)
{
    Widgets.push_back(FWidget{ Name, Class, Cdo, Defaults });
}

void FBlueprintClass::AddWidgetSlot(const std::string& Panel, const std::string& Child, FIndex Class, FIndex Cdo,
                                    const std::vector<FPropertyDef>& Defaults)
{
    WidgetSlots.push_back(FWidgetSlot{ Panel, Child, Class, Cdo, Defaults });
}

/* The objects a default's tags name, a lone struct's members included, which the export creates before it is
   serialized (as ButtonText lists its font). */
static void TagRefs(const FPropertyDef& P, std::vector<int32>& Out)
{
    DefaultRefs(P.Default, Out);
    if (P.Members) for (const FPropertyDef& M : *P.Members) TagRefs(M, Out);
}

void FBlueprintClass::Finish()
{
    const std::string CDOName = "Default__" + ClassName;

    const FIndex BpgcClass = bWidgetClass ? EngineClass("/Script/UMG", "WidgetBlueprintGeneratedClass")
                                          : EngineClass("/Script/Engine", "BlueprintGeneratedClass");
    const bool bWithinObject = WithinPackage == "/Script/CoreUObject" && WithinClass == "Object";
    const FIndex ObjectClass = bWithinObject ? EngineClass("/Script/CoreUObject", "Object") : Null();
    const FIndex FunctionClass = EngineClass("/Script/CoreUObject", "Function");
    const FIndex EnginePkg = PackageImport("/Script/Engine");
    const FIndex CorePkg = PackageImport("/Script/CoreUObject");
    const FIndex BpgcCdo = bWidgetClass ? Imp(P.AddImport({ "/Script/UMG", "WidgetBlueprintGeneratedClass", PackageImport("/Script/UMG"),
                                                            "Default__WidgetBlueprintGeneratedClass" }))
                                        : Imp(P.AddImport({ "/Script/Engine", "BlueprintGeneratedClass",
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
    /* Then the input binding objects, an InputKeyDelegateBinding and an InputActionDelegateBinding, each when the class
       has a binding of its kind. */
    std::vector<std::vector<FInputBinding>> BindingKinds;
    for (const bool bAction : { false, true })
    {
        std::vector<FInputBinding> Of;
        for (const FInputBinding& B : InputBindings) if (B.bAction == bAction) Of.push_back(B);
        if (!Of.empty()) BindingKinds.push_back(Of);
    }
    const int32 RowFirstBinding = RowFirstSubobject + int32(SubobjectOverrides.size());
    /* Then a widget class's ComponentDelegateBinding, when its layout binds a widget's event (S39). */
    const int32 RowComponentBinding = RowFirstBinding + int32(BindingKinds.size());
    /* Then each timeline's template, followed by its tracks' curves. */
    const int32 RowFirstTimeline = RowComponentBinding + (ComponentBindings.empty() ? 0 : 1);
    std::vector<int32> TimelineRows;
    int32 NumTimelineRows = 0;
    for (const FTimeline& T : Timelines)
    {
        TimelineRows.push_back(RowFirstTimeline + NumTimelineRows);
        NumTimelineRows += 1 + int32(std::count_if(T.Tracks.begin(), T.Tracks.end(), [](const FTimelineTrack& K) { return !K.External.V; }));
    }
    /* Then a widget class's tree: the WidgetTree, its widgets in declaration order, their slots. */
    const int32 RowTree = RowFirstTimeline + NumTimelineRows;
    const int32 RowFirstWidget = RowTree + 1;
    const int32 RowFirstWidgetSlot = RowFirstWidget + int32(Widgets.size());
    const int32 NumTreeRows = Widgets.empty() ? 0 : 1 + int32(Widgets.size() + WidgetSlots.size());
    /* Then each widget animation: the WidgetAnimation, its MovieScene, and a track and its section per widget it keys. */
    const int32 RowFirstAnimation = RowTree + NumTreeRows;
    std::vector<int32> AnimationRows;
    int32 NumAnimationRows = 0;
    for (const FAnimation& A : Animations)
    {
        AnimationRows.push_back(RowFirstAnimation + NumAnimationRows);
        NumAnimationRows += 2 + 2 * int32(A.Tracks.size());
    }
    const int32 RowRootTemplate = RowFirstAnimation + NumAnimationRows;
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
    std::vector<FIndex> Bindings;               // DynamicBindingObjects, listed first as BP_PlayerControllerBase_C lists them
    for (size_t I = 0; I < BindingKinds.size(); ++I)
    {
        Bindings.push_back(Exp(RowFirstBinding + int32(I)));
        Class.CreateBeforeSer.push_back(Bindings.back().V);
    }
    if (!ComponentBindings.empty())
    {
        Bindings.push_back(Exp(RowComponentBinding));
        Class.CreateBeforeSer.push_back(Bindings.back().V);
    }
    const bool bHasTree = !Widgets.empty();
    if (bHasTree) Class.CreateBeforeSer.push_back(Exp(RowTree).V);      // listed first, as Basic_FlatButton_C lists it
    std::vector<FIndex> AnimationObjects;
    for (const int32 Row : AnimationRows)
    {
        AnimationObjects.push_back(Exp(Row));
        Class.CreateBeforeSer.push_back(Exp(Row).V);
    }
    std::vector<FIndex> Templates;              // the Timelines list, after the bindings as BP_FragilePlatform_C lists them
    for (const int32 Row : TimelineRows)
    {
        Templates.push_back(Exp(Row));
        Class.CreateBeforeSer.push_back(Exp(Row).V);
    }
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
    const std::vector<FPropertyBinding> WidgetBindings = PropertyBindings;
    const std::vector<std::string> SlotNames = NamedSlots;
    Class.Serialize = [=](FArc& Ar) {
        /* UWidgetBlueprintGeneratedClass's own properties, so before UBlueprintGeneratedClass's, as the cook orders them. */
        if (bHasTree) Tag(Ar, "WidgetTree", "ObjectProperty", [=](FArc& V) { V.Idx(Exp(RowTree)); });
        /* Measured on Basic_DropdownItem_C's: each FDelegateRuntimeBinding written whole, its SourcePath one segment
           naming the function. InitializeBindingsStatic binds through that path when a native binder takes the property
           (a text, a color, a brush), and to FunctionName by name otherwise. */
        if (!WidgetBindings.empty())
            Tag(Ar, "Bindings", "ArrayProperty", [=](FArc& V) {
                FArc Elements(V.Owner());
                for (const FPropertyBinding& B : WidgetBindings)
                {
                    Tag(Elements, "ObjectName", "StrProperty", [=](FArc& E) { E.Str(B.Widget); });    // a C++ name, so ASCII
                    Tag(Elements, "PropertyName", "NameProperty", [=](FArc& E) { E.Name(B.Property); });
                    Tag(Elements, "FunctionName", "NameProperty", [=](FArc& E) { E.Name(B.Function); });
                    Tag(Elements, "SourcePath", "StructProperty", [=](FArc& E) {
                        Tag(E, "Segments", "ArrayProperty", [=](FArc& S) {
                            FArc Segment(S.Owner());
                            Tag(Segment, "Name", "NameProperty", [=](FArc& N) { N.Name(B.Function); });
                            Tag(Segment, "ArrayIndex", "IntProperty", [](FArc& N) { N.I32(-1); });
                            TagEnd(Segment);
                            StructArray(S, "Segments", "PropertyPathSegment", 1, Segment);
                        }, "StructProperty");
                        Tag(E, "CachedFunction", "ObjectProperty", [](FArc& N) { N.Idx(Null()); });
                        TagEnd(E);
                    }, "DynamicPropertyPath");
                    Tag(Elements, "Kind", "EnumProperty", [](FArc& E) { E.Name("EBindingKind::Function"); }, "EBindingKind");
                    TagEnd(Elements);
                }
                StructArray(V, "Bindings", "DelegateRuntimeBinding", int32(WidgetBindings.size()), Elements);
            }, "StructProperty");
        /* UWidgetBlueprintGeneratedClass::InitializeWidgetStatic sets each animation variable by its MovieScene's name. */
        if (!AnimationObjects.empty())
            Tag(Ar, "Animations", "ArrayProperty", [=](FArc& V) {
                V.I32(int32(AnimationObjects.size()));
                for (const FIndex& A : AnimationObjects) V.Idx(A);
            }, "ObjectProperty");
        if (!SlotNames.empty())
            Tag(Ar, "NamedSlots", "ArrayProperty", [=](FArc& V) {
                V.I32(int32(SlotNames.size()));
                for (const std::string& N : SlotNames) V.Name(N);
            }, "NameProperty");
        if (NumReplicated > 0)
            Tag(Ar, "NumReplicatedProperties", "IntProperty", [=](FArc& V) { V.I32(NumReplicated); });
        /* UBlueprintGeneratedClass::GetDynamicBindingObject looks a binding object up here by its class. */
        if (!Bindings.empty())
            Tag(Ar, "DynamicBindingObjects", "ArrayProperty", [=](FArc& V) {
                V.I32(int32(Bindings.size()));
                for (const FIndex& B : Bindings) V.Idx(B);
            }, "ObjectProperty");
        /* AActor::ExecuteConstruction makes a component of each (UBlueprintGeneratedClass::CreateComponentsForActor). */
        if (!Templates.empty())
            Tag(Ar, "Timelines", "ArrayProperty", [=](FArc& V) {
                V.I32(int32(Templates.size()));
                for (const FIndex& T : Templates) V.Idx(T);
            }, "ObjectProperty");
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
        // 5.6 UBlueprintGeneratedClass::Serialize, once FilterEditorOnly: CookedEditorTags, a TMap<FName, FString>
        // the runtime reads and drops (the editor writes BlueprintType). Leaving it out reads past the payload.
        if (IsUE5()) Ar.I32(0);
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

    /* A UE_DEFAULTS bReplicates of its own wins: one tag, the class's word (a component the editor left unreplicated). */
    const bool bCdoReplicates = bReplicates && std::none_of(CdoDefaults.begin(), CdoDefaults.end(),
        [](const FPropertyDef& V) { return V.Name == "bReplicates"; });
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
        // 5.6 UBlueprintGeneratedClass::SerializeDefaultObject then reads SparseClassDataStruct; null makes the loader
        // seek to the export's end. Leaving it out reads the next export's first bytes as an object index.
        if (IsUE5()) Ar.Idx(Null());
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

    /* Measured on BP_PlayerCameraDrone's InputKeyDelegateBinding_0 and BP_PlayerControllerBase's
       InputActionDelegateBinding_0: the class's subobject with no object flags, and every field of every entry written,
       as the cook saves an array's structs whole (no defaults to leave a field out against). The fields go in the
       struct's link order, its own before FBlueprintInputDelegateBinding's three flags. */
    for (const std::vector<FInputBinding>& Of : BindingKinds)
    {
        const bool bAction = Of.front().bAction;
        const std::string Kind = bAction ? "InputActionDelegateBinding" : "InputKeyDelegateBinding";
        const FIndex KindClass = EngineClass("/Script/Engine", Kind);
        const FIndex KindCdo = ClassDefaultObject("/Script/Engine", Kind);
        FExport Binding;
        Binding.ClassIndex = KindClass;
        Binding.TemplateIndex = KindCdo;
        Binding.OuterIndex = Exp(RowClass);
        Binding.ObjectName = Kind + "_0";
        Binding.SerBeforeCreate = { KindClass.V, KindCdo.V };
        Binding.CreateBeforeCreate = { Exp(RowClass).V };
        Binding.Serialize = [=](FArc& Ar) {
            static const char* const Events[] = { "IE_Pressed", "IE_Released", "IE_Repeat", "IE_DoubleClick" };
            Tag(Ar, Kind + "s", "ArrayProperty", [=](FArc& V) {
                FArc Elements(V.Owner());
                for (const FInputBinding& B : Of)
                {
                    if (bAction)
                        Tag(Elements, "InputActionName", "NameProperty", [=](FArc& E) { E.Name(B.Name); });
                    else
                        Tag(Elements, "InputChord", "StructProperty", [=](FArc& E) {
                            Tag(E, "Key", "StructProperty", [=](FArc& K) {
                                Tag(K, "KeyName", "NameProperty", [=](FArc& N) { N.Name(B.Name); });
                                TagEnd(K);
                            }, "Key");
                            TagBool(E, "bShift", B.bShift);
                            TagBool(E, "bCtrl", B.bCtrl);
                            TagBool(E, "bAlt", B.bAlt);
                            TagBool(E, "bCmd", B.bCmd);
                            TagEnd(E);
                        }, "InputChord");
                    Tag(Elements, "InputKeyEvent", "ByteProperty", [=](FArc& E) { E.Name(Events[B.Event]); }, "EInputEvent");
                    Tag(Elements, "FunctionNameToBind", "NameProperty", [=](FArc& E) { E.Name(B.Function); });
                    TagBool(Elements, "bConsumeInput", B.bConsumeInput);
                    TagBool(Elements, "bExecuteWhenPaused", B.bExecuteWhenPaused);
                    TagBool(Elements, "bOverrideParentBinding", B.bOverrideParentBinding);
                    TagEnd(Elements);
                }
                StructArray(V, Kind + "s", "Blueprint" + Kind, int32(Of.size()), Elements);
            }, "StructProperty");
            TagEnd(Ar);
            Ar.Bool(false);
        };
        P.AddExport(std::move(Binding));
    }

    /* Measured on Basic_FlatButton's ComponentDelegateBinding_0: the class's subobject with no object flags, each
       FBlueprintComponentDelegateBinding written whole. */
    if (!ComponentBindings.empty())
    {
        const FIndex KindClass = EngineClass("/Script/Engine", "ComponentDelegateBinding");
        const FIndex KindCdo = ClassDefaultObject("/Script/Engine", "ComponentDelegateBinding");
        FExport Binding;
        Binding.ClassIndex = KindClass;
        Binding.TemplateIndex = KindCdo;
        Binding.OuterIndex = Exp(RowClass);
        Binding.ObjectName = "ComponentDelegateBinding_0";
        Binding.SerBeforeCreate = { KindClass.V, KindCdo.V };
        Binding.CreateBeforeCreate = { Exp(RowClass).V };
        Binding.Serialize = [Of = ComponentBindings](FArc& Ar) {
            Tag(Ar, "ComponentDelegateBindings", "ArrayProperty", [&](FArc& V) {
                FArc Elements(V.Owner());
                for (const FComponentBinding& B : Of)
                {
                    Tag(Elements, "ComponentPropertyName", "NameProperty", [&](FArc& E) { E.Name(B.Component); });
                    Tag(Elements, "DelegatePropertyName", "NameProperty", [&](FArc& E) { E.Name(B.Delegate); });
                    Tag(Elements, "FunctionNameToBind", "NameProperty", [&](FArc& E) { E.Name(B.Function); });
                    TagEnd(Elements);
                }
                StructArray(V, "ComponentDelegateBindings", "BlueprintComponentDelegateBinding", int32(Of.size()), Elements);
            }, "StructProperty");
            TagEnd(Ar);
            Ar.Bool(false);
        };
        P.AddExport(std::move(Binding));
    }

    /* Measured on BP_LostPackStart's LightTimeline_Template and its CurveFloat_0 / CurveFloat_1, BP_DorettaHead's
       CurveVector_0: the template is the class's subobject <Timeline>_Template, flag Transactional, and each curve the
       class's subobject too, Public | Transactional, named as MakeUniqueCurveName numbers them, by curve class. The
       template's tags go in declaration order and leave what is at the engine's defaults out (a length of 5, no
       options); each track struct is written whole. */
    std::map<std::string, int32> CurveNumbers;
    for (size_t TI = 0; TI < Timelines.size(); ++TI)
    {
        const FTimeline T = Timelines[TI];
        const int32 Row = TimelineRows[TI];
        /* A track over a curve asset names its import, and only the others have a curve export of their own. */
        std::vector<FIndex> Curves;
        int32 Own = 0;
        for (const FTimelineTrack& Track : T.Tracks) Curves.push_back(Track.External.V ? Track.External : Exp(Row + 1 + Own++));

        const FIndex TemplateClass = EngineClass("/Script/Engine", "TimelineTemplate");
        const FIndex TemplateCdo = ClassDefaultObject("/Script/Engine", "TimelineTemplate");
        FExport Template;
        Template.ClassIndex = TemplateClass;
        Template.TemplateIndex = TemplateCdo;
        Template.OuterIndex = Exp(RowClass);
        Template.ObjectName = T.Name + "_Template";
        Template.ObjectFlags = RF_Transactional;
        Template.SerBeforeCreate = { TemplateClass.V, TemplateCdo.V };
        Template.CreateBeforeCreate = { Exp(RowClass).V };
        for (const FIndex C : Curves) Template.CreateBeforeSer.push_back(C.V);
        Template.Serialize = [=](FArc& Ar) {
            if (T.Length != 5.0f) Tag(Ar, "TimelineLength", "FloatProperty", [=](FArc& V) { V.Raw(&T.Length, 4); });
            if (T.bLastKeyframe)
                Tag(Ar, "LengthMode", "ByteProperty", [](FArc& V) { V.Name("TL_LastKeyFrame"); }, "ETimelineLengthMode");
            if (T.bAutoPlay) TagBool(Ar, "bAutoPlay", true);
            if (T.bLoop) TagBool(Ar, "bLoop", true);
            if (T.bReplicated) TagBool(Ar, "bReplicated", true);
            if (T.bIgnoreTimeDilation) TagBool(Ar, "bIgnoreTimeDilation", true);
            /* FTTEventTrack, FTTFloatTrack, FTTVectorTrack, FTTLinearColorTrack: the struct's own fields, then
               FTTPropertyTrack's PropertyName, then FTTTrackBase's. */
            static const struct { char Kind; const char *Array, *Struct, *Curve; } Kinds[] = {
                { 'E', "EventTracks", "TTEventTrack", "CurveKeys" }, { 'F', "FloatTracks", "TTFloatTrack", "CurveFloat" },
                { 'V', "VectorTracks", "TTVectorTrack", "CurveVector" }, { 'C', "LinearColorTracks", "TTLinearColorTrack", "CurveLinearColor" } };
            for (const auto& Of : Kinds)
            {
                FArc Elements(Ar.Owner());
                int32 Count = 0;
                for (size_t K = 0; K < T.Tracks.size(); ++K)
                {
                    const FTimelineTrack& Track = T.Tracks[K];
                    if (Track.Kind != Of.Kind) continue;
                    ++Count;
                    if (Of.Kind == 'E') Tag(Elements, "FunctionName", "NameProperty", [&](FArc& E) { E.Name(Track.Name); });
                    Tag(Elements, Of.Curve, "ObjectProperty", [&](FArc& E) { E.Idx(Curves[K]); });
                    if (Of.Kind != 'E') Tag(Elements, "PropertyName", "NameProperty", [&](FArc& E) { E.Name(Track.Name); });
                    Tag(Elements, "TrackName", "NameProperty", [&](FArc& E) { E.Name(Track.Name); });
                    TagBool(Elements, "bIsExternalCurve", Track.External.V != 0);
                    TagEnd(Elements);
                }
                if (Count > 0)
                    Tag(Ar, Of.Array, "ArrayProperty", [&](FArc& V) { StructArray(V, Of.Array, Of.Struct, Count, Elements); },
                        "StructProperty");
            }
            Tag(Ar, "TimelineGuid", "StructProperty", [=](FArc& V) { V.Guid(T.Guid); }, "Guid");
            Tag(Ar, "VariableName", "NameProperty", [=](FArc& V) { V.Name(T.Name); });
            Tag(Ar, "DirectionPropertyName", "NameProperty", [=](FArc& V) { V.Name(T.DirectionProperty); });
            /* A pin with no method names none: the engine's ExecuteIfBound then calls nothing. */
            if (!T.Update.empty()) Tag(Ar, "UpdateFunctionName", "NameProperty", [=](FArc& V) { V.Name(T.Update); });
            if (!T.Finished.empty()) Tag(Ar, "FinishedFunctionName", "NameProperty", [=](FArc& V) { V.Name(T.Finished); });
            TagEnd(Ar);
            Ar.Bool(false);
        };
        P.AddExport(std::move(Template));

        for (const FTimelineTrack& Track : T.Tracks)
        {
            if (Track.External.V) continue;
            const std::string CurveClass = Track.Kind == 'V' ? "CurveVector" : Track.Kind == 'C' ? "CurveLinearColor" : "CurveFloat";
            const FIndex KindClass = EngineClass("/Script/Engine", CurveClass);
            const FIndex KindCdo = ClassDefaultObject("/Script/Engine", CurveClass);
            FExport Curve;
            Curve.ClassIndex = KindClass;
            Curve.TemplateIndex = KindCdo;
            Curve.OuterIndex = Exp(RowClass);
            Curve.ObjectName = CurveClass + "_" + std::to_string(CurveNumbers[CurveClass]++);
            Curve.ObjectFlags = RF_Public | RF_Transactional;
            Curve.SerBeforeCreate = { KindClass.V, KindCdo.V };
            Curve.CreateBeforeCreate = { Exp(RowClass).V };
            Curve.Serialize = [Track](FArc& Ar) { CurveBody(Ar, Track); };
            P.AddExport(std::move(Curve));
        }
    }

    /* S39: the widget tree, as DRG's cook writes Basic_FlatButton's (DESIGN.md → S39). Every widget's outer is the tree,
       every slot's its panel; the hierarchy is the slots' Parent and Content, the panel's Slots and the child's Slot, all
       three written. All are RF_Transactional alone. Each export creates first the objects its tags name but its outer:
       the tree its root, a widget its slots, a slot its content. */
    if (bHasTree)
    {
        auto WidgetRow = [&](const std::string& Name) {
            return RowFirstWidget + int32(std::find_if(Widgets.begin(), Widgets.end(),
                                                        [&](const FWidget& W) { return W.Name == Name; }) - Widgets.begin());
        };
        const FIndex TreeClass = EngineClass("/Script/UMG", "WidgetTree");
        const FIndex TreeCdo = Imp(P.AddImport({ "/Script/UMG", "WidgetTree", PackageImport("/Script/UMG"), "Default__WidgetTree" }));
        FExport Tree;
        Tree.ClassIndex = TreeClass;
        Tree.TemplateIndex = TreeCdo;
        Tree.OuterIndex = Exp(RowClass);
        Tree.ObjectName = "WidgetTree";
        Tree.ObjectFlags = RF_Transactional;
        Tree.CreateBeforeSer = { Exp(RowFirstWidget).V };
        Tree.SerBeforeCreate = { TreeClass.V, TreeCdo.V };
        Tree.CreateBeforeCreate = { Exp(RowClass).V };
        Tree.Serialize = [=](FArc& Ar) {
            Tag(Ar, "RootWidget", "ObjectProperty", [=](FArc& V) { V.Idx(Exp(RowFirstWidget)); });
            TagEnd(Ar);
            Ar.Bool(false);
        };
        P.AddExport(std::move(Tree));

        /* A slot is named <SlotClass>_<n>, counted per panel and class (CanvasPanelSlot_0, OverlaySlot_1). */
        std::vector<std::string> SlotNames;
        std::map<std::string, int32> Counts;
        for (const FWidgetSlot& S : WidgetSlots)
        {
            const std::string Of = P.ImportAt(S.Class)->ObjectName;
            SlotNames.push_back(Of + "_" + std::to_string(Counts[S.Panel + "." + Of]++));
        }
        for (const FWidget& W : Widgets)
        {
            std::vector<FIndex> Slots;
            FIndex Own;
            for (size_t I = 0; I < WidgetSlots.size(); ++I)
            {
                if (WidgetSlots[I].Panel == W.Name) Slots.push_back(Exp(RowFirstWidgetSlot + int32(I)));
                if (WidgetSlots[I].Child == W.Name) Own = Exp(RowFirstWidgetSlot + int32(I));
            }
            /* A nested user widget's named slots this tree fills, {slot name, content}. */
            std::vector<std::pair<std::string, FIndex>> Fills;
            for (const FWidgetSlotFill& F : WidgetSlotFills)
                if (F.Host == W.Name) Fills.push_back({ F.Slot, Exp(WidgetRow(F.Content)) });
            FExport E;
            E.ClassIndex = W.Class;
            E.TemplateIndex = W.Cdo;
            E.OuterIndex = Exp(RowTree);
            E.ObjectName = W.Name;
            E.ObjectFlags = RF_Transactional;
            for (const FIndex& S : Slots) E.CreateBeforeSer.push_back(S.V);
            for (const auto& F : Fills) E.CreateBeforeSer.push_back(F.second.V);      // as Screen_CharacterInfo's TopLeftCurve
            if (Own.V != 0) E.CreateBeforeSer.push_back(Own.V);
            for (const FPropertyDef& D : W.Defaults) TagRefs(D, E.CreateBeforeSer);
            E.SerBeforeCreate = { W.Class.V, W.Cdo.V };
            E.CreateBeforeCreate = { Exp(RowTree).V };
            /* Its own class's properties first, then UUserWidget's NamedSlotBindings (measured on Screen_CharacterInfo,
               each FNamedSlotBinding whole), UPanelWidget's Slots and UWidget's Slot, the cook's order. */
            E.Serialize = [=, Defaults = W.Defaults](FArc& Ar) {
                for (const FPropertyDef& D : Defaults) WriteDefaultTag(Ar, D);
                if (!Fills.empty())
                    Tag(Ar, "NamedSlotBindings", "ArrayProperty", [&](FArc& V) {
                        FArc Elements(V.Owner());
                        for (const auto& F : Fills)
                        {
                            Tag(Elements, "Name", "NameProperty", [&](FArc& N) { N.Name(F.first); });
                            Tag(Elements, "Content", "ObjectProperty", [&](FArc& N) { N.Idx(F.second); });
                            TagEnd(Elements);
                        }
                        StructArray(V, "NamedSlotBindings", "NamedSlotBinding", int32(Fills.size()), Elements);
                    }, "StructProperty");
                if (!Slots.empty())
                    Tag(Ar, "Slots", "ArrayProperty", [=](FArc& V) {
                        V.I32(int32(Slots.size()));
                        for (const FIndex& S : Slots) V.Idx(S);
                    }, "ObjectProperty");
                if (Own.V != 0) Tag(Ar, "Slot", "ObjectProperty", [=](FArc& V) { V.Idx(Own); });
                TagEnd(Ar);
                Ar.Bool(false);
            };
            P.AddExport(std::move(E));
        }
        for (size_t I = 0; I < WidgetSlots.size(); ++I)
        {
            const FWidgetSlot& S = WidgetSlots[I];
            const FIndex Panel = Exp(WidgetRow(S.Panel)), Content = Exp(WidgetRow(S.Child));
            FExport E;
            E.ClassIndex = S.Class;
            E.TemplateIndex = S.Cdo;
            E.OuterIndex = Panel;
            E.ObjectName = SlotNames[I];
            E.ObjectFlags = RF_Transactional;
            E.CreateBeforeSer = { Content.V };
            for (const FPropertyDef& D : S.Defaults) TagRefs(D, E.CreateBeforeSer);
            E.SerBeforeCreate = { S.Class.V, S.Cdo.V };
            E.CreateBeforeCreate = { Panel.V };
            E.Serialize = [=, Defaults = S.Defaults](FArc& Ar) {
                for (const FPropertyDef& D : Defaults) WriteDefaultTag(Ar, D);
                Tag(Ar, "Parent", "ObjectProperty", [=](FArc& V) { V.Idx(Panel); });
                Tag(Ar, "Content", "ObjectProperty", [=](FArc& V) { V.Idx(Content); });
                TagEnd(Ar);
                Ar.Bool(false);
            };
            P.AddExport(std::move(E));
        }
    }

    /* Each widget animation, as the editor cooked BasicCheckbox's AnimateXHover (ElytrasModManager): the class's
       <Name>_INST WidgetAnimation, its MovieScene <Name>, a MovieScene2DTransformTrack per widget it keys and that track's
       one MovieScene2DTransformSection, all RF_Transactional. The MovieScene possesses each widget by a GUID that
       AnimationBindings names with the widget's name, which UWidgetAnimation::LocateBoundObjects looks up in the user
       widget's tree. No CompiledData is written: with none, the first play compiles the sequence (a compiled-data entry
       with no signature is volatile, MovieSceneCompiledVolatilityManager.cpp 35-50), and a track rebuilds its evaluation
       field whenever its EvaluationFieldGuid is not its Signature (MovieSceneTrack.cpp 369-380). So every signed object
       gets a signature, seeded by the class, the animation and its keys: a zero one would match the zero a fresh entry
       holds, and nothing would ever compile. Channels a track does not key are left out: one evaluates only when it is
       in the section's TransformMask and has data (MovieScene2DTransformSection.cpp 237-245). */
    std::map<std::string, FIndex> AnimTemplates;    // one import of each class's default object, for every animation
    for (size_t AI = 0; AI < Animations.size(); ++AI)
    {
        const FAnimation A = Animations[AI];
        const int32 Row = AnimationRows[AI];
        const FIndex Inst = Exp(Row), Scene = Exp(Row + 1);
        std::string Seed = A.Name + ":" + std::to_string(A.EndFrame);
        for (const FAnimTrack& T : A.Tracks)
        {
            Seed += "|" + T.Widget;
            for (int32 C = 0; C < 7; ++C)
                for (const FAnimKey& K : T.Channels[C])
                    Seed += ":" + std::to_string(C) + "," + std::to_string(K.Frame) + "," + std::to_string(K.Value) + ","
                          + std::to_string(K.Arrive) + "," + std::to_string(K.Leave) + "," + std::to_string(K.Interp);
        }
        auto Guid = [Cls = ClassName, Seed](const std::string& Of, uint32 (&Out)[4]) { ScsNodeGuid(Cls, "animation." + Of + "." + Seed, Out); };
        auto Range = [](FArc& V, int32 End) {          // TRange<FFrameNumber>: [0, End), each bound its type then its value
            V.U8(1); V.I32(0);                          // ERangeBoundTypes::Inclusive
            V.U8(0); V.I32(End);                        // ERangeBoundTypes::Exclusive
        };
        auto Obj = [&](const char* Package, const char* Of, FIndex Outer, const std::string& Name) {
            FExport E;
            E.ClassIndex = EngineClass(Package, Of);
            FIndex& Template = AnimTemplates[Of];
            if (!Template.V) Template = Imp(P.AddImport({ Package, Of, PackageImport(Package), std::string("Default__") + Of }));
            E.TemplateIndex = Template;
            E.OuterIndex = Outer;
            E.ObjectName = Name;
            E.ObjectFlags = RF_Transactional;
            E.SerBeforeCreate = { E.ClassIndex.V, E.TemplateIndex.V };
            E.CreateBeforeCreate = { Outer.V };
            return E;
        };
        std::vector<std::array<uint32, 4>> Possessed;
        for (const FAnimTrack& T : A.Tracks)
        {
            uint32 G[4];
            Guid("possessable." + T.Widget, G);
            Possessed.push_back({ G[0], G[1], G[2], G[3] });
        }

        FExport Animation = Obj("/Script/UMG", "WidgetAnimation", Exp(RowClass), A.Name + "_INST");
        Animation.CreateBeforeSer = { Scene.V };
        Animation.Serialize = [=](FArc& Ar) {
            Tag(Ar, "MovieScene", "ObjectProperty", [=](FArc& V) { V.Idx(Scene); });
            Tag(Ar, "AnimationBindings", "ArrayProperty", [&](FArc& V) {
                FArc Elements(V.Owner());
                for (size_t T = 0; T < A.Tracks.size(); ++T)
                {
                    const uint32 G[4] = { Possessed[T][0], Possessed[T][1], Possessed[T][2], Possessed[T][3] };
                    Tag(Elements, "WidgetName", "NameProperty", [&](FArc& E) { E.Name(A.Tracks[T].Widget); });
                    Tag(Elements, "SlotWidgetName", "NameProperty", [&](FArc& E) { E.Name("None"); });
                    Tag(Elements, "AnimationGuid", "StructProperty", [&](FArc& E) { E.Guid(G); }, "Guid");
                    TagBool(Elements, "bIsRootWidget", false);
                    TagEnd(Elements);
                }
                StructArray(V, "AnimationBindings", "WidgetAnimationBinding", int32(A.Tracks.size()), Elements);
            }, "StructProperty");
            uint32 Signature[4];
            Guid("sequence", Signature);
            Tag(Ar, "Signature", "StructProperty", [&](FArc& V) { V.Guid(Signature); }, "Guid");
            TagEnd(Ar);
            Ar.Bool(false);
        };
        P.AddExport(std::move(Animation));

        FExport MovieScene = Obj("/Script/MovieScene", "MovieScene", Inst, A.Name);
        for (size_t T = 0; T < A.Tracks.size(); ++T) MovieScene.CreateBeforeSer.push_back(Exp(Row + 2 + 2 * int32(T)).V);
        for (const FAnimTrack& T : A.Tracks) MovieScene.CreateBeforeSer.push_back(T.WidgetClass.V);
        MovieScene.Serialize = [=](FArc& Ar) {
            Tag(Ar, "Possessables", "ArrayProperty", [&](FArc& V) {
                FArc Elements(V.Owner());
                for (size_t T = 0; T < A.Tracks.size(); ++T)
                {
                    const uint32 G[4] = { Possessed[T][0], Possessed[T][1], Possessed[T][2], Possessed[T][3] };
                    const uint32 Zero[4] = { 0, 0, 0, 0 };
                    Tag(Elements, "Tags", "ArrayProperty", [](FArc& E) { E.I32(0); }, "NameProperty");
                    Tag(Elements, "Guid", "StructProperty", [&](FArc& E) { E.Guid(G); }, "Guid");
                    Tag(Elements, "Name", "StrProperty", [&](FArc& E) { E.Str(A.Tracks[T].Widget); });
                    Tag(Elements, "PossessedObjectClass", "ObjectProperty", [&](FArc& E) { E.Idx(A.Tracks[T].WidgetClass); });
                    Tag(Elements, "ParentGuid", "StructProperty", [&](FArc& E) { E.Guid(Zero); }, "Guid");
                    TagEnd(Elements);
                }
                StructArray(V, "Possessables", "MovieScenePossessable", int32(A.Tracks.size()), Elements);
            }, "StructProperty");
            Tag(Ar, "ObjectBindings", "ArrayProperty", [&](FArc& V) {
                FArc Elements(V.Owner());
                for (size_t T = 0; T < A.Tracks.size(); ++T)
                {
                    const uint32 G[4] = { Possessed[T][0], Possessed[T][1], Possessed[T][2], Possessed[T][3] };
                    Tag(Elements, "ObjectGuid", "StructProperty", [&](FArc& E) { E.Guid(G); }, "Guid");
                    Tag(Elements, "BindingName", "StrProperty", [&](FArc& E) { E.Str(A.Tracks[T].Widget); });
                    Tag(Elements, "Tracks", "ArrayProperty", [&](FArc& E) { E.I32(1); E.Idx(Exp(Row + 2 + 2 * int32(T))); }, "ObjectProperty");
                    TagEnd(Elements);
                }
                StructArray(V, "ObjectBindings", "MovieSceneBinding", int32(A.Tracks.size()), Elements);
            }, "StructProperty");
            Tag(Ar, "PlaybackRange", "StructProperty", [&](FArc& V) { Range(V, A.EndFrame); }, "MovieSceneFrameRange");
            uint32 Signature[4];
            Guid("moviescene", Signature);
            Tag(Ar, "Signature", "StructProperty", [&](FArc& V) { V.Guid(Signature); }, "Guid");
            TagEnd(Ar);
            Ar.Bool(false);
        };
        P.AddExport(std::move(MovieScene));

        for (size_t T = 0; T < A.Tracks.size(); ++T)
        {
            const FIndex TrackRow = Exp(Row + 2 + 2 * int32(T)), SectionRow = Exp(Row + 3 + 2 * int32(T));
            FExport Track = Obj("/Script/UMG", "MovieScene2DTransformTrack", Scene, "MovieScene2DTransformTrack_" + std::to_string(T));
            Track.CreateBeforeSer = { SectionRow.V };
            Track.Serialize = [=](FArc& Ar) {
                Tag(Ar, "PropertyBinding", "StructProperty", [](FArc& V) {
                    Tag(V, "PropertyName", "NameProperty", [](FArc& E) { E.Name("RenderTransform"); });
                    Tag(V, "PropertyPath", "NameProperty", [](FArc& E) { E.Name("RenderTransform"); });
                    TagBool(V, "bCanUseClassLookup", true);
                    TagEnd(V);
                }, "MovieScenePropertyBinding");
                Tag(Ar, "Sections", "ArrayProperty", [=](FArc& V) { V.I32(1); V.Idx(SectionRow); }, "ObjectProperty");
                uint32 Signature[4];
                Guid("track." + A.Tracks[T].Widget, Signature);
                Tag(Ar, "Signature", "StructProperty", [&](FArc& V) { V.Guid(Signature); }, "Guid");
                TagEnd(Ar);
                Ar.Bool(false);
            };
            P.AddExport(std::move(Track));

            FExport Section = Obj("/Script/UMG", "MovieScene2DTransformSection", TrackRow, "MovieScene2DTransformSection_0");
            Section.Serialize = [=](FArc& Ar) {
                /* FMovieSceneFloatChannel::Serialize (MovieSceneFloatChannel.cpp 1085-1166): the extrapolations, the times
                   and the 28-byte FMovieSceneFloatValues as sized arrays, the default, the tick resolution. */
                static const struct { const char* Name; int32 Index; } Channels[7] = {
                    { "Translation", 0 }, { "Translation", 1 }, { "Rotation", 0 }, { "Scale", 0 }, { "Scale", 1 },
                    { "Shear", 0 }, { "Shear", 1 } };
                for (int32 C = 0; C < 7; ++C)
                {
                    const std::vector<FAnimKey>& Keys = A.Tracks[T].Channels[C];
                    if (Keys.empty()) continue;
                    Tag(Ar, Channels[C].Name, "StructProperty", [&](FArc& V) {
                        V.U8(4); V.U8(4);                           // Pre/PostInfinityExtrap: RCCE_Constant
                        V.I32(4); V.I32(int32(Keys.size()));
                        for (const FAnimKey& K : Keys) V.I32(K.Frame);
                        V.I32(28); V.I32(int32(Keys.size()));
                        for (const FAnimKey& K : Keys)
                        {
                            const float Fields[5] = { K.Value, K.Arrive, K.Leave, 0.0f, 0.0f };   // the two weights unused
                            V.Raw(Fields, sizeof(Fields));
                            V.U8(0); V.U8(0); V.U8(0); V.U8(0);     // TangentWeightMode RCTWM_WeightedNone, padding
                            V.U8(K.Interp); V.U8(K.TangentMode); V.U8(0); V.U8(0);
                        }
                        V.Raw("\0\0\0\0", 4);                       // DefaultValue
                        V.Bool(false);                              // bHasDefaultValue
                        V.I32(60000); V.I32(1);                     // TickResolution
                    }, "MovieSceneFloatChannel", Channels[C].Index);
                }
                Tag(Ar, "SectionRange", "StructProperty", [&](FArc& V) { Range(V, A.EndFrame); }, "MovieSceneFrameRange");
                uint32 Signature[4];
                Guid("section." + A.Tracks[T].Widget, Signature);
                Tag(Ar, "Signature", "StructProperty", [&](FArc& V) { V.Guid(Signature); }, "Guid");
                TagEnd(Ar);
                Ar.Bool(false);
            };
            P.AddExport(std::move(Section));
        }
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
        if (C.bGuid) std::copy(std::begin(C.Guid), std::end(C.Guid), NodeGuid);
        else ScsNodeGuid(ClassName, C.Name, NodeGuid);
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
        /* Its class serialized first, as an SCS template's is, and the class that owns the overridden component created:
           the cooker's order (EEE's BP_ElectricCloud). Without the first, the loader may serialize the template while
           its class is only created; the template's ConditionalPostLoadSubobjects then post-loads its outer, the class,
           with no SuperStruct yet: "Can't find ClassConstructor" (Obj.cpp ConditionalPostLoadSubobjects, Class.cpp
           UClass::Bind). A late pak mount loads the class first and never shows it; startup force-loading does. */
        Template.SerBeforeSer = { Exp(RowClass).V };
        Template.CreateBeforeSer = { O.OwnerClass.V };
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
    Api.Docs = ApiDocs;
    Api.bInterface = (ClassFlags & CLASS_Interface) != 0;
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

/* Measured on DRG's Hologram_FadeIn: one export of the curve class, flags Public | Standalone | Transactional, its
   preload edges serBeforeCreate {class, CDO}, the body a track's curve has. */
void FBlueprintClass::FinishCurve(FIndex Class, FIndex ClassCdo, const FTimelineTrack& Curve)
{
    ClassRow = 0;
    FExport A;
    A.ClassIndex = Class;
    A.TemplateIndex = ClassCdo;
    A.ObjectName = ClassName;
    A.ObjectFlags = RF_Public | RF_Standalone | RF_Transactional;
    A.bIsAsset = true;
    A.SerBeforeCreate = { Class.V, ClassCdo.V };
    A.Serialize = [Curve](FArc& Ar) { CurveBody(Ar, Curve); };
    P.AddExport(std::move(A));
}

/* Measured on DRG's ENU_TextCase: the tags (the editor's DisplayNameMap), no GUID, then UEnum::Names as
   (FName, int64) pairs ending in the _MAX entry, then CppForm 1 (Namespaced). Nothing outside WITH_EDITOR
   reads the names' shape, so they are the C++ ones rather than the editor's NewEnumeratorN.
   No DisplayNameMap unless an enumerator is cooked under another name (`__UeEnumNames`): UEnum::GetDisplayNameTextByIndex
   then falls back to the enumerator's own name. With one, each renamed enumerator's display name is its C++ name, the
   map's text as the editor writes it (no flags, a Base history: empty namespace, a 32-hex key, the string). */
void FBlueprintClass::FinishEnum(const std::vector<std::pair<std::string, int64>>& Enumerators,
                                 const std::vector<std::pair<std::string, std::string>>& DisplayNames)
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
    const std::string Seed = P.Name() + "." + ClassName;
    E.Serialize = [=](FArc& Ar) {
        if (!DisplayNames.empty())
            Tag(Ar, "DisplayNameMap", "MapProperty", [&](FArc& M) {
                M.I32(0);                                   // keys to remove
                M.I32(int32(DisplayNames.size()));
                for (const auto& [Key, Shown] : DisplayNames)
                {
                    M.Name(Key);
                    M.U32(0);                               // flags
                    M.U8(0);                                // ETextHistoryType::Base
                    M.Str("");                              // namespace
                    char Hex[33];
                    const uint32 H = StrCrc32(Seed + "::" + Key);
                    snprintf(Hex, sizeof(Hex), "%08X%08X%08X%08X", H, ~H, H * 2654435761u, H ^ 0x9E3779B9u);
                    M.Str(Hex);                             // key
                    M.Str(Shown);                           // source string
                }
            }, "NameProperty,TextProperty");
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
