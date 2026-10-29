// Layout transcribed from UE4.27 EdGraphPin.cpp (UEdGraphPin::Serialize / SerializePinArray) and
// K2Node_EditablePinBase.cpp, and checked against an editor-saved 4.27.2 Blueprint byte for byte.
#include "Uncooked.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>

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
    /* A delegate pin's signature function: the class it lives in and its name (PinSubCategoryMemberReference). */
    FIndex MemberParent;
    std::string MemberName;
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

/* A culture-invariant FText, as Script.cpp writes a text default: the flag, no history, "has a string", the string. */
void WriteText(FArc& Ar, const std::string& S)
{
    Ar.U32(2);          // ETextFlag::CultureInvariant
    Ar.U8(0xFF);
    Ar.Bool(true);
    Ar.Str(S);
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
    Ar.Idx(T.MemberParent);             // PinSubCategoryMemberReference: MemberParent
    Ar.Name(T.MemberName.empty() ? "None" : T.MemberName);  //            MemberName
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
        "COND_SimulatedOrPhysicsNoReplay", "COND_SkipReplay", "COND_None", "COND_Never" };   // 14 is unused
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

/* Shared editor-side package plumbing: the import table this stub builds, and the type mapping from
   a cooked FPropertyDef (which carries FIndexes into the cooked Source package) to an editor pin. */
class FEditorPackage
{
protected:
    FEditorPackage(const FPackage& InSource, std::string PkgPath)
        : Source(InSource), P(std::move(PkgPath))
    {
    }

    FIndex PackageImport(const std::string& Name);
    FIndex Object(const std::string& Package, const std::string& ClassName, const std::string& ObjectName,
                  const std::string& ClassPackage = "/Script/CoreUObject");
    /* The same object as Source names it, re-imported here. False when its outer is not a plain
       package - a type that lives inside the mod's own package has no editor-side asset yet. */
    bool Remap(FIndex Src, FIndex& Out);
    /* A signature function as a delegate pin names it: its class re-imported (SelfRef for this class's own, one the
       cooked class makes per delegate type) and its name. */
    bool SignatureOf(FIndex Fn, FIndex& Parent, std::string& Name);
    bool PinTypeOf(const FPropertyDef& Prop, FPinType& Out);
    /* A widget default with its object references re-imported here. False drops it: what it names is an export of
       the cooked package or an instanced object. A mod asset (a font, a texture) stays named: an uncooked copy
       shipped beside the stubs draws in the designer (the editor refuses a cooked package). */
    bool RemapDefault(FPropertyDef& D);
    bool RemapValue(FDefaultValue& V);

    const FPackage& Source;
    FPackage P;
    std::map<std::string, int32> ImportCache;
    /* The cooked class itself (a dispatcher's or function's `ColorPicker* Picker`), an export of Source: Remap
       names it SelfRef until the stub's own generated class has its export row. */
    std::string SelfClass;
    static constexpr int32 SelfRef = 0x3fffffff;
};

class FApiWriter : public FEditorPackage
{
public:
    FApiWriter(const FApiClass& InClass, const FPackage& InSource)
        : FEditorPackage(InSource, InClass.PackageName + "/" + InClass.AssetName), Class(InClass)
    {
    }

    bool Write(const std::string& OutDir, std::string* Err);

private:
    void WriteWidgetTree(int32 ExBlueprint, int32 ExTree);
    const FApiClass& Class;
};

FIndex FEditorPackage::PackageImport(const std::string& Name)
{
    const std::string Key = "pkg|" + Name;
    auto It = ImportCache.find(Key);
    if (It != ImportCache.end()) return Imp(It->second);
    const int32 Row = P.AddImport({ "/Script/CoreUObject", "Package", Null(), Name });
    ImportCache.emplace(Key, Row);
    return Imp(Row);
}

FIndex FEditorPackage::Object(const std::string& Package, const std::string& ClassName,
                          const std::string& ObjectName, const std::string& ClassPackage)
{
    const std::string Key = ClassPackage + "|" + Package + "|" + ClassName + "|" + ObjectName;
    auto It = ImportCache.find(Key);
    if (It != ImportCache.end()) return Imp(It->second);
    const FIndex Outer = PackageImport(Package);
    const int32 Row = P.AddImport({ ClassPackage, ClassName, Outer, ObjectName });
    ImportCache.emplace(Key, Row);
    return Imp(Row);
}

bool FEditorPackage::Remap(FIndex Src, FIndex& Out)
{
    if (Src.V == 0) { Out = Null(); return true; }
    if (Src.V > 0 && !SelfClass.empty() && size_t(Src.V) <= Source.ExportRows().size()
        && Source.ExportRows()[size_t(Src.V) - 1].ObjectName == SelfClass)
    {
        Out.V = SelfRef;
        return true;
    }
    const FImport* In = Source.ImportAt(Src);
    if (!In) return false;
    const FImport* Outer = Source.ImportAt(In->Outer);
    if (!Outer || Outer->ClassName != "Package") return false;
    /* A /Game type is another of this mod's own assets - a UE_STRUCT / UE_ENUM. It now has an uncooked
       stub emitted beside this one (same package path), so an import of it resolves in the editor: a
       hard ref for a class variable's pin, a soft path (ObjectPath) for a struct member's VarDesc. */
    Out = Object(Outer->ObjectName, In->ClassName, In->ObjectName, In->ClassPackage);
    return true;
}

bool FEditorPackage::SignatureOf(FIndex Fn, FIndex& Parent, std::string& Name)
{
    if (Fn.V > 0 && size_t(Fn.V) <= Source.ExportRows().size())
    {
        const FExport& Row = Source.ExportRows()[size_t(Fn.V) - 1];
        Name = Row.ObjectName;
        return Remap(Row.OuterIndex, Parent);
    }
    const FImport* In = Source.ImportAt(Fn);
    if (!In) return false;
    Name = In->ObjectName;
    return Remap(In->Outer, Parent);
}

bool FEditorPackage::RemapValue(FDefaultValue& V)
{
    if (V.K == FDefaultValue::Obj && (V.Instanced || V.Object.V > 0 || !Remap(V.Object, V.Object))) return false;
    for (FDefaultValue& Item : V.Items)
        if (!RemapValue(Item)) return false;
    if (V.Members)
    {
        auto Kept = std::make_shared<std::vector<FPropertyDef>>();
        for (FPropertyDef M : *V.Members)
            if (RemapDefault(M)) Kept->push_back(std::move(M));
        V.Members = Kept;
    }
    return true;
}

bool FEditorPackage::RemapDefault(FPropertyDef& D)
{
    if (D.Members)
    {
        auto Kept = std::make_shared<std::vector<FPropertyDef>>();
        for (FPropertyDef M : *D.Members)
            if (RemapDefault(M)) Kept->push_back(std::move(M));
        if (Kept->empty() && !D.Members->empty()) return false;
        D.Members = Kept;
    }
    return RemapValue(D.Default);
}

bool FEditorPackage::PinTypeOf(const FPropertyDef& Prop, FPinType& Out)
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
    /* A dispatcher. The compiler finds its signature by name, <Name>__DelegateSignature from the graph of that name
       in DelegateSignatureGraphs, so the pin carries no member reference. */
    else if (T == "MulticastInlineDelegateProperty" || T == "MulticastDelegateProperty") Out.Category = "mcdelegate";
    /* A delegate value names its signature (PinSubCategoryMemberReference), as CreatePropertyOnScope resolves it. */
    else if (T == "DelegateProperty")
    {
        Out.Category = "delegate";
        if (!SignatureOf(Prop.Extra, Out.MemberParent, Out.MemberName)) return false;
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
            // FEdGraphTerminalType has no member reference, so a map's value is no delegate.
            if (!Prop.Value || !PinTypeOf(*Prop.Value, *Value) || Value->Category == "delegate") return false;
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
    SelfClass = ClassName;
    const bool bFunctionLibrary = Class.ParentClass == "BlueprintFunctionLibrary";

    /* An actor needs nothing extra here: UBlueprint::PostLoad builds the SimpleConstructionScript,
       its SCS_Node and the DefaultSceneRoot itself, once there is a GeneratedClass to outer them to
       and a CDO export for the deferred loader. Both are emitted below. */

    const bool bWidget = Class.bWidget;
    const FIndex ImpBlueprint = bWidget ? Object("/Script/UMGEditor", "Class", "WidgetBlueprint")
                                        : Object("/Script/Engine", "Class", "Blueprint");
    const FIndex ImpEdGraph = Object("/Script/Engine", "Class", "EdGraph");
    const FIndex ImpSchema = Object("/Script/BlueprintGraph", "Class", "EdGraphSchema_K2");
    const FIndex ImpEntry = Object("/Script/BlueprintGraph", "Class", "K2Node_FunctionEntry");
    const FIndex ImpResult = Object("/Script/BlueprintGraph", "Class", "K2Node_FunctionResult");
    /* A mod parent (an interface extending another of the mod's) is a Blueprint's generated class. */
    const bool bModParent = Class.ParentPackage.compare(0, 6, "/Game/") == 0;
    const FIndex ImpParent = bModParent ? Object(Class.ParentPackage, "BlueprintGeneratedClass", Class.ParentClass, "/Script/Engine")
                                        : Object(Class.ParentPackage, "Class", Class.ParentClass);
    const FIndex ImpBpgc = bWidget ? Object("/Script/UMG", "Class", "WidgetBlueprintGeneratedClass")
                                   : Object("/Script/Engine", "Class", "BlueprintGeneratedClass");
    const FIndex ImpObjectClass = Object("/Script/CoreUObject", "Class", "Object");
    const FIndex ImpParentCdo = Object(Class.ParentPackage, Class.ParentClass,
                                        "Default__" + Class.ParentClass, Class.ParentPackage);
    std::vector<FIndex> Interfaces;
    for (FIndex SourceInterface : Class.Interfaces)
    {
        FIndex Interface;
        if (Remap(SourceInterface, Interface)) Interfaces.push_back(Interface);
    }

    struct FGraph
    {
        std::string Name;
        std::string Category, ToolTip, Keywords;
        uint32 Flags = 0;
        bool bPure = false;
        bool bDelegate = false;     // a dispatcher's signature, one of DelegateSignatureGraphs
        FIndex Interface;
        std::vector<FPinDef> EntryPins, ResultPins;
    };
    std::vector<FGraph> Graphs;

    /* A dispatcher is its variable (below) and a signature graph named as it is, whose entry node carries the
       parameters, as the editor's "Add Event Dispatcher" makes them; the compiler names the function
       <Name>__DelegateSignature. A signature with no visible dispatcher of that name is left out. */
    const std::string SignatureSuffix = "__DelegateSignature";
    auto DispatcherOf = [&](const FApiFunction& Fn) {
        if (!(Fn.Flags & (FUNC_Delegate | FUNC_MulticastDelegate)) || Fn.Name.size() <= SignatureSuffix.size()
            || Fn.Name.compare(Fn.Name.size() - SignatureSuffix.size(), SignatureSuffix.size(), SignatureSuffix) != 0)
            return std::string();
        const std::string Name = Fn.Name.substr(0, Fn.Name.size() - SignatureSuffix.size());
        for (const FPropertyDef& Prop : Class.Variables)
            if (Prop.Name == Name && Prop.Type.compare(0, 9, "Multicast") == 0) return Name;
        return std::string();
    };

    /* One graph per kept function. A signature graph (Signature, the name without the suffix) takes the flags the
       editor's new dispatcher gets (FBlueprintEditor::OnAddNewDelegate). False when a parameter has no pin type. */
    auto AddGraph = [&](const FApiFunction& Fn, const std::string& Signature) {
        FGraph G;
        G.Name = Signature.empty() ? Fn.Name : Signature;
        G.bDelegate = !Signature.empty();
        if (Fn.Interface.V && !Remap(Fn.Interface, G.Interface)) return false;
        if (auto Cat = Class.Categories.find(Fn.Name); Cat != Class.Categories.end()) G.Category = Cat->second;
        if (auto Doc = Class.Docs.find(Fn.Name); Doc != Class.Docs.end()) std::tie(G.ToolTip, G.Keywords) = Doc->second;
        // The flags the editor's new dispatcher gets (FBlueprintEditor::OnAddNewDelegate).
        G.Flags = G.bDelegate ? FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public : Fn.Flags;
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
            /* The editor owns this one. UK2Node_FunctionEntry::AllocateDefaultPins gives every
               static function graph a hidden __WorldContext pin (and ensure()s that none exists),
               and the compiler appends the matching parm last plus MD_WorldContext. Carrying our
               own leaves a visible pin on every call node - and naming it __WorldContext trips the
               ensure. It is last in the cooked signature too, so the layouts still line up. */
            if ((Fn.Flags & FUNC_Static) && Prop.Name.compare(0, 12, "WorldContext") == 0) continue;
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
        if (bOk) Graphs.push_back(std::move(G));
        return bOk;
    };

    for (const FApiFunction& Fn : Class.Functions)
    {
        const std::string Dispatcher = DispatcherOf(Fn);
        if (Dispatcher.empty())
        {
            if (!(Fn.Flags & FUNC_BlueprintCallable)) continue;
            if (Fn.Flags & (FUNC_UbergraphFunction | FUNC_Delegate | FUNC_MulticastDelegate)) continue;
            if (bFunctionLibrary && (Fn.Flags & (FUNC_Private | FUNC_Protected))) continue;
        }
        AddGraph(Fn, Dispatcher);
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
        std::string Category;
        uint8 RepCondition = 0;
        uint32 Guid[4] = { 0, 0, 0, 0 };
    };
    std::vector<FApiVar> Vars;
    for (const FPropertyDef& Prop : Class.Variables)
    {
        /* A designer widget is its own variable once the editor compiles the tree (bIsVariable, below). */
        if (std::any_of(Class.Widgets.begin(), Class.Widgets.end(),
                        [&](const FApiClass::FWidget& W) { return W.Name == Prop.Name; }))
            continue;
        FApiVar Var;
        Var.Name = Prop.Name;
        if (!PinTypeOf(Prop, Var.Type))
        {
            printf("  %-14s skipped: variable has no editor-side type yet\n", Prop.Name.c_str());
            continue;
        }
        /* The editor's compiler refuses one ("has an invalid type Array of Delegates"): only a parameter carries one. */
        if (Var.Type.Category == "delegate")
        {
            printf("  %-14s skipped: a Blueprint variable cannot hold a delegate\n", Prop.Name.c_str());
            continue;
        }
        if (Var.Type.Category == "mcdelegate"
            && std::none_of(Graphs.begin(), Graphs.end(), [&](const FGraph& G) { return G.bDelegate && G.Name == Prop.Name; }))
        {
            printf("  %-14s skipped: dispatcher's signature has a parameter with no editor-side type yet\n",
                   Prop.Name.c_str());
            continue;
        }
        /* The editor's own set. CPF_Parm and the compiler-internal bits mean nothing on a variable,
           and a flag the details panel cannot show would only be lost on the next compile anyway. */
        Var.Flags = Prop.PropertyFlags & (CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly
                                          | CPF_DisableEditOnInstance | CPF_DisableEditOnTemplate
                                          | CPF_EditConst | CPF_Net | CPF_RepNotify | CPF_Transient
                                          | CPF_Config | CPF_SaveGame | CPF_Interp | CPF_AdvancedDisplay
                                          | CPF_ExposeOnSpawn | CPF_NonTransactional
                                          | CPF_BlueprintAssignable | CPF_BlueprintCallable);
        // What lists a dispatcher under the designer's Events and lets a graph bind and call it.
        if (Var.Type.Category == "mcdelegate") Var.Flags |= CPF_BlueprintAssignable | CPF_BlueprintCallable;
        Var.RepNotify = Prop.RepNotify;
        Var.RepCondition = Prop.RepCondition;
        Var.Default = DefaultString(Prop);
        if (auto Cat = Class.Categories.find(Prop.Name); Cat != Class.Categories.end()) Var.Category = Cat->second;
        MakeGuid(Class.AssetName + ".var." + Prop.Name, Var.Guid);
        Vars.push_back(std::move(Var));
    }

    /* A delegate pin of this class's own type names a signature function the cooked class made for it (one per
       delegate type, named after its first user). The stub makes it the way the compiler makes a dispatcher's: a
       signature graph of that name without the suffix. The compiler warns it finds no dispatcher of that name, and
       makes the function all the same (FinishCompilingFunction). */
    std::set<std::string> OwnSignatures;
    auto NoteSignature = [&](const FPinType& T) { if (T.MemberParent.V == SelfRef) OwnSignatures.insert(T.MemberName); };
    for (const FGraph& G : Graphs)
    {
        for (const FPinDef& Pin : G.EntryPins) NoteSignature(Pin.Type);
        for (const FPinDef& Pin : G.ResultPins) NoteSignature(Pin.Type);
    }
    for (const FApiVar& Var : Vars) NoteSignature(Var.Type);
    for (const FApiFunction& Fn : Class.Functions)
        if (OwnSignatures.count(Fn.Name) && Fn.Name.size() > SignatureSuffix.size()
            && !AddGraph(Fn, Fn.Name.substr(0, Fn.Name.size() - SignatureSuffix.size())))
            printf("  %-14s the signature has a parameter with no editor-side type yet: what names it will not"
                   " compile\n", Fn.Name.c_str());

    // Export rows, in the order the engine's own saves use: the Blueprint, then each graph with its nodes.
    const int32 ExBlueprint = 0;
    std::vector<int32> GraphExports;
    int32 Next = 1;
    for (const FGraph& G : Graphs)
    {
        GraphExports.push_back(Next);
        Next += G.ResultPins.size() > (G.bPure ? size_t(0) : size_t(1)) ? 3 : 2;
    }
    /* FAssetTypeActions_Blueprint::OpenAssetEditor warns "derives from an invalid class" on a null
       GeneratedClass, so the stub carries an empty one (compile-on-load fills it from the graphs)
       plus its CDO - the editor's deferred loading needs the CDO export to exist. */
    const int32 ExClass = Next;
    const int32 ExCdo = Next + 1;
    auto FixSelf = [&](FPinType& T) {
        if (T.SubCategoryObject.V == SelfRef) T.SubCategoryObject = Exp(ExClass);
        if (T.ValueType && T.ValueType->SubCategoryObject.V == SelfRef) T.ValueType->SubCategoryObject = Exp(ExClass);
        if (T.MemberParent.V == SelfRef) T.MemberParent = Exp(ExClass);
    };
    for (FGraph& G : Graphs)
    {
        for (FPinDef& Pin : G.EntryPins) FixSelf(Pin.Type);
        for (FPinDef& Pin : G.ResultPins) FixSelf(Pin.Type);
    }
    for (FApiVar& Var : Vars) FixSelf(Var.Type);

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
    if (Graphs.empty() && Vars.empty() && !bWidget && !Class.bOverrides)
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
    std::vector<int32> GraphRows, SignatureRows;
    /* An interface implementation belongs to that interface's Graphs, not FunctionGraphs. Otherwise
       ConformImplementedInterfaces creates a second graph of its name when the editor compiles it. */
    for (size_t I = 0; I < Graphs.size(); ++I)
        if (Graphs[I].bDelegate) SignatureRows.push_back(GraphExports[I]);
        else if (!Graphs[I].Interface.V) GraphRows.push_back(GraphExports[I]);
    /* The designer tree follows the class and its default object: UWidgetBlueprint's WidgetTree default subobject,
       then each widget, then each slot (WriteWidgetTree). */
    const int32 ExTree = Class.Widgets.empty() ? 0 : ExCdo + 1;
    Blueprint.Serialize = [=](FArc& Ar) {
        if (ExTree) Tag(Ar, "WidgetTree", "ObjectProperty", [=](FArc& V) { V.Idx(Exp(ExTree)); });
        Tag(Ar, "ParentClass", "ObjectProperty", [=](FArc& V) { V.Idx(ImpParent); });
        if (bFunctionLibrary || Class.bInterface)
            Tag(Ar, "BlueprintType", "ByteProperty",
                [&](FArc& V) { V.Name(Class.bInterface ? "BPTYPE_Interface" : "BPTYPE_FunctionLibrary"); }, "EBlueprintType");
        /* UBlueprint::BlueprintDescription: FinishCompilingClass sets it as the class's Tooltip metadata. */
        if (!Class.Description.empty())
            Tag(Ar, "BlueprintDescription", "StrProperty", [&](FArc& V) { V.Str(Class.Description); });
        Tag(Ar, "BlueprintSystemVersion", "IntProperty", [](FArc& V) { V.I32(2); });
        if (!Interfaces.empty())
            Tag(Ar, "ImplementedInterfaces", "ArrayProperty", [=](FArc& V) {
                FArc Elements(V.Owner());
                for (FIndex Interface : Interfaces)
                {
                    Tag(Elements, "Interface", "ObjectProperty", [&](FArc& E) { E.Idx(Interface); });
                    std::vector<int32> Rows;
                    for (size_t I = 0; I < Graphs.size(); ++I)
                        if (Graphs[I].Interface.V == Interface.V) Rows.push_back(GraphExports[I]);
                    if (!Rows.empty())
                        Tag(Elements, "Graphs", "ArrayProperty", [&](FArc& E) {
                            E.I32(int32(Rows.size()));
                            for (int32 Row : Rows) E.Idx(Exp(Row));
                        }, "ObjectProperty");
                    TagEnd(Elements);
                }
                StructArray(V, "ImplementedInterfaces", "BPInterfaceDescription", int32(Interfaces.size()), Elements);
            }, "StructProperty");
        if (!GraphRows.empty())
            Tag(Ar, "FunctionGraphs", "ArrayProperty", [=](FArc& V) {
                V.I32(int32(GraphRows.size()));
                for (int32 Row : GraphRows) V.Idx(Exp(Row));
            }, "ObjectProperty");
        if (!SignatureRows.empty())
            Tag(Ar, "DelegateSignatureGraphs", "ArrayProperty", [=](FArc& V) {
                V.I32(int32(SignatureRows.size()));
                for (int32 Row : SignatureRows) V.Idx(Exp(Row));
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
                    if (!Var.Category.empty())      // FBPVariableDescription::Category
                        Tag(Elements, "Category", "TextProperty", [&](FArc& E) { WriteText(E, Var.Category); });
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

                StructArray(V, "NewVariables", "BPVariableDescription", int32(Vars.size()), Elements);
            }, "StructProperty");
        Tag(Ar, "GeneratedClass", "ObjectProperty", [=](FArc& V) { V.Idx(Exp(ExClass)); });
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
            if (G.bDelegate) TagBool(Ar, "bEditable", false);   // a signature is edited through its dispatcher
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
        const FIndex FnInterface = G.Interface;
        const std::string FnCategory = G.Category, FnToolTip = G.ToolTip, FnKeywords = G.Keywords;
        Entry.Serialize = [=](FArc& Ar) {
            /* Not decoration: ConformFunctionNames renames a graph to its entry node's member name
               on load, so an entry without one turns every function into "None". */
            Tag(Ar, "FunctionReference", "StructProperty", [=](FArc& V) {
                if (FnInterface.V) Tag(V, "MemberParent", "ObjectProperty", [=](FArc& N) { N.Idx(FnInterface); });
                Tag(V, "MemberName", "NameProperty", [=](FArc& N) { N.Name(FnName); });
                TagEnd(V);
            }, "MemberReference");
            /* UK2Node_FunctionEntry::MetaData, an FKismetUserDeclaredFunctionMetadata, its fields in declaration order. */
            if (!FnCategory.empty() || !FnToolTip.empty() || !FnKeywords.empty())
                Tag(Ar, "MetaData", "StructProperty", [=](FArc& V) {
                    if (!FnToolTip.empty()) Tag(V, "ToolTip", "TextProperty", [=](FArc& T) { WriteText(T, FnToolTip); });
                    if (!FnCategory.empty()) Tag(V, "Category", "TextProperty", [=](FArc& T) { WriteText(T, FnCategory); });
                    if (!FnKeywords.empty()) Tag(V, "Keywords", "TextProperty", [=](FArc& T) { WriteText(T, FnKeywords); });
                    TagEnd(V);
                }, "KismetUserDeclaredFunctionMetadata");
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
                if (FnInterface.V) Tag(V, "MemberParent", "ObjectProperty", [=](FArc& N) { N.Idx(FnInterface); });
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

    FExport Generated;
    Generated.ClassIndex = ImpBpgc;
    Generated.SuperIndex = ImpParent;
    Generated.ObjectName = ClassName;
    Generated.ObjectFlags = RF_Public | RF_Transactional;
    Generated.Serialize = [=](FArc& Ar) {
        TagEnd(Ar);
        Ar.Bool(false);

        Ar.Idx(ImpParent);                          // SuperStruct
        Ar.I32(0);                                  // Children
        Ar.I32(0);                                  // ChildProperties
        Ar.I32(0);                                  // script bytecode size
        Ar.I32(0);                                  // script storage size

        Ar.I32(0);                                  // FuncMap
        /* CLASS_Parsed | CLASS_CompiledFromBlueprint, and CLASS_Interface on a Blueprint Interface (measured on an
           editor-made one, 0x44010). */
        Ar.U32(Class.bInterface ? 0x00044010 : 0x00040010);
        Ar.Idx(ImpObjectClass);                     // ClassWithin
        Ar.Name("Engine");                          // ClassConfigName
        Ar.Idx(Exp(ExBlueprint));                   // ClassGeneratedBy
        Ar.I32(int32(Interfaces.size()));
        for (FIndex Interface : Interfaces)
        {
            Ar.Idx(Interface);
            Ar.I32(0);
            Ar.Bool(true);                          // bImplementedByK2
        }
        Ar.Bool(false);                             // bDeprecatedForceScriptOrder
        Ar.Name("None");
        Ar.Bool(false);                             // bCooked
        Ar.Idx(Exp(ExCdo));                         // ClassDefaultObject
    };
    P.AddExport(std::move(Generated));

    FExport Cdo;
    Cdo.ClassIndex = Exp(ExClass);
    Cdo.TemplateIndex = ImpParentCdo;
    Cdo.ObjectName = "Default__" + ClassName;
    Cdo.ObjectFlags = RF_Public | RF_ClassDefaultObject | RF_ArchetypeObject;
    Cdo.Serialize = [](FArc& Ar) { TagEnd(Ar); };   // a CDO omits the lazy-object guid
    P.AddExport(std::move(Cdo));

    if (ExTree) WriteWidgetTree(ExBlueprint, ExTree);

    /* Without registry rows the content browser sees a package with nothing in it. */
    FRegistryObject Row;
    Row.ObjectPath = Class.AssetName;
    Row.ClassName = bWidget ? "WidgetBlueprint" : "Blueprint";
    const std::string ParentPath = "Class'" + Class.ParentPackage + "." + Class.ParentClass + "'";
    Row.Tags = {
        { "BlueprintType", Class.bInterface ? "BPTYPE_Interface" : bFunctionLibrary ? "BPTYPE_FunctionLibrary" : "BPTYPE_Normal" },
        { "ParentClass", ParentPath },
        { "NativeParentClass", ParentPath },
        { "GeneratedClass", std::string(bWidget ? "WidgetBlueprintGeneratedClass'" : "BlueprintGeneratedClass'")
                            + P.Name() + "." + ClassName + "'" },
        { "NumReplicatedProperties", "0" },
        { "IsDataOnly", "False" },
    };
    if (!Class.Description.empty()) Row.Tags.push_back({ "BlueprintDescription", Class.Description });   // the content browser's tooltip
    P.SetRegistryObjects({ Row });

    return P.Save(OutDir + "/" + Class.AssetName, Err);
}

/* The designer tree as the 4.27 editor saves a Widget Blueprint's (measured on an editor-saved one): no templates,
   every widget under the tree and every slot under its panel, the hierarchy in the slots' Parent and Content, the
   panel's Slots and the child's Slot, and the tree's editor-only AllWidgets. The cooked class's widget defaults ride
   along, so the designer draws the layout and what the constructor sets; what PreConstruct sets at run time is not
   there. Each widget is a variable (bIsVariable), as its C++ member is; one the game starts hidden is hidden in the
   designer too. */
void FApiWriter::WriteWidgetTree(int32 ExBlueprint, int32 ExTree)
{
    const int32 ExFirstWidget = ExTree + 1;
    const int32 ExFirstSlot = ExFirstWidget + int32(Class.Widgets.size());
    auto WidgetRow = [&](const std::string& Name) {
        for (size_t I = 0; I < Class.Widgets.size(); ++I)
            if (Class.Widgets[I].Name == Name) return ExFirstWidget + int32(I);
        return ExFirstWidget;
    };
    auto Defaults = [&](const std::vector<FPropertyDef>& In) {
        std::vector<FPropertyDef> Out;
        for (FPropertyDef D : In)
            if (RemapDefault(D)) Out.push_back(std::move(D));
        return Out;
    };
    auto ClassOf = [&](FIndex Src) {
        FIndex Out;
        return Remap(Src, Out) ? Out : Object("/Script/UMG", "Class", "Widget");
    };

    const int32 Count = int32(Class.Widgets.size());
    FExport Tree;
    Tree.ClassIndex = Object("/Script/UMG", "Class", "WidgetTree");
    Tree.OuterIndex = Exp(ExBlueprint);
    Tree.ObjectName = "WidgetTree";
    Tree.ObjectFlags = RF_Public | RF_Transactional | RF_DefaultSubObject;
    Tree.Serialize = [=](FArc& Ar) {
        Tag(Ar, "RootWidget", "ObjectProperty", [=](FArc& V) { V.Idx(Exp(ExFirstWidget)); });
        Tag(Ar, "AllWidgets", "ArrayProperty", [=](FArc& V) {
            V.I32(Count);
            for (int32 I = 0; I < Count; ++I) V.Idx(Exp(ExFirstWidget + I));
        }, "ObjectProperty");
        TagEnd(Ar);
        Ar.Bool(false);
    };
    P.AddExport(std::move(Tree));

    for (const FApiClass::FWidget& W : Class.Widgets)
    {
        std::vector<FIndex> Slots;
        FIndex Own;
        for (size_t I = 0; I < Class.WidgetSlots.size(); ++I)
        {
            if (Class.WidgetSlots[I].Panel == W.Name) Slots.push_back(Exp(ExFirstSlot + int32(I)));
            if (Class.WidgetSlots[I].Child == W.Name) Own = Exp(ExFirstSlot + int32(I));
        }
        FExport E;
        E.ClassIndex = ClassOf(W.Class);
        E.OuterIndex = Exp(ExTree);
        E.ObjectName = W.Name;
        E.ObjectFlags = RF_Transactional;
        /* A nested user widget's named slots this tree fills, {slot name, content}: UUserWidget's NamedSlotBindings. */
        std::vector<std::pair<std::string, FIndex>> Fills;
        for (const FApiClass::FWidgetSlotFill& F : Class.WidgetSlotFills)
            if (F.Host == W.Name) Fills.push_back({ F.Slot, Exp(WidgetRow(F.Content)) });
        E.Serialize = [=, Kept = Defaults(W.Defaults)](FArc& Ar) {
            /* The designer draws every widget whatever its Visibility, so one the game starts without (a button's
               glyph, collapsed until set) is hidden there instead: bHiddenInDesigner. */
            bool bVariable = false, bHidden = false;
            for (const FPropertyDef& D : Kept)
            {
                WriteDefaultTag(Ar, D);
                bVariable |= D.Name == "bIsVariable";
                bHidden |= D.Name == "Visibility"
                    && (D.Default.S == "ESlateVisibility::Collapsed" || D.Default.S == "ESlateVisibility::Hidden");
            }
            if (bHidden) TagBool(Ar, "bHiddenInDesigner", true);
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
            if (!bVariable) TagBool(Ar, "bIsVariable", true);
            TagEnd(Ar);
            Ar.Bool(false);
        };
        P.AddExport(std::move(E));
    }

    /* A slot is named <SlotClass>_<n>, counted per panel and class, as the cooked tree names it. */
    std::map<std::string, int32> Counts;
    for (const FApiClass::FWidgetSlot& S : Class.WidgetSlots)
    {
        const FImport* Of = Source.ImportAt(S.Class);
        const std::string SlotClass = Of ? Of->ObjectName : "PanelSlot";
        const FIndex Panel = Exp(WidgetRow(S.Panel)), Content = Exp(WidgetRow(S.Child));
        FExport E;
        E.ClassIndex = ClassOf(S.Class);
        E.OuterIndex = Panel;
        E.ObjectName = SlotClass + "_" + std::to_string(Counts[S.Panel + "." + SlotClass]++);
        E.ObjectFlags = RF_Transactional;
        E.Serialize = [=, Kept = Defaults(S.Defaults)](FArc& Ar) {
            for (const FPropertyDef& D : Kept) WriteDefaultTag(Ar, D);
            Tag(Ar, "Parent", "ObjectProperty", [=](FArc& V) { V.Idx(Panel); });
            Tag(Ar, "Content", "ObjectProperty", [=](FArc& V) { V.Idx(Content); });
            TagEnd(Ar);
            Ar.Bool(false);
        };
        P.AddExport(std::move(E));
    }
}

/* One StructVariableDescription the editor recompiles a member from. */
struct FVarDesc
{
    std::string Name;            // GUID-suffixed compiled name (== the cooked layout's field name)
    uint32 Guid[4] = { 0, 0, 0, 0 };
    std::string Friendly;        // clean C++ field name, for the details panel
    FPinType Type;
    std::string SubObjPath;      // SoftObjectPath to the pin's SubCategoryObject, empty when none
};

/* The 32-hex tail of a compiled member name is the field's FGuid (FMemberVariableNameHelper::Generate
   wrote <Friendly>_<UniqueId>_<Guid32hex>). Parse it back so VarGuid and VarName name the same GUID and
   the editor's GetGuidFromName resolves the member; recover the clean friendly name from the middle. */
bool GuidFromName(const std::string& Name, uint32 (&Out)[4], std::string& Friendly)
{
    if (Name.size() < 34 || Name[Name.size() - 33] != '_') return false;
    const std::string Hex = Name.substr(Name.size() - 32);
    for (char C : Hex) if (!std::isxdigit(static_cast<unsigned char>(C))) return false;
    for (int32 I = 0; I < 4; ++I) Out[I] = uint32(std::stoul(Hex.substr(I * 8, 8), nullptr, 16));
    const std::string Base = Name.substr(0, Name.size() - 33);      // drop "_<32hex>"
    const size_t U = Base.rfind('_');                              // drop "_<UniqueId>"
    Friendly = U != std::string::npos ? Base.substr(0, U) : Base;
    return true;
}

/* FSoftObjectPath: an FName asset path plus an (always empty here) sub-path FString. */
void WriteSoftObject(FArc& Ar, const std::string& Path)
{
    Ar.Name(Path.empty() ? "None" : Path);
    Ar.I32(0);
}

/* FEdGraphTerminalType, the map value half, as tagged properties. All-None for a non-map pin. */
void WriteTerminalType(FArc& Ar, const FPinType& T)
{
    const FPinType* V = (T.Container == 3 && T.ValueType) ? T.ValueType.get() : nullptr;
    Tag(Ar, "TerminalCategory", "NameProperty", [&](FArc& E) { E.Name(V ? V->Category : "None"); });
    Tag(Ar, "TerminalSubCategory", "NameProperty",
        [&](FArc& E) { E.Name(V && !V->SubCategory.empty() ? V->SubCategory : "None"); });
    Tag(Ar, "TerminalSubCategoryObject", "ObjectProperty",
        [&](FArc& E) { E.Idx(V ? V->SubCategoryObject : Null()); });
    TagBool(Ar, "bTerminalIsConst", V ? V->bIsConst : false);
    TagBool(Ar, "bTerminalIsWeakPointer", false);
    TagBool(Ar, "bTerminalIsUObjectWrapper", false);
    TagEnd(Ar);
}

/* One array element. An array's struct elements serialize with no CDO to delta against, so every
   field is written even at its default - the order and full set are what the editor reads back. */
void WriteVarDesc(FArc& Ar, const FVarDesc& VD)
{
    Tag(Ar, "VarName", "NameProperty", [&](FArc& E) { E.Name(VD.Name); });
    Tag(Ar, "VarGuid", "StructProperty", [&](FArc& E) { E.Raw(VD.Guid, 16); }, "Guid");
    Tag(Ar, "FriendlyName", "StrProperty", [&](FArc& E) { E.Str(VD.Friendly); });
    Tag(Ar, "DefaultValue", "StrProperty", [](FArc& E) { E.I32(0); });
    Tag(Ar, "Category", "NameProperty", [&](FArc& E) { E.Name(VD.Type.Category); });
    Tag(Ar, "SubCategory", "NameProperty",
        [&](FArc& E) { E.Name(VD.Type.SubCategory.empty() ? "None" : VD.Type.SubCategory); });
    Tag(Ar, "SubCategoryObject", "SoftObjectProperty", [&](FArc& E) { WriteSoftObject(E, VD.SubObjPath); });
    Tag(Ar, "PinValueType", "StructProperty", [&](FArc& E) { WriteTerminalType(E, VD.Type); }, "EdGraphTerminalType");
    Tag(Ar, "ContainerType", "EnumProperty", [&](FArc& E) {
        static const char* const Kinds[] = { "None", "Array", "Set", "Map" };
        E.Name(std::string("EPinContainerType::") + Kinds[VD.Type.Container & 3]);
    }, "EPinContainerType");
    TagBool(Ar, "bDontEditOnInstance", false);
    TagBool(Ar, "bEnableSaveGame", false);
    TagBool(Ar, "bEnableMultiLineText", false);
    TagBool(Ar, "bEnable3dWidget", false);
    Tag(Ar, "CurrentDefaultValue", "StrProperty", [](FArc& E) { E.I32(0); });
    Tag(Ar, "ToolTip", "StrProperty", [](FArc& E) { E.I32(0); });
    TagEnd(Ar);
}

/* Emits the two-export uncooked UserDefinedStruct: the compiled body (same GUID-suffixed member names
   as the cooked asset, plus an EditorData tag) and the UserDefinedStructEditorData the editor rebuilds
   from. Measured against an editor-saved 4.27.2 UserDefinedStruct. */
class FApiStructWriter : public FEditorPackage
{
public:
    FApiStructWriter(const FApiStruct& InStruct, const FPackage& InSource)
        : FEditorPackage(InSource, InStruct.PackageName), Struct(InStruct)
    {
    }

    bool Write(const std::string& OutDir, std::string* Err);

private:
    std::string ObjectPath(FIndex InP);
    const FApiStruct& Struct;
};

/* A hard import in P resolved to its "<OuterPackage>.<Object>" path, for a soft reference. */
std::string FApiStructWriter::ObjectPath(FIndex InP)
{
    if (InP.V == 0) return "";
    const FImport* Im = P.ImportAt(InP);
    if (!Im) return "";
    const FImport* Outer = P.ImportAt(Im->Outer);
    return (Outer ? Outer->ObjectName : "") + "." + Im->ObjectName;
}

bool FApiStructWriter::Write(const std::string& OutDir, std::string* Err)
{
    /* The editor recompiles the struct from the VarDescs, so the compiled body must hold exactly the
       members the VarDescs do. A member skipped here (its type has no editor-side asset) is dropped
       from both, or its compiled StructProperty would dangle and crash FStructProperty::LinkInternal. */
    std::vector<FVarDesc> Descs;
    std::vector<FPropertyDef> Members;
    for (const FPropertyDef& M : Struct.Members)
    {
        FVarDesc VD;
        VD.Name = M.Name;
        if (!GuidFromName(M.Name, VD.Guid, VD.Friendly))
        {
            if (Err) *Err = Struct.StructName + ": member " + M.Name + " has no GUID suffix";
            return false;
        }
        if (!PinTypeOf(M, VD.Type))
        {
            /* A member whose type is another mod's UE_STRUCT / UE_ENUM has no editor-side asset to
               point a pin at yet (Remap rejects a /Game outer). Skip it with a note - the same gap
               the class writer prints for such a variable - rather than emit a half-typed member. */
            printf("  %-14s skipped member %s: type has no editor-side asset yet\n",
                   Struct.StructName.c_str(), VD.Friendly.c_str());
            continue;
        }
        VD.SubObjPath = ObjectPath(VD.Type.SubCategoryObject);
        Descs.push_back(std::move(VD));
        Members.push_back(M);
    }
    if (Descs.empty())
    {
        if (Err) *Err = Struct.StructName + ": no member has an editor-side type yet";
        return false;
    }

    const FIndex ImpUds = Object("/Script/Engine", "Class", "UserDefinedStruct");
    const FIndex ImpEditorData = Object("/Script/UnrealEd", "Class", "UserDefinedStructEditorData");

    uint32 PkgGuid[4];
    MakeGuid(P.Name(), PkgGuid);
    P.SetUncooked(true);
    P.SetGuid(PkgGuid[0], PkgGuid[1], PkgGuid[2], PkgGuid[3]);

    const int32 ExStruct = 0, ExEditorData = 1;
    uint32 StructGuid[4] = { Struct.Guid[0], Struct.Guid[1], Struct.Guid[2], Struct.Guid[3] };

    FExport S;
    S.ClassIndex = ImpUds;
    S.OuterIndex = Null();
    S.ObjectName = Struct.StructName;
    S.ObjectFlags = RF_Public | RF_Standalone | RF_Transactional;
    S.bIsAsset = true;
    S.Serialize = [=](FArc& Ar) {
        Tag(Ar, "EditorData", "ObjectProperty", [=](FArc& V) { V.Idx(Exp(ExEditorData)); });
        Tag(Ar, "Guid", "StructProperty", [=](FArc& V) { V.Guid(StructGuid); }, "Guid");
        TagEnd(Ar);
        Ar.Bool(false);

        Ar.Idx(Null());                         // SuperStruct
        Ar.I32(0);                              // Children (empty UField array)
        Ar.I32(int32(Members.size()));          // ChildProperties
        for (const FPropertyDef& M : Members) WriteProperty(Ar, M, /*bUncooked=*/true);
        Ar.I32(0);                              // bytecode size
        Ar.I32(0);                              // bytecode storage
        Ar.U32(0);                              // StructFlags
        for (const FPropertyDef& M : Members) WriteDefaultTag(Ar, M);
        TagEnd(Ar);
    };
    P.AddExport(std::move(S));

    const std::vector<FVarDesc> VarDescs = Descs;
    FExport ED;
    ED.ClassIndex = ImpEditorData;
    ED.OuterIndex = Exp(ExStruct);
    ED.ObjectName = "UserDefinedStructEditorData_0";
    ED.ObjectFlags = RF_Transactional;
    ED.Serialize = [=](FArc& Ar) {
        Tag(Ar, "UniqueNameId", "UInt32Property", [=](FArc& V) { V.U32(uint32(VarDescs.size())); });
        Tag(Ar, "VariablesDescriptions", "ArrayProperty", [=](FArc& V) {
            FArc Elements(V.Owner());
            for (const FVarDesc& VD : VarDescs) WriteVarDesc(Elements, VD);
            StructArray(V, "VariablesDescriptions", "StructVariableDescription", int32(VarDescs.size()), Elements);
        }, "StructProperty");
        TagEnd(Ar);
        Ar.Bool(false);
    };
    P.AddExport(std::move(ED));

    FRegistryObject Row;
    Row.ObjectPath = Struct.StructName;
    Row.ClassName = "UserDefinedStruct";
    P.SetRegistryObjects({ Row });

    return P.Save(OutDir + "/" + Struct.StructName, Err);
}
}   // namespace

bool WriteApiAsset(const FApiClass& Class, const FPackage& Source, const std::string& OutDir,
                   std::string* Err)
{
    FApiWriter Writer(Class, Source);
    return Writer.Write(OutDir, Err);
}

bool WriteApiStructAsset(const FApiStruct& Struct, const FPackage& Source, const std::string& OutDir,
                         std::string* Err)
{
    FApiStructWriter Writer(Struct, Source);
    return Writer.Write(OutDir, Err);
}

/* One UserDefinedEnum export. Measured against an editor-saved 4.27.2 UserDefinedEnum: tagged-props
   terminator (the editor's DisplayNameMap stays empty and is rebuilt in PostLoad), a 4-byte zero
   where the cooked writer drops a 1-byte bool, then UEnum::Names as (FName "<Enum>::<Entry>", int64)
   pairs ending in the "<Enum>_MAX" sentinel, then ECppForm::Namespaced. */
bool WriteApiEnumAsset(const FApiEnum& Enum, const std::string& OutDir, std::string* Err)
{
    FPackage P(Enum.PackageName);
    const int32 EnginePkg = P.AddImport({ "/Script/CoreUObject", "Package", Null(), "/Script/Engine" });
    const FIndex EnumClass = Imp(P.AddImport({ "/Script/CoreUObject", "Class", Imp(EnginePkg), "UserDefinedEnum" }));

    uint32 PkgGuid[4];
    MakeGuid(P.Name(), PkgGuid);
    P.SetUncooked(true);
    P.SetGuid(PkgGuid[0], PkgGuid[1], PkgGuid[2], PkgGuid[3]);

    int64 Max = 0;
    for (const auto& En : Enum.Entries) Max = std::max(Max, En.second + 1);
    const std::string Prefix = Enum.EnumName;
    const auto Entries = Enum.Entries;

    FExport E;
    E.ClassIndex = EnumClass;
    E.OuterIndex = Null();
    E.ObjectName = Enum.EnumName;
    E.ObjectFlags = RF_Public | RF_Standalone | RF_Transactional;
    E.bIsAsset = true;
    E.Serialize = [=](FArc& Ar) {
        TagEnd(Ar);
        Ar.I32(0);
        Ar.I32(int32(Entries.size() + 1));
        for (const auto& [Name, Value] : Entries)
        {
            Ar.Name(Prefix + "::" + Name);
            Ar.I64(Value);
        }
        Ar.Name(Prefix + "::" + Prefix + "_MAX");
        Ar.I64(Max);
        Ar.U8(1);                       // ECppForm::Namespaced
    };
    P.AddExport(std::move(E));

    FRegistryObject Row;
    Row.ObjectPath = Enum.EnumName;
    Row.ClassName = "UserDefinedEnum";
    P.SetRegistryObjects({ Row });
    return P.Save(OutDir + "/" + Enum.EnumName, Err);
}

}   // namespace Uasset
