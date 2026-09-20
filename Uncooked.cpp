// Layout transcribed from UE4.27 EdGraphPin.cpp (UEdGraphPin::Serialize / SerializePinArray) and
// K2Node_EditablePinBase.cpp, and checked against an editor-saved 4.27.2 Blueprint byte for byte.
#include "Uncooked.h"

#include <cstdio>
#include <map>
#include <memory>

namespace Uasset
{
namespace
{
/* A pin's type, the FEdGraphPinType subset a signature needs. */
struct FPinType
{
    std::string Category, SubCategory;
    FIndex SubCategoryObject;
    uint8 Container = 0;                // EPinContainerType: 0 none, 1 array, 2 set, 3 map
    bool bIsReference = false;
    bool bIsConst = false;
    std::shared_ptr<FPinType> ValueType;    // map only
};

struct FPinDef
{
    std::string Name;
    uint8 Direction = 0;                // EEdGraphPinDirection: 0 input, 1 output
    FPinType Type;
    uint32 Guid[4] = { 0, 0, 0, 0 };
};

/* Stable across rebuilds, and valid (the engine checks a pin GUID is non-zero). */
void MakeGuid(const std::string& Seed, uint32 (&Out)[4])
{
    Out[0] = StrCrc32(Seed) | 1u;
    Out[1] = Strihash(Seed);
    Out[2] = StrCrc32(Seed + "\x01");
    Out[3] = Strihash(Seed + "\x02") | 1u;
}

/* FArc::Str writes a one-byte empty string; an FString the engine saved empty is a bare zero length. */
void EmptyStr(FArc& Ar) { Ar.I32(0); }

/* An empty FText: flags, then ETextHistoryType::None, then "no culture-invariant string". */
void EmptyText(FArc& Ar)
{
    Ar.U32(0);
    Ar.U8(0xFF);
    Ar.Bool(false);
}

void WritePinType(FArc& Ar, const FPinType& T)
{
    Ar.Name(T.Category);
    Ar.Name(T.SubCategory.empty() ? "None" : T.SubCategory);
    Ar.Idx(T.SubCategoryObject);
    Ar.U8(T.Container);
    if (T.Container == 3 && T.ValueType)
    {
        // FEdGraphTerminalType: the map's value half.
        Ar.Name(T.ValueType->Category);
        Ar.Name(T.ValueType->SubCategory.empty() ? "None" : T.ValueType->SubCategory);
        Ar.Idx(T.ValueType->SubCategoryObject);
        Ar.Bool(T.ValueType->bIsConst);
        Ar.Bool(false);                 // bTerminalIsWeakPointer
        Ar.Bool(false);                 // bTerminalIsUObjectWrapper (FReleaseObjectVersion >= 32)
    }
    Ar.Bool(T.bIsReference);
    Ar.Bool(false);                     // bIsWeakPointer
    Ar.Idx(Null());                     // PinSubCategoryMemberReference: MemberParent
    Ar.Name("None");                    //                                MemberName
    for (int32 I = 0; I < 4; ++I) Ar.U32(0);   //                         MemberGuid
    Ar.Bool(T.bIsConst);
    Ar.Bool(false);                     // bIsUObjectWrapper
}

/*
A variable's literal initializer as the editor stores it: FBPVariableDescription::DefaultValue is
the property's ExportText form, so the details panel shows `int X = 5` as 5 rather than 0. Only the
scalar kinds are representable - a struct or container default lives on the CDO, which the editor
side has none of, and an empty string there means "the type's zero".
*/
std::string DefaultString(const FPropertyDef& P)
{
    const FDefaultValue& D = P.Default;
    if (D.K == FDefaultValue::None) return std::string();
    switch (D.K)
    {
    case FDefaultValue::Bool:
        return D.I ? "True" : "False";
    case FDefaultValue::Int:
        /* A byte backed by an enum reads as its enumerator; only the zero one is known here. */
        if (P.Type == "ByteProperty" && !P.EnumZero.empty())
            return D.I == 0 ? P.EnumZero : std::string();
        return std::to_string(D.I);
    case FDefaultValue::Float:
    {
        /* FString::SanitizeFloat: shortest round-tripping form, at least one fractional digit. */
        char Buffer[64];
        snprintf(Buffer, sizeof(Buffer), "%.17g", D.F);
        std::string Out(Buffer);
        if (Out.find_first_of(".eE") == std::string::npos) Out += ".0";
        return Out;
    }
    case FDefaultValue::Str:
        return D.S;
    default:
        return std::string();       // object references and containers: left to the cooked CDO
    }
}

/* ELifetimeCondition, by ordinal: what Cpp.cpp's UE_REPLICATED parses back into a name. */
const char* LifetimeCondition(uint8 Value)
{
    static const char* const Names[] = {
        "COND_None", "COND_InitialOnly", "COND_OwnerOnly", "COND_SkipOwner", "COND_SimulatedOnly",
        "COND_AutonomousOnly", "COND_SimulatedOrPhysics", "COND_InitialOrOwner", "COND_Custom",
        "COND_ReplayOrOwner", "COND_ReplayOnly", "COND_SimulatedOnlyNoReplay",
        "COND_SimulatedOrPhysicsNoReplay", "COND_SkipReplay", "COND_Never" };
    return Value < sizeof(Names) / sizeof(Names[0]) ? Names[Value] : "COND_None";
}

/* One entry of a node's own Pins array: the SerializePin header, then the pin itself. */
void WritePin(FArc& Ar, int32 NodeExport, const FPinDef& Pin)
{
    Ar.Bool(false);                     // bNullPtr
    Ar.Idx(Exp(NodeExport));
    Ar.Raw(Pin.Guid, 16);

    Ar.Idx(Exp(NodeExport));            // UEdGraphPin::Serialize repeats both
    Ar.Raw(Pin.Guid, 16);
    Ar.Name(Pin.Name);
    EmptyText(Ar);                      // PinFriendlyName
    EmptyStr(Ar);                       // PinToolTip
    Ar.U8(Pin.Direction);
    WritePinType(Ar, Pin.Type);
    EmptyStr(Ar);                       // DefaultValue
    EmptyStr(Ar);                       // AutogeneratedDefaultValue
    Ar.Idx(Null());                     // DefaultObject
    EmptyText(Ar);                      // DefaultTextValue
    Ar.I32(0);                          // LinkedTo
    Ar.I32(0);                          // SubPins
    Ar.Bool(true);                      // ParentPin: null
    Ar.Bool(true);                      // ReferencePassThroughConnection: null
    for (int32 I = 0; I < 4; ++I) Ar.U32(0);   // PersistentGuid
    Ar.U32(0);                          // bHidden / bNotConnectable / ... bitfield
}

/* The tail every K2 editable node shares: its pins, then the user-defined pins the compiler
   turns back into parameters. */
void WriteNodeTail(FArc& Ar, int32 NodeExport, const std::vector<FPinDef>& Pins,
                   const std::vector<const FPinDef*>& UserPins)
{
    Ar.I32(int32(Pins.size()));
    for (const FPinDef& Pin : Pins) WritePin(Ar, NodeExport, Pin);

    Ar.I32(int32(UserPins.size()));
    for (const FPinDef* Pin : UserPins)
    {
        Ar.Name(Pin->Name);
        WritePinType(Ar, Pin->Type);
        Ar.U8(Pin->Direction);
        EmptyStr(Ar);                   // PinDefaultValue
    }
}

/* ---- the writer ---- */

class FApiWriter
{
public:
    FApiWriter(const FApiClass& InClass, const FPackage& InSource)
        : Class(InClass), Source(InSource), P(InClass.PackageName + "/" + InClass.AssetName)
    {
    }

    bool Write(const std::string& OutDir, std::string* Err);

private:
    FIndex PackageImport(const std::string& Name);
    FIndex Object(const std::string& Package, const std::string& ClassName, const std::string& ObjectName);
    /* The same object as Source names it, re-imported here. False when its outer is not a plain
       package - a type that lives inside the mod's own package has no editor-side asset yet. */
    bool Remap(FIndex Src, FIndex& Out);
    bool PinTypeOf(const FPropertyDef& Prop, FPinType& Out);

    const FApiClass& Class;
    const FPackage& Source;
    FPackage P;
    std::map<std::string, int32> ImportCache;
};

FIndex FApiWriter::PackageImport(const std::string& Name)
{
    const std::string Key = "pkg|" + Name;
    auto It = ImportCache.find(Key);
    if (It != ImportCache.end()) return Imp(It->second);
    const int32 Row = P.AddImport({ "/Script/CoreUObject", "Package", Null(), Name });
    ImportCache.emplace(Key, Row);
    return Imp(Row);
}

FIndex FApiWriter::Object(const std::string& Package, const std::string& ClassName,
                          const std::string& ObjectName)
{
    const std::string Key = Package + "|" + ClassName + "|" + ObjectName;
    auto It = ImportCache.find(Key);
    if (It != ImportCache.end()) return Imp(It->second);
    const FIndex Outer = PackageImport(Package);
    const int32 Row = P.AddImport({ "/Script/CoreUObject", ClassName, Outer, ObjectName });
    ImportCache.emplace(Key, Row);
    return Imp(Row);
}

bool FApiWriter::Remap(FIndex Src, FIndex& Out)
{
    if (Src.V == 0) { Out = Null(); return true; }
    const FImport* In = Source.ImportAt(Src);
    if (!In) return false;
    const FImport* Outer = Source.ImportAt(In->Outer);
    if (!Outer || Outer->ClassName != "Package") return false;
    /* A /Game type is another cooked asset of this mod - a UE_STRUCT or UE_ENUM. Those have no
       uncooked counterpart yet, and an import of one would resolve to nothing in the editor. */
    if (Outer->ObjectName.size() >= 6 && Outer->ObjectName.compare(0, 6, "/Game/") == 0) return false;
    Out = Object(Outer->ObjectName, In->ClassName, In->ObjectName);
    return true;
}

bool FApiWriter::PinTypeOf(const FPropertyDef& Prop, FPinType& Out)
{
    const std::string& T = Prop.Type;
    if (T == "BoolProperty") Out.Category = "bool";
    else if (T == "IntProperty") Out.Category = "int";
    else if (T == "Int64Property") Out.Category = "int64";
    else if (T == "FloatProperty") Out.Category = "float";
    else if (T == "NameProperty") Out.Category = "name";
    else if (T == "StrProperty") Out.Category = "string";
    else if (T == "TextProperty") Out.Category = "text";
    else if (T == "ByteProperty" || T == "EnumProperty")
    {
        // A Blueprint enum pin is a byte whose subcategory object is the UEnum.
        Out.Category = "byte";
        if (!Remap(Prop.Extra, Out.SubCategoryObject)) return false;
    }
    else if (T == "ObjectProperty" || T == "SoftObjectProperty" || T == "InterfaceProperty"
             || T == "StructProperty")
    {
        Out.Category = T == "StructProperty" ? "struct"
                     : T == "InterfaceProperty" ? "interface"
                     : T == "SoftObjectProperty" ? "softobject" : "object";
        if (!Remap(Prop.Extra, Out.SubCategoryObject)) return false;
    }
    else if (T == "ClassProperty" || T == "SoftClassProperty")
    {
        // The pin carries the subclass filter, not UClass itself.
        Out.Category = T == "ClassProperty" ? "class" : "softclass";
        if (!Remap(Prop.Extra2, Out.SubCategoryObject)) return false;
    }
    else if (T == "ArrayProperty" || T == "SetProperty" || T == "MapProperty")
    {
        if (!Prop.Inner || !PinTypeOf(*Prop.Inner, Out)) return false;
        Out.Container = T == "ArrayProperty" ? 1 : T == "SetProperty" ? 2 : 3;
        if (Out.Container == 3)
        {
            auto Value = std::make_shared<FPinType>();
            if (!Prop.Value || !PinTypeOf(*Prop.Value, *Value)) return false;
            Out.ValueType = Value;
        }
    }
    else return false;

    // `T&` in the source is CPF_OutParm | CPF_ReferenceParm, which in a graph is an input pin
    // passed by reference - not an entry on the result node.
    Out.bIsReference = (Prop.PropertyFlags & CPF_ReferenceParm) != 0;
    Out.bIsConst = (Prop.PropertyFlags & CPF_ConstParm) != 0;
    return true;
}

bool FApiWriter::Write(const std::string& OutDir, std::string* Err)
{
    const std::string ClassName = Class.AssetName + "_C";
    const bool bFunctionLibrary = Class.ParentClass == "BlueprintFunctionLibrary";

    /*
    An actor Blueprint is the one shape this cannot emit yet. UBlueprint::PostLoad gives every
    actor a SimpleConstructionScript and asserts on a null GeneratedClass to outer it to, and a
    hand-written empty generated class does not survive the editor's deferred CDO loading (its CDO
    export never gets created, so the load ends in "CDO for class X did not load"). Emitting one
    that does means emitting the whole compiled class, which is the cooked writer's job - see the
    TODO in ROADMAP. Until then an actor mod ships without an editor-side stub rather than with
    one that crashes the editor that opens it.
    */
    if (Class.bIsActor)
    {
        if (Err) *Err = Class.AssetName + " is actor-derived: an API asset needs a generated class the editor accepts (TODO)";
        return false;
    }

    const FIndex ImpBlueprint = Object("/Script/Engine", "Class", "Blueprint");
    const FIndex ImpEdGraph = Object("/Script/Engine", "Class", "EdGraph");
    const FIndex ImpSchema = Object("/Script/BlueprintGraph", "Class", "EdGraphSchema_K2");
    const FIndex ImpEntry = Object("/Script/BlueprintGraph", "Class", "K2Node_FunctionEntry");
    const FIndex ImpResult = Object("/Script/BlueprintGraph", "Class", "K2Node_FunctionResult");
    const FIndex ImpParent = Object(Class.ParentPackage, "Class", Class.ParentClass);

    struct FGraph
    {
        std::string Name;
        uint32 Flags = 0;
        bool bPure = false;
        std::vector<FPinDef> EntryPins, ResultPins;
    };
    std::vector<FGraph> Graphs;

    for (const FApiFunction& Fn : Class.Functions)
    {
        if (!(Fn.Flags & FUNC_BlueprintCallable)) continue;
        if (Fn.Flags & (FUNC_UbergraphFunction | FUNC_Delegate | FUNC_MulticastDelegate)) continue;

        FGraph G;
        G.Name = Fn.Name;
        G.Flags = Fn.Flags;
        /* A pure function draws without exec pins, so UE_PURE decides the node shape here and
           not just the flag: an entry node with a `then` pin would compile back as impure. */
        G.bPure = (Fn.Flags & FUNC_BlueprintPure) != 0;

        if (!G.bPure)
        {
            FPinDef Then;
            Then.Name = "then";
            Then.Direction = 1;
            Then.Type.Category = "exec";
            MakeGuid(Class.AssetName + "." + Fn.Name + ".then", Then.Guid);
            G.EntryPins.push_back(Then);

            FPinDef Execute;
            Execute.Name = "execute";
            Execute.Direction = 0;
            Execute.Type.Category = "exec";
            MakeGuid(Class.AssetName + "." + Fn.Name + ".execute", Execute.Guid);
            G.ResultPins.push_back(Execute);
        }

        bool bOk = true;
        for (const FPropertyDef& Prop : Fn.Params)
        {
            if (!(Prop.PropertyFlags & CPF_Parm)) continue;
            FPinDef Pin;
            Pin.Name = Prop.Name;
            MakeGuid(Class.AssetName + "." + Fn.Name + "." + Prop.Name, Pin.Guid);
            if (!PinTypeOf(Prop, Pin.Type))
            {
                printf("  %-14s skipped: parameter %s has no editor-side type yet\n",
                       Fn.Name.c_str(), Prop.Name.c_str());
                bOk = false;
                break;
            }
            // An entry node hands the inputs out; a result node takes the outputs in.
            const bool bOutput = (Prop.PropertyFlags & CPF_OutParm) && !Pin.Type.bIsReference;
            Pin.Direction = bOutput ? 0 : 1;
            (bOutput ? G.ResultPins : G.EntryPins).push_back(Pin);
        }
        if (!bOk) continue;

        Graphs.push_back(std::move(G));
    }

    // Export rows, in the order the engine's own saves use: the Blueprint, then each graph with its nodes.
    const int32 ExBlueprint = 0;
    std::vector<int32> GraphExports;
    {
        int32 Next = 1;
        for (const FGraph& G : Graphs)
        {
            GraphExports.push_back(Next);
            Next += G.ResultPins.size() > (G.bPure ? size_t(0) : size_t(1)) ? 3 : 2;
        }
    }

    /* Class variables: the same FEdGraphPinType, plus the flags that decide how the editor treats
       each one - visible/read-only, replicated, its RepNotify and condition. */
    struct FApiVar
    {
        std::string Name;
        FPinType Type;
        uint64 Flags = 0;
        std::string RepNotify;
        std::string Default;
        uint8 RepCondition = 0;
        uint32 Guid[4] = { 0, 0, 0, 0 };
    };
    std::vector<FApiVar> Vars;
    for (const FPropertyDef& Prop : Class.Variables)
    {
        FApiVar Var;
        Var.Name = Prop.Name;
        if (!PinTypeOf(Prop, Var.Type))
        {
            printf("  %-14s skipped: variable has no editor-side type yet\n", Prop.Name.c_str());
            continue;
        }
        /* The editor's own set. CPF_Parm and the compiler-internal bits mean nothing on a variable,
           and a flag the details panel cannot show would only be lost on the next compile anyway. */
        Var.Flags = Prop.PropertyFlags & (CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly
                                          | CPF_DisableEditOnInstance | CPF_DisableEditOnTemplate
                                          | CPF_EditConst | CPF_Net | CPF_RepNotify | CPF_Transient
                                          | CPF_Config | CPF_SaveGame | CPF_Interp | CPF_AdvancedDisplay
                                          | CPF_ExposeOnSpawn | CPF_NonTransactional);
        Var.RepNotify = Prop.RepNotify;
        Var.RepCondition = Prop.RepCondition;
        Var.Default = DefaultString(Prop);
        MakeGuid(Class.AssetName + ".var." + Prop.Name, Var.Guid);
        Vars.push_back(std::move(Var));
    }

    /* A field the mod assigns from its construction script or BeginPlay has no literal default to
       carry: the value is a call, and only a real compiled graph could express it. Say so rather
       than writing a zero that looks deliberate. */
    if (!Vars.empty())
        for (const FApiFunction& Fn : Class.Functions)
            if (Fn.Name == "UserConstructionScript" || Fn.Name == "ReceiveBeginPlay")
            {
                printf("  %-14s TODO: %s may set variables from calls; function-based defaults are not"
                       " in the API asset until it emits real graphs\n", Class.AssetName.c_str(), Fn.Name.c_str());
                break;
            }

    /* A class with neither is not an API surface - most often a pure data holder or a class whose
       whole body is inlined - and an empty Blueprint would only be clutter in someone's project. */
    if (Graphs.empty() && Vars.empty())
    {
        if (Err) *Err = Class.AssetName + " exposes no callable function or variable";
        return false;
    }

    uint32 PkgGuid[4];
    MakeGuid(P.Name(), PkgGuid);
    P.SetUncooked(true);
    P.SetGuid(PkgGuid[0], PkgGuid[1], PkgGuid[2], PkgGuid[3]);

    FExport Blueprint;
    Blueprint.ClassIndex = ImpBlueprint;
    Blueprint.OuterIndex = Null();
    Blueprint.ObjectName = Class.AssetName;
    Blueprint.ObjectFlags = RF_Public | RF_Standalone | RF_Transactional;
    Blueprint.bIsAsset = true;
    const std::vector<int32> GraphRows = GraphExports;
    Blueprint.Serialize = [=](FArc& Ar) {
        Tag(Ar, "ParentClass", "ObjectProperty", [=](FArc& V) { V.Idx(ImpParent); });
        if (bFunctionLibrary)
            Tag(Ar, "BlueprintType", "ByteProperty",
                [](FArc& V) { V.Name("BPTYPE_FunctionLibrary"); }, "EBlueprintType");
        Tag(Ar, "BlueprintSystemVersion", "IntProperty", [](FArc& V) { V.I32(2); });
        if (!GraphRows.empty())
            Tag(Ar, "FunctionGraphs", "ArrayProperty", [=](FArc& V) {
                V.I32(int32(GraphRows.size()));
                for (int32 Row : GraphRows) V.Idx(Exp(Row));
            }, "ObjectProperty");
        if (!Vars.empty())
            Tag(Ar, "NewVariables", "ArrayProperty", [=](FArc& V) {
                /* The elements go into a scratch archive first: an array of structs carries an inner
                   FPropertyTag between the count and the elements, and its Size is their byte count -
                   a zero there sends the loader off the end of the payload. */
                FArc Elements(V.Owner());
                for (const FApiVar& Var : Vars)
                {
                    Tag(Elements, "VarName", "NameProperty", [&](FArc& E) { E.Name(Var.Name); });
                    Tag(Elements, "VarGuid", "StructProperty", [&](FArc& E) { E.Raw(Var.Guid, 16); }, "Guid");
                    Tag(Elements, "VarType", "StructProperty", [&](FArc& E) { WritePinType(E, Var.Type); },
                        "EdGraphPinType");
                    Tag(Elements, "PropertyFlags", "UInt64Property", [&](FArc& E) { E.I64(int64(Var.Flags)); });
                    if (!Var.RepNotify.empty())
                        Tag(Elements, "RepNotifyFunc", "NameProperty", [&](FArc& E) { E.Name(Var.RepNotify); });
                    if (Var.RepCondition)
                        Tag(Elements, "ReplicationCondition", "ByteProperty",
                            [&](FArc& E) { E.Name(LifetimeCondition(Var.RepCondition)); }, "ELifetimeCondition");
                    if (!Var.Default.empty())
                        Tag(Elements, "DefaultValue", "StrProperty", [&](FArc& E) { E.Str(Var.Default); });
                    TagEnd(Elements);
                }

                V.I32(int32(Vars.size()));
                V.Name("NewVariables");
                V.Name("StructProperty");
                V.I32(int32(Elements.B.size()));
                V.I32(0);                       // ArrayIndex
                V.Name("BPVariableDescription");
                for (int32 I = 0; I < 4; ++I) V.U32(0);      // StructGuid
                V.U8(0);                        // HasPropertyGuid
                V.Append(Elements);
            }, "StructProperty");
        TagBool(Ar, "bLegacyNeedToPurgeSkelRefs", false);
        Tag(Ar, "BlueprintGuid", "StructProperty", [=](FArc& V) { V.Raw(PkgGuid, 16); }, "Guid");
        TagEnd(Ar);
        Ar.Bool(false);
    };
    P.AddExport(std::move(Blueprint));


    for (size_t I = 0; I < Graphs.size(); ++I)
    {
        const FGraph& G = Graphs[I];
        const int32 ExGraph = GraphExports[I];
        const int32 ExEntry = ExGraph + 1;
        const size_t FirstUserPin = G.bPure ? 0 : 1;
        const int32 ExResult = G.ResultPins.size() > FirstUserPin ? ExGraph + 2 : 0;

        uint32 GraphGuid[4];
        MakeGuid(Class.AssetName + "." + G.Name + ".graph", GraphGuid);

        FExport Graph;
        Graph.ClassIndex = ImpEdGraph;
        Graph.OuterIndex = Exp(ExBlueprint);
        Graph.ObjectName = G.Name;
        Graph.ObjectFlags = RF_Transactional;
        Graph.Serialize = [=](FArc& Ar) {
            Tag(Ar, "Schema", "ObjectProperty", [=](FArc& V) { V.Idx(ImpSchema); });
            Tag(Ar, "Nodes", "ArrayProperty", [=](FArc& V) {
                V.I32(ExResult ? 2 : 1);
                V.Idx(Exp(ExEntry));
                if (ExResult) V.Idx(Exp(ExResult));
            }, "ObjectProperty");
            Tag(Ar, "GraphGuid", "StructProperty", [=](FArc& V) { V.Raw(GraphGuid, 16); }, "Guid");
            TagEnd(Ar);
            Ar.Bool(false);
        };
        P.AddExport(std::move(Graph));

        // The compiler reads the function's flags off the entry node; the name comes from the graph.
        /* Exactly the set UK2Node_FunctionEntry treats as its own (CleanBlueprintFunctionFlags lists
           the rest as invalid there); the compiler derives Exec/Event/HasOutParms itself. Access,
           constness, purity and the whole RPC group ride along, so a UE_SERVER/UE_RELIABLE function
           is still an RPC on the editor side. */
        const uint32 EntryFlags = G.Flags & (FUNC_Public | FUNC_Protected | FUNC_Private
                                             | FUNC_Static | FUNC_Const
                                             | FUNC_BlueprintPure | FUNC_BlueprintCallable
                                             | FUNC_BlueprintEvent | FUNC_BlueprintAuthorityOnly
                                             | FUNC_Net | FUNC_NetReliable | FUNC_NetMulticast
                                             | FUNC_NetServer | FUNC_NetClient);
        uint32 EntryGuid[4];
        MakeGuid(Class.AssetName + "." + G.Name + ".entry", EntryGuid);
        const std::vector<FPinDef> EntryPins = G.EntryPins;

        FExport Entry;
        Entry.ClassIndex = ImpEntry;
        Entry.OuterIndex = Exp(ExGraph);
        Entry.ObjectName = "K2Node_FunctionEntry_0";
        Entry.ObjectFlags = RF_Transactional;
        const std::string FnName = G.Name;
        Entry.Serialize = [=](FArc& Ar) {
            /* Not decoration: ConformFunctionNames renames a graph to its entry node's member name
               on load, so an entry without one turns every function into "None". */
            Tag(Ar, "FunctionReference", "StructProperty", [=](FArc& V) {
                Tag(V, "MemberName", "NameProperty", [=](FArc& N) { N.Name(FnName); });
                TagEnd(V);
            }, "MemberReference");
            Tag(Ar, "ExtraFlags", "IntProperty", [=](FArc& V) { V.I32(int32(EntryFlags)); });
            Tag(Ar, "NodePosX", "IntProperty", [](FArc& V) { V.I32(0); });
            Tag(Ar, "NodeGuid", "StructProperty", [=](FArc& V) { V.Raw(EntryGuid, 16); }, "Guid");
            TagEnd(Ar);
            Ar.Bool(false);
            std::vector<const FPinDef*> User;
            for (size_t Pin = FirstUserPin; Pin < EntryPins.size(); ++Pin) User.push_back(&EntryPins[Pin]);
            WriteNodeTail(Ar, ExEntry, EntryPins, User);
        };
        P.AddExport(std::move(Entry));

        if (!ExResult) continue;

        uint32 ResultGuid[4];
        MakeGuid(Class.AssetName + "." + G.Name + ".result", ResultGuid);
        const std::vector<FPinDef> ResultPins = G.ResultPins;

        FExport Result;
        Result.ClassIndex = ImpResult;
        Result.OuterIndex = Exp(ExGraph);
        Result.ObjectName = "K2Node_FunctionResult_0";
        Result.ObjectFlags = RF_Transactional;
        Result.Serialize = [=](FArc& Ar) {
            Tag(Ar, "FunctionReference", "StructProperty", [=](FArc& V) {
                Tag(V, "MemberName", "NameProperty", [=](FArc& N) { N.Name(FnName); });
                TagEnd(V);
            }, "MemberReference");
            Tag(Ar, "NodePosX", "IntProperty", [](FArc& V) { V.I32(400); });
            Tag(Ar, "NodeGuid", "StructProperty", [=](FArc& V) { V.Raw(ResultGuid, 16); }, "Guid");
            TagEnd(Ar);
            Ar.Bool(false);
            std::vector<const FPinDef*> User;
            for (size_t Pin = FirstUserPin; Pin < ResultPins.size(); ++Pin) User.push_back(&ResultPins[Pin]);
            WriteNodeTail(Ar, ExResult, ResultPins, User);
        };
        P.AddExport(std::move(Result));
    }

    /* Without registry rows the content browser sees a package with nothing in it. */
    FRegistryObject Row;
    Row.ObjectPath = Class.AssetName;
    Row.ClassName = "Blueprint";
    const std::string ParentPath = "Class'" + Class.ParentPackage + "." + Class.ParentClass + "'";
    Row.Tags = {
        { "BlueprintType", bFunctionLibrary ? "BPTYPE_FunctionLibrary" : "BPTYPE_Normal" },
        { "ParentClass", ParentPath },
        { "NativeParentClass", ParentPath },
        { "GeneratedClass", "BlueprintGeneratedClass'" + P.Name() + "." + ClassName + "'" },
        { "NumReplicatedProperties", "0" },
        { "IsDataOnly", "False" },
    };
    P.SetRegistryObjects({ Row });

    return P.Save(OutDir + "/" + Class.AssetName, Err);
}
}   // namespace

bool WriteApiAsset(const FApiClass& Class, const FPackage& Source, const std::string& OutDir,
                   std::string* Err)
{
    FApiWriter Writer(Class, Source);
    return Writer.Write(OutDir, Err);
}

}   // namespace Uasset
