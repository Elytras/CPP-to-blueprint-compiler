#include "Cpp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <map>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Blueprint.h"
#include "Package.h"
#include "Registry.h"
#include "Script.h"

namespace Uasset
{
namespace
{
using Json = nlohmann::json;

std::string Kind(const Json& N) { return N.value("kind", std::string()); }
std::string Name(const Json& N) { return N.value("name", std::string()); }

/* A UserDefinedStruct field's real FName is FMemberVariableNameHelper::Generate's output -
   <Friendly>_<UniqueId>_<Guid32hex>. We mint it deterministically from (struct package, field) so
   the cooked layout, every FFieldPath that references the field, and (later) the editor VarGuid all
   spell the byte-identical name. Forcing the suffix on every mod-struct field keeps us off the
   WITH_EDITOR authored-name fallback and matches what the editor itself would write. Native
   ScriptStructs have no suffix, so they keep their plain C++ field names. */
std::string ModFieldName(const std::string& StructPkg, const std::string& Field)
{
    const uint32 H = StrCrc32(StructPkg + "." + Field);
    const uint32 G[4] = { ~H, H * 2654435761u, H ^ 0x9E3779B9u, H };
    char Guid[33];
    std::snprintf(Guid, sizeof Guid, "%08X%08X%08X%08X", G[0], G[1], G[2], G[3]);
    return Field + "_" + std::to_string(H % 1000000u) + "_" + Guid;
}

/* FDeref and FDerefTextView are the compiler's own read-hoist plumbing: their fields are referenced
   by fixed names from hardcoded emit sites (ViewFieldOf, the __DerefScratch__ prologue), never
   through user field access, so they must keep their plain C++ names on both the cooked layout and
   every reference. ponytail: explicit two-name list; extend if another internal view struct appears. */
bool IsInternalViewStruct(const std::string& CppName)
{
    return CppName == "FDeref" || CppName == "FDerefTextView";
}

const Json* First(const Json& N)
{
    auto It = N.find("inner");
    return (It == N.end() || It->empty()) ? nullptr : &(*It)[0];
}

const Json* Nth(const Json& N, size_t I)
{
    auto It = N.find("inner");
    return (It == N.end() || It->size() <= I) ? nullptr : &(*It)[I];
}

std::string StripTypeKeywords(std::string T);
const Json* PeelLvalue(const Json* N);

std::string TypeOf(const Json& N)
{
    auto It = N.find("type");
    return It == N.end() ? std::string() : It->value("qualType", std::string());
}

/* A one-argument CXXConstructExpr is a wrapper (FString from a literal, a copy, a conversion)
   and is looked through; one with several arguments is a struct literal and stays. */
const Json* Strip(const Json* N)
{
    while (N)
    {
        const std::string K = Kind(*N);
        if (K == "CXXConstructExpr")
        {
            auto In = N->find("inner");
            if (In != N->end() && In->size() > 1) return N;
        }
        if (K != "ImplicitCastExpr" && K != "CStyleCastExpr" && K != "ParenExpr"
            && K != "ConstantExpr" && K != "ExprWithCleanups"
            && K != "CXXBindTemporaryExpr" && K != "MaterializeTemporaryExpr"
            && K != "CXXConstructExpr" && K != "CXXFunctionalCastExpr"
            && K != "CXXStaticCastExpr" && K != "CXXReinterpretCastExpr" && K != "CXXConstCastExpr"
            && K != "CXXDefaultArgExpr")
            return N;
        const Json* Inner = First(*N);
        if (!Inner) return N;
        N = Inner;
    }
    return nullptr;
}

template <typename F>
void ForEach(const Json& N, const F& Fn)
{
    auto It = N.find("inner");
    if (It == N.end()) return;
    for (const Json& C : *It) Fn(C);
}

/* What clang writes for a member the braces leave out; an aggregate member (a struct, a TArray) is a list of those. */
bool IsUnsetInit(const Json& E)
{
    const std::string K = Kind(E);
    if (K == "ImplicitValueInitExpr" || K == "CXXDefaultInitExpr") return true;
    if (K == "CXXConstructExpr") return !First(E);
    if (K != "InitListExpr") return false;
    bool bAll = true;
    ForEach(E, [&](const Json& C) { bAll = bAll && IsUnsetInit(C); });
    return bAll;
}

/* Every type spelling under N with the aliases in scope written out. clang spells a use as the source did, and a
   class-scope alias means what it says only inside that class and the ones deriving it - so it is expanded there,
   once, before anything reads a type. A name already qualified (`A::Leaf`) is left alone. */
void ExpandAliases(Json& N, const std::map<std::string, std::string>& InScope)
{
    if (!N.is_structured()) return;
    auto T = N.is_object() ? N.find("type") : N.end();
    if (T != N.end() && T->is_object() && T->contains("qualType"))
    {
        const std::string Q = (*T)["qualType"].get<std::string>();
        std::string Out;
        for (size_t I = 0; I < Q.size();)
        {
            if (!std::isalpha(uint8(Q[I])) && Q[I] != '_') { Out += Q[I++]; continue; }
            size_t E = I;
            while (E < Q.size() && (std::isalnum(uint8(Q[E])) || Q[E] == '_')) ++E;
            const bool bQualified = I >= 2 && Q[I - 1] == ':' && Q[I - 2] == ':';
            const auto A = bQualified ? InScope.end() : InScope.find(Q.substr(I, E - I));
            Out += A == InScope.end() ? Q.substr(I, E - I) : A->second;
            I = E;
        }
        if (Out != Q) (*T)["qualType"] = Out;
    }
    for (Json& C : N) ExpandAliases(C, InScope);
}

/* A CXXOperatorCallExpr whose callee is operator=: struct assignment, which clang does not spell
   as a BinaryOperator. The callee is the first inner node. */
bool IsAssignOperatorCall(const Json& N)
{
    const Json* Callee = Strip(First(N));
    if (!Callee || Kind(*Callee) != "DeclRefExpr") return false;
    auto Ref = Callee->find("referencedDecl");
    return Ref != Callee->end() && Ref->value("name", std::string()) == "operator=";
}

/* A clang StringLiteral spelling (prefix, quotes, escapes) as UTF-8. clang escapes a narrow
   literal's non-ASCII bytes in octal, and a wide literal's code units in octal or hex. */
std::string Unquote(std::string Spelling)
{
    bool bWide = false;
    if (Spelling.compare(0, 2, "u8") == 0) Spelling.erase(0, 2);
    else if (!Spelling.empty() && (Spelling[0] == 'L' || Spelling[0] == 'u' || Spelling[0] == 'U'))
    {
        bWide = true;
        Spelling.erase(0, 1);
    }
    if (Spelling.size() < 2) return Spelling;

    const size_t End = Spelling.size() - 1;     // the closing quote
    auto Hex = [](char C) { return C <= '9' ? C - '0' : (C | 0x20) - 'a' + 10; };
    std::vector<uint32> Units;
    for (size_t I = 1; I < End; ++I)
    {
        if (Spelling[I] != '\\' || I + 1 >= End) { Units.push_back(uint8(Spelling[I])); continue; }
        char C = Spelling[++I];
        if (C >= '0' && C <= '7')
        {
            uint32 V = 0;
            for (int32 N = 0; N < 3 && I < End && Spelling[I] >= '0' && Spelling[I] <= '7'; ++N, ++I)
                V = V * 8 + uint32(Spelling[I] - '0');
            Units.push_back(V);
            --I;
        }
        else if (C == 'x')
        {
            uint32 V = 0;
            while (I + 1 < End && std::isxdigit(uint8(Spelling[I + 1]))) V = V * 16 + uint32(Hex(Spelling[++I]));
            Units.push_back(V);
        }
        else
        {
            static const std::map<char, char> Simple = {
                { 'n', '\n' }, { 't', '\t' }, { 'r', '\r' }, { 'a', '\a' }, { 'b', '\b' }, { 'f', '\f' }, { 'v', '\v' } };
            auto It = Simple.find(C);
            Units.push_back(uint8(It != Simple.end() ? It->second : C));
        }
    }

    std::string Out;
    for (size_t I = 0; I < Units.size(); ++I)
    {
        uint32 Cp = Units[I];
        if (!bWide) { Out.push_back(char(Cp)); continue; }
        if (Cp >= 0xD800 && Cp < 0xDC00 && I + 1 < Units.size() && Units[I + 1] >= 0xDC00 && Units[I + 1] < 0xE000)
            Cp = 0x10000 + ((Cp - 0xD800) << 10) + (Units[++I] - 0xDC00);
        if (Cp < 0x80) Out.push_back(char(Cp));
        else if (Cp < 0x800) { Out.push_back(char(0xC0 | (Cp >> 6))); Out.push_back(char(0x80 | (Cp & 0x3F))); }
        else if (Cp < 0x10000)
        {
            Out.push_back(char(0xE0 | (Cp >> 12)));
            Out.push_back(char(0x80 | ((Cp >> 6) & 0x3F)));
            Out.push_back(char(0x80 | (Cp & 0x3F)));
        }
        else
        {
            Out.push_back(char(0xF0 | (Cp >> 18)));
            Out.push_back(char(0x80 | ((Cp >> 12) & 0x3F)));
            Out.push_back(char(0x80 | ((Cp >> 6) & 0x3F)));
            Out.push_back(char(0x80 | (Cp & 0x3F)));
        }
    }
    return Out;
}

bool FindLiteral(const Json& N, std::string& Out)
{
    if (Kind(N) == "StringLiteral" && N.contains("value"))
    {
        Out = Unquote(N["value"].get<std::string>());
        return true;
    }
    bool bFound = false;
    ForEach(N, [&](const Json& C) { if (!bFound) bFound = FindLiteral(C, Out); });
    return bFound;
}

/* Does this subtree call the named function? The call's own spelling, not a string. */
bool CallsFunction(const Json& N, const char* Fn)
{
    if (Kind(N) == "DeclRefExpr" && N.contains("referencedDecl") && Name(N["referencedDecl"]) == Fn) return true;
    bool bFound = false;
    ForEach(N, [&](const Json& C) { if (!bFound) bFound = CallsFunction(C, Fn); });
    return bFound;
}

/* A variable's braced initializer, or null. A temporary inside the braces (a container's list) wraps them in cleanups. */
const Json* BracedInit(const Json& Var)
{
    const Json* I = First(Var);
    if (I && Kind(*I) == "ExprWithCleanups") I = First(*I);
    return I && Kind(*I) == "InitListExpr" ? I : nullptr;
}

/* What an override or an interface implementation takes of its parent's flags (KismetCompiler.cpp PrecompileFunction
   and the event stubs; measured on BP_SentryGun_MoveMarker). */
constexpr uint32 kOverrideInherits = FUNC_Exec | FUNC_Event | FUNC_BlueprintCallable | FUNC_BlueprintEvent
                                   | FUNC_BlueprintAuthorityOnly | FUNC_BlueprintCosmetic | FUNC_Const
                                   | FUNC_Public | FUNC_Protected | FUNC_Private | FUNC_BlueprintPure
                                   | FUNC_Net | FUNC_NetReliable | FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast;

bool IsStaticDecl(const Json& Decl) { return Decl.value("storageClass", std::string()) == "static"; }

/* UE_SERVER / UE_CLIENT / UE_MULTICAST / UE_RELIABLE, carried as attribute kinds (UeMeta.h). */
uint32 NetFlagsOf(const Json& Decl)
{
    uint32 Flags = 0;
    ForEach(Decl, [&](const Json& C) {
        const std::string K = Kind(C);
        if (K == "HotAttr") Flags |= FUNC_Net | FUNC_NetServer;
        else if (K == "ColdAttr") Flags |= FUNC_Net | FUNC_NetClient;
        else if (K == "FlattenAttr") Flags |= FUNC_Net | FUNC_NetMulticast;
        else if (K == "NoDebugAttr") Flags |= FUNC_Net | FUNC_NetReliable;
    });
    return Flags;
}

/* UE_AUTHORITY_ONLY / UE_COSMETIC, the same way: the VM skips the call where the flag says it should not run. */
uint32 AccessFlagsOf(const Json& Decl)
{
    uint32 Flags = 0;
    ForEach(Decl, [&](const Json& C) {
        const std::string K = Kind(C);
        /* `#pragma clang optimize off` adds an implicit noinline beside its optnone: not a UE_AUTHORITY_ONLY. */
        if (K == "NoInlineAttr" && !C.value("implicit", false)) Flags |= FUNC_BlueprintAuthorityOnly;
        else if (K == "NoInstrumentFunctionAttr") Flags |= FUNC_BlueprintCosmetic;
    });
    return Flags;
}

/* UE_NO_OPTIMIZE, or an optimize-off pragma over the function: [[clang::optnone]], meaning what it says. */
bool IsNoOptDecl(const Json& Decl)
{
    bool bNoOpt = false;
    ForEach(Decl, [&](const Json& C) { bNoOpt = bNoOpt || Kind(C) == "OptimizeNoneAttr"; });
    return bNoOpt;
}

/* UE_PURE: clang keeps [[gnu::pure]] as a PureAttr child, and copies it onto an out-of-line definition. */
bool IsPureDecl(const Json& Decl)
{
    bool bPure = false;
    ForEach(Decl, [&](const Json& C) { bPure = bPure || Kind(C) == "PureAttr"; });
    return bPure;
}

std::vector<std::string> ParmNames(const Json& Decl)
{
    std::vector<std::string> Out;
    ForEach(Decl, [&](const Json& C) { if (Kind(C) == "ParmVarDecl") Out.push_back(Name(C)); });
    return Out;
}

/* WorldContextObject, WorldContext, Dumper-7's WorldContextObject_0: the parameter a Blueprint wires to self. */
bool IsWcoName(const std::string& N) { return N.compare(0, 12, "WorldContext") == 0; }

struct FRecord
{
    std::string CppName;
    std::string UePackage;                          // from UE_CLASS; empty = declared here
    std::string UeName;
    std::string Base;
    std::map<std::string, const Json*> Methods;     // in-class decl (carries storageClass)
    std::map<std::string, const Json*> MethodDefs;  // out-of-line definition (carries body/parms)
    std::vector<const Json*> Fields;
    std::vector<std::string> Interfaces;            // every base after the first
    std::map<std::string, std::string> Replicated;  // UE_REPLICATED*: variable -> "Notify:Condition"
    std::set<std::string> Components;               // UE_COMPONENT: variables that are also SCS nodes
    /* genueapi's `<X>__UeName`: the engine's name of a member or function Dumper-7 had to respell (`Name_0` is
       `Name`, `Audio_Flying` is `Audio Flying`). C++ keeps its spelling; everything cooked goes through UeNameOf. */
    std::map<std::string, std::string> UeNames;
    /* The C++ access specifier, which is the editor's own: private is the class alone, protected its subclasses too.
       A function carries it as a flag. A variable has private only, and only as editor metadata, so a private
       field is simply left out of the API stub; a protected one stays visible, a subclass having a right to it. */
    std::map<std::string, uint32> MethodAccess;     // method -> FUNC_Public / FUNC_Protected / FUNC_Private
    std::set<std::string> PrivateFields;
    std::map<std::string, std::string> ScsNodes;    // `<X>__UeScsNode`: a game Blueprint's component -> its node's guid, 32 hex
    std::map<std::string, std::string> TypeAliases; // `using Leaf = Game::...::Leaf;` in the class body
    const Json* Defaults = nullptr;                 // UE_DEFAULTS: the static-init block, never lowered
    bool bIsLocal = false;      // UePackage == ModPackage/CppName: cooked here, published at its /Game path
    bool bIsStruct = false;     // UE_STRUCT: cooked as a UserDefinedStruct asset
    bool bIsInterface = false;  // UE_INTERFACE: cooked as a BPGC whose super is UInterface

    bool IsNative() const { return !UePackage.empty() && !bIsLocal; }
    bool IsGenerated() const { return !IsNative() && (bIsStruct || bIsInterface || !Base.empty()); }
    /* A UserDefinedStruct: cooked here, or cooked by the mod its UE_STRUCT_IN names and imported. */
    bool IsModStruct() const { return bIsStruct && (UePackage.empty() || UePackage.compare(0, 6, "/Game/") == 0); }
};

struct FCallIR;

struct FArgIR
{
    enum EKind { Self, Int, Int64, Float, Bool, Byte, Str, Name, Text, Field, Local, LocalOut, Member, Call, NullObj, StructLit,
                 ObjConst, SoftPath, DynCast, Index, Delegate, InterfaceCtx, LatentInfo } K = Self;
    int32 I = 0;            // Int / Byte: the value; StructLit: the struct's size
    int64 I64 = 0;
    float F = 0.0f;
    bool B = false;
    bool bWide = false;     // Str: emit EX_UnicodeStringConst
    std::string S;          // Str: the literal; Field/Local/LocalOut/Member: the property name; Delegate: the bound function
    FIndex Owner;           // Field: declaring class; Member / StructLit: the struct; ObjConst / DynCast: the class (locals are owned by the function, resolved at emit)
    EExprToken LetOp = EX_Let;
    EExprToken CastOp = EX_DynamicCast;     // DynCast: which class cast
    std::string InnerType;          // clang type of the expression this was lowered from
    std::shared_ptr<FCallIR> Sub;   // Call: the call; StructLit: Args holds one value per reflected field, in order
    std::shared_ptr<FArgIR> Base;   // Member: the struct-valued expression; Index: the array variable (Sub->Args[0] is the index);
                                    // Field: the object, when not self; InterfaceCtx: the interface value
};

enum EStrKind { SK_None, SK_Str, SK_Name, SK_Text, SK_Int, SK_Int64, SK_Float, SK_Bool, SK_Byte, SK_Object };

const char* TypeNameOf(EStrKind K)
{
    switch (K)
    {
    case SK_Str: return "FString"; case SK_Name: return "FName"; case SK_Text: return "FText";
    case SK_Int: return "int"; case SK_Int64: return "int64"; case SK_Float: return "float";
    case SK_Bool: return "bool"; case SK_Byte: return "uint8"; case SK_Object: return "class UObject*";
    default: return "";
    }
}

/* One row of UeApi/Conv.json: a Kismet Conv_XToY between two canonical type names, plus the
   constants for its formatting parameters. */
struct FConv
{
    std::string From, To;
    std::string Package, Class, Fn;
    std::vector<Json> Extra;
};

/* One row of UeApi/Ops.json: the Kismet function behind `Lhs <op> Rhs`. */
struct FOpInfo
{
    std::string Op, Lhs, Rhs, Ret;
    std::string Package, Class, Fn;
    std::vector<Json> Extra;
};

/* UeApi/Types.json: what a native enum or ScriptStruct is called in its package, and its layout. */
struct FEnumInfo
{
    std::string Package, UeName, Underlying, First;     // First: the enumerator a zero is written as
};

bool IsContainerType(const std::string& T)
{
    const std::string S = StripTypeKeywords(T);
    return S.compare(0, 7, "TArray<") == 0 || S.compare(0, 5, "TSet<") == 0 || S.compare(0, 5, "TMap<") == 0;
}

/* "K, V" -> {"K", "V"} at top-level commas. */
std::vector<std::string> SplitTemplateArgs(const std::string& S)
{
    std::vector<std::string> Out;
    int32 Depth = 0;
    size_t Start = 0;
    for (size_t I = 0; I < S.size(); ++I)
    {
        if (S[I] == '<') ++Depth;
        else if (S[I] == '>') --Depth;
        else if (S[I] == ',' && Depth == 0) { Out.push_back(StripTypeKeywords(S.substr(Start, I - Start))); Start = I + 1; }
    }
    Out.push_back(StripTypeKeywords(S.substr(Start)));
    for (std::string& A : Out) while (!A.empty() && A.front() == ' ') A.erase(A.begin());
    return Out;
}

struct FStructInfo
{
    std::string Package, UeName;
    int32 Size = 0, Align = 1;
    bool bComplete = false;                                     // every reflected field is known, so a literal can be built
    std::vector<std::pair<std::string, std::string>> Fields;    // (type, name) in property order
};

std::vector<Json> ExtraArgs(const Json& Row)
{
    std::vector<Json> Out;
    auto It = Row.find("extra");
    if (It != Row.end()) for (const Json& E : *It) Out.push_back(E);
    return Out;
}

FArgIR ConstArg(const Json& E)
{
    FArgIR X;
    if (E.is_boolean())         { X.K = FArgIR::Bool;  X.B = E.get<bool>(); }
    else if (E.is_number_float()) { X.K = FArgIR::Float; X.F = E.get<float>(); }
    else                        { X.K = FArgIR::Int;   X.I = E.get<int32>(); }
    return X;
}

struct FStmtIR;

struct FCallIR
{
    std::shared_ptr<std::vector<FStmtIR>> Inline;   // __Inline__: the expanded body, one Block statement
    std::string InlineResult;                       // __Inline__: the local holding its return value, or empty
    std::string InlineType;                         // __Inline__: that local's type
    FIndex Fn;                          // empty for intrinsics
    std::string Intrinsic;              // __NAME__ compiler intrinsic
    FIndex Extra;
    FIndex Extra2;
    bool bScript = false;               // callee is Blueprint bytecode
    bool bInstance = false;             // non-static method: needs the context object, not the class CDO
    FIndex Context;                     // CDO a static call runs against; null = self
    bool bPure = false;                 // a function of its arguments (UE_PURE, a Kismet operator or conversion): see DropUnusedPure
    std::string VirtualName;            // a generated class's own instance method: EX_VirtualFunction resolves it by name at run time
    bool bLocalVirtual = false;         // ... as EX_LocalVirtualFunction: a script function that is no RPC
    std::string View;                   // __RefAtInline__: the TArray field of the view struct in Extra
    std::shared_ptr<int32> Resume;      // __AwaitPoint__: receives the ubergraph offset the awaited event re-enters at
    std::shared_ptr<FArgIR> Target;     // the object an instance call runs against; null = self
    std::vector<FArgIR> Args;
};

/* EX_CallMath calls UFunction::Func with the CALLER's frame, which is only correct for a native;
   a bytecode callee must go through EX_FinalFunction (UFunction::Invoke builds its own frame).
   EX_CallMath also runs on the function's outer-class CDO and ignores EX_Context, so it is only
   right for a static: an instance native on it would run against e.g. Default__FSDGameState. */
void EmitCallOp(FScript& S, const FCallIR& Call)
{
    if (!Call.VirtualName.empty() && Call.bLocalVirtual) S.LocalVirtualFunction(Call.VirtualName);
    else if (!Call.VirtualName.empty()) S.VirtualFunction(Call.VirtualName);
    else if (Call.bScript || Call.bInstance) S.FinalFunction(Call.Fn);
    else S.CallMath(Call.Fn);
}

bool EmitCall(FScript& S, const FCallIR& Call, FIndex SelfExp, std::string* Err);

struct FStmtIR
{
    enum EKind
    {
        StaticCall,
        Assign,
        Decl,
        Return,
        If,
        While,
        Break,
        Continue,
        Switch,
        Label,
        Block,                                      // an inline function's body: Body, where InlineReturn jumps past
        InlineReturn,
        Goto,                                       // LabelId: the GotoLabel it jumps to
        GotoLabel,
    } K = StaticCall;

    FCallIR Target;
    FCallIR Call;
    FArgIR Var;
    FArgIR Value;
    FArgIR Cond;
    bool bHasValue = false;
    std::shared_ptr<std::vector<FStmtIR>> Then;
    std::shared_ptr<std::vector<FStmtIR>> Else;
    std::shared_ptr<std::vector<FStmtIR>> Body;
    std::shared_ptr<std::vector<FStmtIR>> Inc;      // While: a `for` increment, where `continue` lands
    std::shared_ptr<std::vector<FStmtIR>> Trailer;  // While: runs on `break` only, before leaving the loop
    bool bPostTest = false;                         // While: `do {} while (Cond)`, the test after Body and Inc
    std::vector<FArgIR> CaseTests;                  // Switch: per case, true when the value does NOT match
    int32 LabelId = -1;                             // Label: the case it marks; Switch: the default's label, or -1;
                                                    // Goto / GotoLabel: the label, unique in the class
    std::vector<int64> CaseValues;                  // Switch: each case's constant, in CaseTests order
    int32 SwitchWidth = 4;                          // Switch: the value's size, 1 / 4 / 8; 0 for an FName
    FArgIR SwitchValue;                             // Switch: the temp the value was stored in
    bool bAssignLocal = false;
    bool bAssignOutParm = false;
};

/* The TArray field at offset 0 of a read's engine view struct (kReadViews), by its __DerefRead*__. */
const char* ViewFieldOf(const std::string& DerefIntrinsic)
{
    return DerefIntrinsic == "__DerefReadI64__"  ? "NameHashes"     :
           DerefIntrinsic == "__DerefReadI32__"  ? "Mapping"        :
           DerefIntrinsic == "__DerefReadF__"    ? "Data"           :
           DerefIntrinsic == "__DerefReadStr__"  ? "AssetScanPaths" :
           DerefIntrinsic == "__DerefReadText__" ? "Data"           :
                                                   "Kilobyte";
}

/* SelfExp: the enclosing function's export index, FFieldPath owner of its params and locals. */
bool EmitArgs(FScript& S, const std::vector<FArgIR>& Args, FIndex SelfExp, std::string* Err);
FArgIR NotOf(FArgIR V, FBlueprintClass& BP);

bool EmitArg(FScript& S, const FArgIR& A, FIndex SelfExp, std::string* Err)
{
    switch (A.K)
    {
    case FArgIR::Self:    S.Self(); return true;
    case FArgIR::NullObj: if (A.CastOp == EX_NoInterface) S.NoInterface(); else S.NoObject(); return true;
    case FArgIR::Int:   S.IntConst(A.I); return true;
    case FArgIR::Int64: S.Int64Const(A.I64); return true;
    case FArgIR::Float: S.FloatConst(A.F); return true;
    case FArgIR::Bool:  A.B ? S.True() : S.False(); return true;
    case FArgIR::Byte:  S.ByteConst(uint8(A.I)); return true;
    case FArgIR::ObjConst: S.ObjectConst(A.Owner); return true;
    case FArgIR::Index:
    {
        if (!A.Base || !A.Sub || A.Sub->Args.size() != 1) { if (Err) *Err = "internal: Index arg is incomplete"; return false; }
        bool bOk = true;
        std::string SubErr;
        S.ArrayGetByRef([&](FScript& C) { bOk = bOk && EmitArg(C, *A.Base, SelfExp, &SubErr); },
                        [&](FScript& C) { bOk = bOk && EmitArg(C, A.Sub->Args[0], SelfExp, &SubErr); });
        if (!bOk && Err) *Err = SubErr;
        return bOk;
    }
    case FArgIR::SoftPath: S.SoftObjectConst(A.S); return true;
    case FArgIR::DynCast:
    {
        if (!A.Sub || A.Sub->Args.size() != 1) { if (Err) *Err = "internal: DynCast arg has no operand"; return false; }
        bool bOk = true;
        std::string SubErr;
        const auto Operand = [&](FScript& C) { bOk = EmitArg(C, A.Sub->Args[0], SelfExp, &SubErr); };
        if (A.CastOp == EX_PrimitiveCast) S.PrimitiveCast(ECastToken(A.I), Operand);
        else S.ClassCast(A.CastOp, A.Owner, Operand);
        if (!bOk && Err) *Err = SubErr;
        return bOk;
    }
    case FArgIR::Delegate: S.InstanceDelegate(A.S); return true;
    case FArgIR::InterfaceCtx:
    {
        if (!A.Base) { if (Err) *Err = "internal: InterfaceCtx arg has no interface"; return false; }
        bool bOk = true;
        std::string SubErr;
        S.InterfaceContext([&](FScript& C) { bOk = EmitArg(C, *A.Base, SelfExp, &SubErr); });
        if (!bOk && Err) *Err = SubErr;
        return bOk;
    }
    case FArgIR::StructLit:
    {
        if (!A.Sub) { if (Err) *Err = "internal: StructLit arg has no members"; return false; }
        bool bOk = true;
        std::string SubErr;
        S.StructConst(A.Owner, A.I, [&](FScript& C) {
            for (const FArgIR& M : A.Sub->Args)
                if (bOk) bOk = EmitArg(C, M, SelfExp, &SubErr);
        });
        if (!bOk && Err) *Err = SubErr;
        return bOk;
    }
    case FArgIR::LatentInfo:
        /* Measured on BP_LiftPod's Delay calls: {Linkage, UUID, ExecuteUbergraph_<Class>, self}, serialized size 32.
           Linkage is the resume point, the end of the statement, which EmitStmts patches in. */
        S.StructConst(A.Owner, 32, [&](FScript& C) {
            C.LatentResumes.push_back(C.SkipOffsetConst(0));
            C.IntConst(A.I);
            C.NameConst(A.S);
            C.Self();
        });
        return true;
    case FArgIR::Name:  S.NameConst(A.S); return true;
    case FArgIR::Text:  S.TextConst(A.S, A.bWide); return true;
    case FArgIR::Str:
        /* EX_StringConst is Latin-1, so anything non-ASCII goes out as UTF-16. */
        if (A.bWide || !IsAscii(A.S)) S.UnicodeStringConst(Utf8To16(A.S));
        else S.StringConst(A.S);
        return true;
    case FArgIR::Field:
    {
        if (!A.Base) { S.InstanceVariable(A.S, A.Owner); return true; }
        /* Another object's property: the rvalue names it, so a null object reads as zero. */
        bool bOk = true;
        std::string SubErr;
        S.Context([&](FScript& O) { bOk = EmitArg(O, *A.Base, SelfExp, &SubErr); },
                  [&](FScript& C) { C.InstanceVariable(A.S, A.Owner); }, A.S, A.Owner);
        if (!bOk && Err) *Err = SubErr;
        return bOk;
    }
    case FArgIR::Local: S.LocalVariable(A.S, SelfExp); return true;
    case FArgIR::LocalOut: S.LocalOutVariable(A.S, SelfExp); return true;
    case FArgIR::Member:
    {
        if (!A.Base) { if (Err) *Err = "internal: Member arg has no base"; return false; }
        bool bOk = true;
        std::string SubErr;
        S.StructMember(A.S, A.Owner, [&](FScript& C) { bOk = EmitArg(C, *A.Base, SelfExp, &SubErr); });
        if (!bOk && Err) *Err = SubErr;
        return bOk;
    }
    case FArgIR::Call:
        if (!A.Sub) { if (Err) *Err = "internal: Call arg has no sub-call"; return false; }
        if (A.Sub->Intrinsic == "__CurrentFunction__")
        {
            S.ObjectConst(SelfExp);
            return true;
        }
        if (A.Sub->Intrinsic == "__AddrOf__" || A.Sub->Intrinsic == "__NameIndex__" || A.Sub->Intrinsic == "__AsObject__")
        {
            /* StructMember with a donor field at Offset_Internal=0 copies ElementSize bytes straight out
               of the argument's own storage: ScreenMessageString.Key (8) / IntPoint.X (4 = ComparisonIndex) /
               DebugDisplayProperty.obj (8, an int64 read back as a UObject*). */
            const std::string InnerField = A.Sub->Intrinsic == "__AddrOf__" ? "Key"
                                         : A.Sub->Intrinsic == "__AsObject__" ? "obj" : "X";
            if (A.Sub->Args.size() != 1)
            {
                if (Err) *Err = A.Sub->Intrinsic + " takes exactly one argument";
                return false;
            }
            const FArgIR& Inner = A.Sub->Args[0];
            std::string SubErr;
            bool bInnerOk = true;
            S.StructMember(InnerField, A.Sub->Extra,
                [&](FScript& Ctx) { bInnerOk = EmitArg(Ctx, Inner, SelfExp, &SubErr); });
            if (!bInnerOk) { if (Err) *Err = SubErr; return false; }
            return true;
        }
        if (A.Sub->Intrinsic == "__DerefReadI64__" || A.Sub->Intrinsic == "__DerefReadI32__"
            || A.Sub->Intrinsic == "__DerefReadF__" || A.Sub->Intrinsic == "__DerefReadU8__"
            || A.Sub->Intrinsic == "__DerefReadStr__" || A.Sub->Intrinsic == "__DerefReadText__")
        {
            /* Hoisted Read: outer StructMember reads the view struct's TArray<T> at offset 0 out of
               the caller's FDeref __DerefScratch__ (Num=1, Data=<addr>). ArrayGetByRef then copies
               InnerProp.ElementSize bytes from Data[0] into the caller's dest. No class-owner check
               is involved (StructMemberContext + ArrayGetByRef are both guard-free). */
            const char* ViewField = ViewFieldOf(A.Sub->Intrinsic);
            const FIndex ViewStruct = A.Sub->Extra;
            S.ArrayGetByRef(
                [&](FScript& O)
                {
                    O.StructMember(ViewField, ViewStruct,
                        [&](FScript& I) { I.LocalVariable("__DerefScratch__", SelfExp); });
                },
                [&](FScript& I) { I.IntZero(); });
            return true;
        }
        if (A.Sub->Intrinsic == "__RefAtInline__")
        {
            const FIndex View = A.Sub->Extra;
            const std::string Scratch = A.S;
            S.ArrayGetByRef(
                [&](FScript& O)
                {
                    O.StructMember(A.Sub->View, View,
                        [&](FScript& I) { I.LocalVariable(Scratch, SelfExp); });
                },
                [&](FScript& I) { I.IntZero(); });
            return true;
        }
        if (!A.Sub->Intrinsic.empty())
        {
            if (Err) *Err = "TODO: unimplemented intrinsic " + A.Sub->Intrinsic;
            return false;
        }
        return EmitCall(S, *A.Sub, SelfExp, Err);
    }
    if (Err) *Err = "internal: unknown argument kind";
    return false;
}

bool EmitArgs(FScript& S, const std::vector<FArgIR>& Args, FIndex SelfExp, std::string* Err)
{
    for (const FArgIR& A : Args)
        if (!EmitArg(S, A, SelfExp, Err)) return false;
    return true;
}

bool EmitCall(FScript& S, const FCallIR& Call, FIndex SelfExp, std::string* Err)
{
    if (Call.Intrinsic == "__AwaitPoint__") { S.ResumeSinks.push_back(Call.Resume); return true; }
    if (Call.Intrinsic == "__Asm__")
    {
        if (Call.Args.empty() || Call.Args[0].K != FArgIR::Str)
        { if (Err) *Err = "__Asm__: first argument must be a string literal of hex bytes"; return false; }
        const std::string& Hex = Call.Args[0].S;
        std::vector<uint8> Bytes;
        Bytes.reserve(Hex.size() / 2);
        auto Nib = [](char C) { return C <= '9' ? C - '0' : (C | 0x20) - 'a' + 10; };
        int32 Half = -1;    // the high nibble waiting for its low nibble
        for (char C : Hex)
        {
            if (C == ' ' || C == '\t' || C == '\n' || C == '\r' || C == ',' || C == '_') continue;
            if (!std::isxdigit(uint8(C)))
            { if (Err) *Err = std::string("__Asm__: not a hex character: '") + C + "'"; return false; }
            if (Half < 0) Half = Nib(C);
            else { Bytes.push_back(uint8((Half << 4) | Nib(C))); Half = -1; }
        }
        if (Half >= 0) { if (Err) *Err = "__Asm__: odd number of hex digits"; return false; }
        int32 MemBytes = int32(Bytes.size());
        if (Call.Args.size() >= 2)
        {
            if (Call.Args[1].K != FArgIR::Int)
            { if (Err) *Err = "__Asm__: MemBytes must be an integer literal"; return false; }
            MemBytes = Call.Args[1].I;
        }
        S.Raw(Bytes.data(), Bytes.size(), MemBytes);
        return true;
    }
    /* A dispatcher operation: Args[0] is the dispatcher, then the delegate or the broadcast's arguments. */
    const bool bAdd = Call.Intrinsic == "__AddDelegate__", bRemove = Call.Intrinsic == "__RemoveDelegate__";
    if (bAdd || bRemove || Call.Intrinsic == "__ClearDelegate__" || Call.Intrinsic == "__Broadcast__")
    {
        bool bOk = true;
        std::string SubErr;
        auto Arg = [&](size_t I) { return [&, I](FScript& C) { bOk = bOk && EmitArg(C, Call.Args[I], SelfExp, &SubErr); }; };
        if (bAdd) S.AddMulticastDelegate(Arg(0), Arg(1));
        else if (bRemove) S.RemoveMulticastDelegate(Arg(0), Arg(1));
        else if (Call.Intrinsic == "__ClearDelegate__") S.ClearMulticastDelegate(Arg(0));
        else
        {
            S.CallMulticastDelegate(Call.Fn);
            bOk = EmitArgs(S, Call.Args, SelfExp, &SubErr);
            S.EndFunctionParms();
        }
        if (!bOk && Err) *Err = SubErr;
        return bOk;
    }

    /* A static call runs against its class's CDO via EX_Context; EX_CallMath finds the CDO itself. */
    if (Call.Context.V != 0 || Call.Target)
    {
        bool bOk = true;
        std::string SubErr;
        S.Context(
            [&](FScript& O) { if (Call.Target) EmitArg(O, *Call.Target, SelfExp, &SubErr); else O.ObjectConst(Call.Context); },
            [&](FScript& C)
            {
                EmitCallOp(C, Call);
                bOk = EmitArgs(C, Call.Args, SelfExp, &SubErr);
                C.EndFunctionParms();
            });
        if (!bOk && Err) *Err = SubErr;
        return bOk;
    }

    EmitCallOp(S, Call);
    if (!EmitArgs(S, Call.Args, SelfExp, Err)) return false;
    S.EndFunctionParms();
    return true;
}

void StampIdentity(FPackage& P, const std::string& PackageName)
{
    const uint32 H = StrCrc32(PackageName);
    P.SetGuid(H, H ^ 0x9E3779B9u, ~H, H * 2654435761u);
    P.SetPackageSource(H);
}

std::string ReadText(const std::string& Path)
{
    std::string Out;
    FILE* F = nullptr;
    if (fopen_s(&F, Path.c_str(), "rb") != 0 || !F) return Out;
    char Buf[16384];
    size_t N;
    while ((N = fread(Buf, 1, sizeof Buf, F)) > 0) Out.append(Buf, N);
    fclose(F);
    return Out;
}

/* Each entry maps a source-level __Read*__ intrinsic to the engine view struct the hoister
   reads through. The view struct has a TArray<T> at offset 0; ArrayGetByRef reads element[0]
   from the caller's FDeref __DerefScratch__ (Num=1, Data=<addr>) into a fresh local of type
   ResultType. StructMemberContext + ArrayGetByRef are guard-free, so no bitfix is needed. */
struct FReadViewSpec
{
    const char* Intrinsic;          // __Read64__ / __Read32__ / __ReadFloat__ / ...
    const char* DerefIntrinsic;     // __DerefReadI64__ / __DerefReadI32__ / __DerefReadF__ / __DerefReadU8__
    const char* ResultType;         // C++ type of the tmp local receiving the read
    const char* ViewStructPkg;
    const char* ViewStructName;     // an engine ScriptStruct whose first member is TArray<T>
};

/* __ReadObject__ / __ReadName__ share the int64 view: both are 8-byte scalars whose
   CopySingleValue is a plain 8-byte memcpy, which is exactly what UInt64Property.CopySingleValue
   does through ArrayGetByRef. */
static const FReadViewSpec kReadViews[] = {
    { "__Read64__",     "__DerefReadI64__", "int64",           "/Script/Engine", "MaterialCachedParameterEntry" },
    { "__Read32__",     "__DerefReadI32__", "int32",           "/Script/Engine", "LODMappingData"               },
    { "__ReadFloat__",  "__DerefReadF__",   "float",           "/Script/Engine", "CustomPrimitiveData"          },
    { "__ReadByte__",   "__DerefReadU8__",  "uint8",           "/Script/Engine", "BandwidthTestItem"            },
    { "__ReadObject__", "__DerefReadI64__", "class UObject *", "/Script/Engine", "MaterialCachedParameterEntry" },
    { "__ReadName__",   "__DerefReadI64__", "FName",           "/Script/Engine", "MaterialCachedParameterEntry" },
    /* __ReadClass__ shares the int64 view: CopySingleValue on the InnerProp only cares about
       ElementSize (an 8-byte memcpy), so the tmp's type just decides the FField class the
       assign lands in - which is what __ClassOf__(Out) checks against at each Get<T>PropertyByName. */
    { "__ReadClass__",  "__DerefReadI64__", "class UClass *",  "/Script/Engine", "MaterialCachedParameterEntry" },
    /* FString view: FStrProperty::CopySingleValue does a deep FString operator= (allocates a
       fresh TArray<TCHAR>), so ArrayGetByRef of the FString at *(FString*)Addr into an
       FString local produces an owned copy that DestroyStruct cleans up on return. */
    { "__ReadString__", "__DerefReadStr__", "FString",         "/Script/Engine", "AssetManagerSearchRules"      },
    /* FText view: no engine ScriptStruct has TArray<FText> at offset 0, so ReadProperty.cpp
       declares its own FDerefTextView { TArray<FText> Data; } which AssetGen cooks as a sibling
       .uasset in the ReadProperty mod folder. Blueprint.cpp:81 registers it on CallImports so
       its script init preloads before any bytecode referencing it. Same deep-copy pattern as
       FString: FTextProperty::CopySingleValue is an FText operator= that shares the TSharedRef
       refcount, so DestroyStruct cleans up on return. */
    { "__ReadText__",   "__DerefReadText__", "FText",           "/Game/_ElytrasMods/ReadProperty/FDerefTextView", "FDerefTextView" },
};

const FReadViewSpec* FindReadView(const std::string& Intrinsic)
{
    for (const FReadViewSpec& V : kReadViews)
        if (Intrinsic == V.Intrinsic) return &V;
    return nullptr;
}

class FCompiler
{
public:
    bool Run(const std::string& SourcePath, const std::string& IncludeDir,
             const std::string& OutDir, const std::string& InApiDir, std::string* Err);

private:
    bool Collect(std::string* Err);
    bool Generate(const FRecord& R, const std::string& OutDir, std::string* Err);
    bool GenerateStruct(const FRecord& R, const std::string& OutDir, std::string* Err);
    bool GenerateInterface(const FRecord& R, const std::string& OutDir, std::string* Err);
    /* Every T& parm is treated as an out-parm; the return value is the caller's to append. */
    bool LowerParams(const Json& M, const std::string& Fn, FBlueprintClass& BP,
                     std::vector<FPropertyDef>& Params, std::string* Err);
    bool GenerateEnum(const std::string& Name, const std::string& OutDir, std::string* Err);
    /* A namespace-scope `UMyDef MD_Big = { .Health = 500 };`: an instance of a UE class as its own package. */
    bool GenerateAsset(const Json& Var, const std::string& OutDir, std::string* Err);
    bool TypeToProperty(const std::string& QualType, const std::string& PName, uint64 ExtraFlags,
                        const std::string& Where, FBlueprintClass& BP, FPropertyDef* Out, std::string* Err);
    bool LayoutOf(const std::string& QualType, int32* Size, int32* Align, std::string* Err);
    bool StructLayout(const FRecord& R, int32* Size, int32* Align, std::string* Err);
    bool LowerBody(const Json& Body, FBlueprintClass& BP, std::vector<FStmtIR>& Out,
                   std::vector<FPropertyDef>& Locals, std::string* Err);

    /* A local this body declares, holds them all the uses of, and only ever reads: its constant initialiser can
       stand in for every read (ParmConst), so neither the property nor its store is compiled. */
    bool ReadOnlyLocal(const std::string& DeclId) const;
    bool LowerCall(const Json& CallExprNode, FBlueprintClass& BP, FCallIR& Out, std::string* Err);
    bool LowerRangeFor(const Json& ForNode, FBlueprintClass& BP, std::vector<FStmtIR>& Out,
                       std::vector<FPropertyDef>& Locals, std::string* Err);
    bool LowerWithoutPrefix(const Json& Stmt, FBlueprintClass& BP, std::vector<FStmtIR>& Out,
                            std::vector<FPropertyDef>& Locals, std::string* Err);

    /* `inline` functions are the editor's macros: never a UFunction, their body is copied into each caller.
       `inline` may sit on the declaration or on an out-of-line definition. */
    bool IsInlineMethod(const FRecord& R, const std::string& Method) const;
    bool ExpandInline(const Json& CallNode, const Json& Def, const std::string& Method, bool bMethod, FBlueprintClass& BP,
                      FCallIR& Out, std::string* Err);
    /* A constant outside any function body, `constexpr int32 kMax = 40;` at namespace scope or static in a class:
       decl id -> its VarDecl. It has no storage in a Blueprint, so a use is its value (FoldConst). */
    std::map<std::string, const Json*> ConstVars;
    struct FConstVal { bool bFloat = false; double F = 0; int64 I = 0; double Num() const { return bFloat ? F : double(I); } };
    bool FoldConst(const Json& E, FConstVal& Out) const;
    bool ConstToArg(const FConstVal& V, const std::string& Type, FArgIR& Out) const;
    std::map<std::string, const Json*> FreeInlines;                     // decl id -> an inline free function's definition
                                                                        // (a template's: each instantiation)
    /* The class a field access `Obj->Field` / `Field` reads from: Obj's static type, or the class being generated. */
    /* An interface and the interfaces it extends, nearest first. A native one has no base in UeApi. */
    std::vector<const FRecord*> InterfaceChain(const FRecord* I) const
    {
        std::vector<const FRecord*> Out;
        for (; I; I = I->Base.empty() ? nullptr : Find(I->Base)) Out.push_back(I);
        return Out;
    }
    /* A mod interface's variable is a property of the class that implements the interface, the one class in
       an ancestry that lists it (or an interface extending it): that class of C's, or null. */
    const FRecord* InterfaceVarHolder(const FRecord& Iface, const FRecord* C) const
    {
        for (; C; C = C->Base.empty() ? nullptr : Find(C->Base))
            for (const std::string& I : C->Interfaces)
                for (const FRecord* Link : InterfaceChain(Find(I)))
                    if (Link == &Iface) return C;
        return nullptr;
    }

    const FRecord* RecordOfFieldAccess(const Json& MemberNode) const
    {
        const Json* Base = Strip(First(MemberNode));
        if (!Base || Kind(*Base) == "CXXThisExpr") return Cur;
        std::string T = StripTypeKeywords(TypeOf(*Base));
        while (!T.empty() && (T.back() == '*' || T.back() == ' ')) T.pop_back();
        return Find(T);
    }
    std::string LocalName(const Json& Decl) const
    {
        auto It = LocalRename.find(Decl.value("id", std::string()));
        return It == LocalRename.end() ? Name(Decl) : It->second;
    }
    std::map<std::string, std::string> LocalRename;                     // decl id -> an inlined local's unique name
    std::map<std::string, FArgIR> ParmConst;                            // decl id -> the constant an inlined parameter is
    std::vector<std::pair<std::string, std::string>> InlineResults;     // per expansion in progress: result local, type
    std::vector<std::string> InlineStack;                               // the inline functions being expanded
    std::vector<FPropertyDef>* CurLocals = nullptr;                     // the function being lowered's locals
    bool LowerArg(const Json& ArgNode, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    bool LowerArgRaw(const Json& N, const std::string& OuterType, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    bool ConvertArg(const std::string& ToType, FBlueprintClass& BP, FArgIR& Arg, std::string* Err);
    bool LowerField(const Json& MemberNode, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    bool LowerMakeStruct(const std::string& Type, const Json* List, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    bool LowerDispatcherCall(const Json& Call, const Json& Callee, const Json& Obj, FBlueprintClass& BP,
                             FArgIR& Out, std::string* Err);
    bool LowerDelegateValue(const Json& Obj, const Json& Fn, FArgIR& Out, std::string* Err);

    /* Raw pointers: a pointer to anything but a UObject (void*, int32*, FName*, UObject**) is an address,
       an int64 at run time. NormalizePointers respells them int64 in a declaration's subtree before
       lowering, so every int64 path takes them; the pointer type stays in type.origQualType. */
    bool IsRawPointer(std::string QualType) const;
    void NormalizePointers(Json& N) const;
    bool IsDerefLvalue(const Json& N) const;
    bool IsEagerSafe(const Json& N) const;
    void ArgumentsInPlace(std::vector<FStmtIR>& Body, const std::vector<std::string>& Binds, const Json& Def,
                          const std::vector<std::string>& BindIds, std::vector<FPropertyDef>& Locals);
    const FReadViewSpec* ViewFor(const std::string& Pointee) const;
    bool LowerAddress(const Json& Lvalue, FBlueprintClass& BP, FArgIR& Out, std::string* Pointee, std::string* Err);
    bool ReadThrough(FArgIR Addr, const std::string& Pointee, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    bool RefThrough(FArgIR Addr, const std::string& Pointee, FArgIR& Out, std::string* Err);
    bool ScaleIndex(const Json& IndexNode, const std::string& Pointee, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    bool HoistOperand(FArgIR& Operand, FBlueprintClass& BP, std::vector<FPropertyDef>& Locals,
                      std::vector<FStmtIR>& OutPre, std::string* Err);
    bool HasDerefStruct(std::string* Err) const;
    bool LowerPtrCastSource(const Json& Call, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    std::map<std::string, std::string> RefAddr;     // VarDecl id -> pointee: `T& R = *P` keeps the address in an int64 local R
    std::map<std::string, Json> RefAlias;           // VarDecl id -> the variable `T& R = V` is another name for; a copy,
                                                    // since a for-init is lowered out of a temporary Json
    /* A /Game struct the bytecode names only through a member (a view, `P->X`) is not kept loaded: the script
       reference collector skips property operands (FArchive::operator<<(FField*&) does nothing), and only a
       property's own type reaches UStruct::ScriptAndPropertyObjectReferences (UStruct::Link), which the GC
       follows. An unused local of that type keeps it, unless a parm or local already has the type. */
    std::map<int32, FPropertyDef> KeepLoaded;       // struct import -> its keep-alive local
    void KeepStructLoaded(FIndex Struct, const std::string& Name, int32 Size)
    {
        FPropertyDef PD = StructParam("__Keep" + Name + "__", Struct, Name, Size, 0);
        PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
        KeepLoaded.emplace(Struct.V, PD);
    }

    /* Post-pass over the IR that turns every `__Read*__(Addr)` sub-expression into a pair of
       statements hoisted to the enclosing statement level:
           __DerefScratch__.Data = <Addr>
           __DerefTmpN__         = ArrayGetByRef(view.<field>, 0)   [via __DerefRead*__ intrinsic]
       The original sub-expression is replaced with LocalVariable(__DerefTmpN__). One
       __DerefScratch__ FDeref local is added per function on first use; the prologue seeds
       its Num=1 so ArrayGetByRef's bounds check passes. */
    void DropUnusedPure(std::vector<FStmtIR>& Stmts);
    void DropUnusedLocals(std::vector<FStmtIR>& Stmts, std::vector<FPropertyDef>& Locals);
    bool HoistReadsInList(std::vector<FStmtIR>& Stmts, FBlueprintClass& BP,
                          std::vector<FPropertyDef>& Locals, std::string* Err);
    bool HoistReadsInStmt(FStmtIR& St, FBlueprintClass& BP,
                          std::vector<FPropertyDef>& Locals,
                          std::vector<FStmtIR>& OutPre, std::string* Err);
    bool HoistReadsInArg(FArgIR& A, FBlueprintClass& BP,
                         std::vector<FPropertyDef>& Locals,
                         std::vector<FStmtIR>& OutPre, std::string* Err);
    bool HoistReadCall(FArgIR& A, const FReadViewSpec& V, FBlueprintClass& BP,
                       std::vector<FPropertyDef>& Locals,
                       std::vector<FStmtIR>& OutPre, std::string* Err);
    bool HoistRefAt(FArgIR& A, FBlueprintClass& BP, std::vector<FPropertyDef>& Locals,
                    std::vector<FStmtIR>& OutPre, std::string* Err);
    bool HoistBranch(FArgIR& A, FBlueprintClass& BP, std::vector<FPropertyDef>& Locals,
                     std::vector<FStmtIR>& OutPre, std::string* Err);

    /* The native UFunction Method overrides, or null; InheritedFlags gets the flags it passes on, also
       for a method implementing an interface's function, which has no Super. */
    uint32 ModMethodFlags(const FRecord& Owner, const std::string& Method, FBlueprintClass& BP);
    FIndex FindEvent(FBlueprintClass& BP, const std::string& FromRecord, const std::string& Method,
                     uint32* InheritedFlags, bool bFlagsOnly = false);

    /* Records are keyed by qualified name; Bare holds only leaf names exactly one class claims,
       so an ambiguous bare name fails instead of picking the last class collected. */
    /* The engine's name for the member or function C++ spells `Cpp`, as seen from R: R's own marker, else an
       ancestor's or an interface's. A mod's own names have no marker and are their C++ spelling; Dumper-7 keeps a
       spelling unique along a class chain, so the first marker found is the one. */
    std::string UeNameOf(const FRecord* R, const std::string& Cpp, int Depth = 0) const
    {
        for (; R && Depth < 64; R = R->Base.empty() ? nullptr : Find(R->Base), ++Depth)
        {
            if (auto It = R->UeNames.find(Cpp); It != R->UeNames.end()) return It->second;
            for (const std::string& I : R->Interfaces)
                if (const FRecord* IR = Find(I); IR && IR != R)
                    if (std::string Ue = UeNameOf(IR, Cpp, Depth + 1); Ue != Cpp) return Ue;
        }
        return Cpp;
    }
    /* And back, within one record: the C++ spelling of the function the engine calls `Ue`. */
    static std::string CppNameOf(const FRecord& R, const std::string& Ue)
    {
        for (const auto& N : R.UeNames) if (N.second == Ue) return N.first;
        return Ue;
    }

    const FRecord* Find(const std::string& CppName) const
    {
        auto It = Records.find(CppName);
        if (It != Records.end()) return &It->second;
        auto B = Bare.find(CppName);
        if (B != Bare.end()) { It = Records.find(B->second); return It == Records.end() ? nullptr : &It->second; }
        /* `using JSONValue_C = Game::_AssemblyStorm::Common::JSON::JSONValue_C;` - how a mod names a class two
           packages both have, which the headers can give no short name. clang spells a use as the alias. */
        auto A = Aliases.find(CppName);
        return A == Aliases.end() || A->second == CppName ? nullptr : Find(A->second);
    }

    Json Doc;
    bool LoadTables(const std::string& IncludeDir, std::string* Err);
    /* The name Conv.json / Ops.json / Types.json use for a clang type: keywords, references and
       spelling variants dropped, an enum is its underlying byte, any object pointer is UObject. */
    std::string Canon(std::string T) const;
    bool ZeroArg(const std::string& Type, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    FIndex ClassImportOf(const FRecord& R, FBlueprintClass& BP) const;
    bool LowerStructLiteral(const Json& CtorNode, const FStructInfo& SI, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    std::vector<FConv> Convs;
    std::vector<FOpInfo> Ops;
    std::map<std::string, FEnumInfo> Enums;
    std::map<std::string, FStructInfo> Structs;
    std::map<std::string, int64> EnumValues;          // clang EnumConstantDecl id -> value
    std::map<std::string, std::vector<std::pair<std::string, int64>>> EnumDecls;   // C++ name -> its enumerators, in order
    std::map<std::string, int32> EnumConstWidth;      // clang EnumConstantDecl id -> its enum's size, 1 / 4 / 8
    std::map<std::string, uint32> EventFlags;         // UeApi/Events.json: "Package.Class.Function" -> EFunctionFlags
    std::map<std::string, FIndex> CurSignatures;      // Generate: dispatcher name -> its signature function export
    const FConv* FindConv(const std::string& From, const std::string& To) const;
    const FOpInfo* FindOp(const std::string& Op, const std::string& Lhs, const std::string& Rhs) const;
    void ApplyConv(const FConv& C, FBlueprintClass& BP, FArgIR& Arg);
    std::string ModPackage;
    std::string ApiDir;         // set by `--api`: where the uncooked editor-side stubs go
    std::string SourceDir;      // the compiled .cpp's folder: what __EmbedFile__ resolves a relative path against
    /* A container inside a container: UE has no such property, so the inner one is the single member (Value) of a
       wrapper struct, <wrapper name> -> the container type. The wrapper has the container's layout. */
    std::map<std::string, std::string> NestedWrappers;
    static constexpr const char* NestedPackage = "/Game/_ElytrasMods/_NestedContainerStructs";
    bool NestedWrapperOut(const std::string& ContainerType, size_t OutArg, const std::string& ResultType, FStmtIR Copy,
                          FBlueprintClass& BP, FArgIR& Call, std::string* Err);
    std::string NestedWrapper(const std::string& ContainerType)
    {
        /* clang spells one type int or int32 depending on where it comes from: one wrapper for both. */
        std::string Name = "FNC_", Word;
        auto Flush = [&]() { Name += Word == "int32" ? "int" : Word == "long" ? "int64" : Word; Word.clear(); };
        for (char C : StripTypeKeywords(ContainerType) + " ")
            if (std::isalnum(uint8(C)) || C == '_') Word += C;
            else
            {
                if (Word == "long" && Name.size() >= 5 && Name.compare(Name.size() - 5, 5, "int64") == 0) Word.clear();
                Flush();
                if (C == '<' || C == ',') Name += '_';
                else if (C == '*') Name += "Ptr";
            }
        NestedWrappers.emplace(Name, StripTypeKeywords(ContainerType));
        return Name;
    }
    FIndex NestedWrapperImport(const std::string& ContainerType, FBlueprintClass& BP)
    {
        const std::string Name = NestedWrapper(ContainerType);
        return BP.ScriptStruct(std::string(NestedPackage) + "/" + Name, Name);
    }
    bool GenerateNestedWrappers(const std::string& OutDir, std::string* Err);
    std::map<std::string, FRecord> Records;
    std::map<std::string, std::string> MethodOwner;   // clang decl id -> owning record
    std::map<std::string, std::string> FieldOwner;    // clang decl id -> declaring record
    std::map<std::string, std::string> Bare;          // unambiguous leaf name -> qualified name
    std::map<std::string, std::string> Aliases;       // a namespace-scope `using A = B;` / typedef: A -> B
    const FRecord* Cur = nullptr;                     // record Generate is working on
    std::set<std::string> CurrentOutParms;            // T& parm names of the function being lowered
    bool bCurNet = false;                             // the function being lowered is an RPC
    bool bCurNoOpt = false;                           // ... is UE_NO_OPTIMIZE: no drops, no && / || fold
    const Json* CurBody = nullptr;                    // the body LowerBody is walking: scope of the locals it declares
    std::set<std::string> WarnedRefParms;             // its reference parameters already warned about

    /* A write through an RPC's reference parameter: the receiving side gets a copy of the argument, so the caller sees
       the change only when the call ran locally (a server calling its own Server RPC). Warned once per parameter. */
    void WarnRpcRefWrite(const FArgIR& Written)
    {
        const FArgIR* Root = &Written;
        while ((Root->K == FArgIR::Index || Root->K == FArgIR::Member) && Root->Base) Root = Root->Base.get();
        if (!bCurNet || Root->K != FArgIR::LocalOut || !WarnedRefParms.insert(Root->S).second) return;
        printf("  warning: %s::%s: modifying reference parameter %s of an RPC reaches the caller only when the call runs "
               "locally; take it by value or const&\n", Cur ? Cur->CppName.c_str() : "", CurFnName.c_str(), Root->S.c_str());
    }
    std::string CurrentWco;                           // its WorldContext* parm when it is a static, else empty
    std::vector<FRegistryAsset> RegistryRows;
    std::vector<const Json*> AssetDecls;        // namespace-scope variables brace-initialized, see GenerateAsset
    std::map<std::string, std::string> AssetPaths;  // UE_ASSET_AT: variable -> the /Game package of an asset cooked elsewhere
    /* `&MD_Big`, where MD_Big is an asset of this mod or a UE_ASSET_AT: its import. False when N is anything else. */
    bool AssetRef(const Json& N, FBlueprintClass& BP, FIndex* Out);
    /* A member initializer becomes PD.Default, which the CDO / struct default instance / asset writes. It must be a
       literal the member's own type can hold (optionally negated), nullptr, an argless ctor, `&Asset`, or a braced
       list of those for a TArray / TSet, of { key, value } pairs for a TMap. Init overrides F's own initializer. */
    bool LowerDefault(const Json& F, FPropertyDef& PD, FBlueprintClass& BP, std::string* Err, const Json* Init = nullptr,
                      bool bKeepZero = false);
    std::map<std::string, std::vector<std::pair<std::string, int64>>> ModEnums;  // UE_ENUM cooked here: enumerators

    /* Per-function state reset in Generate: whether this function needs the FDeref scratch
       local (and its Num=1 prologue) and the counter that names each hoisted temp. */
    bool ReadScratchAdded = false;
    int32 ReadTmpCounter = 0;
    int32 LoopDepth = 0;                              // LowerBody: the loops around the statement being lowered
    std::string CurFnName;                            // Generate: the method being lowered
    std::string LatentRefusal;                        // why that method cannot make a latent call, or empty
    bool bMadeLatentCall = false;                     // LowerCall: it made one, so it moves into the ubergraph
    int32 LatentCount = 0;
    /* A generated event a latent call's completion delegate binds: it stores its parameter into the frame local
       the call's value is read from after the resume. */
    struct FCompletion
    {
        std::string Event;
        std::vector<FPropertyDef> Parms;
        std::string Local;                            // where Parms[0] is stored, or empty
        std::shared_ptr<int32> Resume;                // an await's: the event then re-enters the ubergraph here
    };
    std::string FreshEventName(const std::string& Stem);
    bool LowerAwait(const Json& CallNode, FBlueprintClass& BP, FCallIR& Out, std::string* Err);
    std::vector<FCompletion> Completions;             // the method being lowered's
    std::set<std::string> GeneratedEvents;            // the class's, so two never share a name
    std::set<std::string> ActivatedActions;           // the method's variables an await already activated
    int32 SwitchDepth = 0;                            // LowerBody: the switches around it
    std::map<std::string, int32> GotoLabels;          // the body being lowered's: clang LabelDecl id -> FStmtIR::LabelId
    int32 NextGotoLabel = 0;                          // never reset: latent functions share one ubergraph script
    bool bBodyHasGoto = false;                        // a goto can re-reach any declaration, as a loop does
    int32 WriteBackDepth = 0;                         // LowerBody: the TMap range-fors around it that write the value back
    int32 GotoLabelOf(const std::string& DeclId)
    {
        const auto It = GotoLabels.find(DeclId);
        return It != GotoLabels.end() ? It->second : (GotoLabels[DeclId] = NextGotoLabel++);
    }
};

/* The innermost loop's forward jumps, patched once its exit / continue offsets are known. */
struct FLoopPatches
{
    std::vector<int32> Breaks, Continues;
};

/* Returns: the innermost inline Block's forward jumps to its end. */
void EmitStmts(const std::vector<FStmtIR>& Stmts, FScript& S, FIndex SelfExp, FLoopPatches* Loop = nullptr,
               std::vector<int32>* Returns = nullptr)
{
    for (const FStmtIR& St : Stmts)
    {
        switch (St.K)
        {
        case FStmtIR::Assign:
        {
            if (St.Var.K == FArgIR::Call && St.Var.Sub && St.Var.Sub->Intrinsic == "__RefAtInline__")
            {
                /* `*P = V`: EX_Let writes the value into whatever address the destination leaves (ScriptCore.cpp
                   execLet). LetBool / LetObj would cast the destination property, which a view's never is. */
                S.LetPath(EX_Let, { St.Var.Sub->View, St.Var.Sub->View }, St.Var.Sub->Extra,
                          [St, SelfExp](FScript& V) { EmitArg(V, St.Var, SelfExp, nullptr); },
                          [St, SelfExp](FScript& V) { EmitArg(V, St.Value, SelfExp, nullptr); });
                break;
            }
            /* Let's property path: the variable itself, or an array element as {element, array}; the
               path's owner is the declaring class for a Field, the function for a local. */
            const FArgIR& Top = St.Var.K == FArgIR::Index ? *St.Var.Base : St.Var;
            const std::vector<std::string> Path = St.Var.K == FArgIR::Index
                ? std::vector<std::string>{ Top.S, Top.S } : std::vector<std::string>{ Top.S };
            const bool bLocalDest = Top.K == FArgIR::Local || Top.K == FArgIR::LocalOut;
            const FIndex VarOwner = bLocalDest ? SelfExp : Top.Owner;
            S.LetPath(St.Var.LetOp, Path, VarOwner,
                      [St, SelfExp](FScript& V) { EmitArg(V, St.Var, SelfExp, nullptr); },
                      [St, SelfExp](FScript& V) { EmitArg(V, St.Value, SelfExp, nullptr); });
            break;
        }

        case FStmtIR::StaticCall:
            EmitCall(S, St.Call, SelfExp, nullptr);
            break;

        case FStmtIR::Decl:
            /* No initialiser emits nothing: the frame already zeroed the slot. */
            if (St.bHasValue)
                S.Let(St.Var.LetOp, St.Var.S, SelfExp,
                      [St, SelfExp](FScript& V) { EmitArg(V, St.Var, SelfExp, nullptr); },
                      [St, SelfExp](FScript& V) { EmitArg(V, St.Value, SelfExp, nullptr); });
            break;

        case FStmtIR::Return:
            if (St.bHasValue)
                S.Return([St, SelfExp](FScript& V) { EmitArg(V, St.Value, SelfExp, nullptr); });
            else
                S.Return();
            break;

        case FStmtIR::If:
        {
            const int32 NotPatch = S.JumpIfNot(0,
                [St, SelfExp](FScript& C) { EmitArg(C, St.Cond, SelfExp, nullptr); });
            if (St.Then) EmitStmts(*St.Then, S, SelfExp, Loop, Returns);
            if (St.Else && !St.Else->empty())
            {
                const int32 EndPatch = S.Jump(0);
                S.PatchJumpTarget(NotPatch, S.MemorySize());
                EmitStmts(*St.Else, S, SelfExp, Loop, Returns);
                S.PatchJumpTarget(EndPatch, S.MemorySize());
            }
            else
            {
                S.PatchJumpTarget(NotPatch, S.MemorySize());
            }
            break;
        }

        case FStmtIR::While:
        {
            /* `break` and `continue` are forward EX_Jumps patched here; no flow stack is involved, so a
               `return` from inside the loop leaves nothing behind. */
            FLoopPatches Inner;
            const int32 Head = S.MemorySize();
            int32 ExitPatch = -1;
            auto Test = [&] {
                ExitPatch = S.JumpIfNot(0, [St, SelfExp](FScript& C) { EmitArg(C, St.Cond, SelfExp, nullptr); });
            };
            if (!St.bPostTest) Test();
            if (St.Body) EmitStmts(*St.Body, S, SelfExp, &Inner, Returns);
            for (int32 P : Inner.Continues) S.PatchJumpTarget(P, S.MemorySize());
            if (St.Inc) EmitStmts(*St.Inc, S, SelfExp, Loop, Returns);
            if (St.bPostTest) Test();               // do/while: `continue` lands on the test, as in C++
            S.Jump(Head);
            if (St.Trailer && !Inner.Breaks.empty())
            {
                /* A `break` runs the trailer and falls into the exit; a failed condition skips it. */
                for (int32 P : Inner.Breaks) S.PatchJumpTarget(P, S.MemorySize());
                Inner.Breaks.clear();
                EmitStmts(*St.Trailer, S, SelfExp, Loop, Returns);
            }
            S.PatchJumpTarget(ExitPatch, S.MemorySize());
            for (int32 P : Inner.Breaks) S.PatchJumpTarget(P, S.MemorySize());
            break;
        }

        case FStmtIR::Break:
        case FStmtIR::Continue:
            if (Loop) (St.K == FStmtIR::Break ? Loop->Breaks : Loop->Continues).push_back(S.Jump(0));
            break;

        case FStmtIR::Switch:
        {
            /* The case values split into runs, then the body in order, so a case with no `break` falls through.
               A run of 3+ cases whose range is at most 4x its case count (int32 / uint8 only) is a jump table: a
               bounds check that skips to the next run, then EX_ComputedJump to Table + (Value - Min) * 5, a table
               of 5-byte EX_Jumps, the way the ubergraph entry dispatches. A 5-byte slot against a ~30-byte compare
               keeps a hole-y table smaller than the chain. The other cases are one JumpIfNot(Value != Case) each,
               straight to their label. A miss goes to `default`, or past the body.
               ponytail: runs are tried in order, a binary search over their bounds if switches grow many runs. */
            const size_t NumCases = St.CaseTests.size();
            struct FRun { int64 Min, Max; std::vector<size_t> Cases; };
            std::vector<FRun> Runs;
            std::vector<size_t> Singles;
            if (St.SwitchWidth == 1 || St.SwitchWidth == 4)
            {
                std::vector<size_t> Order(NumCases);
                for (size_t I = 0; I < NumCases; ++I) Order[I] = I;
                std::sort(Order.begin(), Order.end(), [&](size_t L, size_t R) { return St.CaseValues[L] < St.CaseValues[R]; });
                for (size_t I : Order)
                {
                    const int64 V = St.CaseValues[I];
                    if (!Runs.empty() && V - Runs.back().Min + 1 <= int64(Runs.back().Cases.size() + 1) * 4 && V - Runs.back().Min < 1024)
                    { Runs.back().Max = V; Runs.back().Cases.push_back(I); }
                    else Runs.push_back({ V, V, { I } });
                }
                for (auto It = Runs.begin(); It != Runs.end();)
                    if (It->Cases.size() < 3) { Singles.insert(Singles.end(), It->Cases.begin(), It->Cases.end()); It = Runs.erase(It); }
                    else ++It;
                std::sort(Singles.begin(), Singles.end());      // source order, as the chain always was
            }
            else
                for (size_t I = 0; I < NumCases; ++I) Singles.push_back(I);

            FLoopPatches Inner;
            std::vector<int32> LabelAt(NumCases + 1, -1);
            std::vector<std::pair<int32, size_t>> ToLabel;              // a patch, and the case it jumps to
            std::vector<std::pair<int32, int64>> TableEntries;          // a table slot, and its value
            int32 Miss = 0;
            FArgIR Value = St.SwitchValue;
            if (!Runs.empty() && St.SwitchWidth == 1)
            {
                FArgIR Byte = Value;
                Value = FArgIR();
                Value.K = FArgIR::Call;
                Value.Sub = std::make_shared<FCallIR>();
                Value.Sub->Fn = St.Call.Fn;             // Conv_ByteToInt, resolved when lowering
                Value.Sub->Args = { Byte };
            }
            auto Int = [](int64 V) { FArgIR A; A.K = FArgIR::Int; A.I = int32(V); return A; };
            auto Bool = [](bool V) { FArgIR A; A.K = FArgIR::Bool; A.B = V; return A; };
            for (const FRun& Run : Runs)
            {
                FArgIR InRange;
                InRange.K = FArgIR::Call;
                InRange.Sub = std::make_shared<FCallIR>();
                InRange.Sub->Fn = St.Call.Extra;        // InRange_IntInt
                InRange.Sub->Args = { Value, Int(Run.Min), Int(Run.Max), Bool(true), Bool(true) };
                const int32 NextRun = S.JumpIfNot(0, [&InRange, SelfExp](FScript& C) { EmitArg(C, InRange, SelfExp, nullptr); });

                /* Offset = Value * 5 + (Table - Min * 5); the constant is patched once the table's offset is known. */
                int32 BaseAt = 0;
                S.ComputedJump([&](FScript& C) {
                    C.CallMath(St.Call.Extra2);         // Add_IntInt
                    C.CallMath(St.Target.Fn);           // Multiply_IntInt
                    EmitArg(C, Value, SelfExp, nullptr);
                    C.IntConst(5);
                    C.EndFunctionParms();
                    C.Op(EX_IntConst);
                    BaseAt = C.StorageSize();
                    C.RawInt32(0);
                    C.EndFunctionParms();
                });
                S.PatchJumpTarget(BaseAt, int32(S.MemorySize() - Run.Min * 5));
                for (int64 V = Run.Min; V <= Run.Max; ++V) TableEntries.push_back({ S.Jump(0), V });
                S.PatchJumpTarget(NextRun, S.MemorySize());
            }
            for (size_t I : Singles)
            {
                const FArgIR& Test = St.CaseTests[I];
                ToLabel.push_back({ S.JumpIfNot(0, [&Test, SelfExp](FScript& C) { EmitArg(C, Test, SelfExp, nullptr); }), I });
            }
            Miss = S.Jump(0);
            for (const FStmtIR& B : *St.Body)
            {
                if (B.K == FStmtIR::Label) { LabelAt[B.LabelId] = S.MemorySize(); continue; }
                EmitStmts({ B }, S, SelfExp, &Inner, Returns);
            }
            const int32 End = S.MemorySize();
            const int32 MissTarget = St.LabelId >= 0 ? LabelAt[St.LabelId] : End;
            S.PatchJumpTarget(Miss, MissTarget);
            for (const auto& [Patch, Case] : ToLabel) S.PatchJumpTarget(Patch, LabelAt[Case]);
            for (const auto& [Patch, V] : TableEntries)
            {
                const auto Hit = std::find(St.CaseValues.begin(), St.CaseValues.end(), V);
                S.PatchJumpTarget(Patch, Hit == St.CaseValues.end() ? MissTarget : LabelAt[Hit - St.CaseValues.begin()]);
            }
            for (int32 P : Inner.Breaks) S.PatchJumpTarget(P, End);
            if (Loop) for (int32 P : Inner.Continues) Loop->Continues.push_back(P);
            break;
        }

        case FStmtIR::Label:
            break;

        case FStmtIR::Block:
        {
            /* A loop around the call site is not the inline body's: `break` cannot cross a function. */
            std::vector<int32> Ends;
            if (St.Body) EmitStmts(*St.Body, S, SelfExp, nullptr, &Ends);
            for (int32 P : Ends) S.PatchJumpTarget(P, S.MemorySize());
            break;
        }

        case FStmtIR::InlineReturn:
            if (Returns) Returns->push_back(S.Jump(0));
            break;

        case FStmtIR::Goto:
        {
            /* The same patched EX_Jump `break` is: nothing sits on the flow stack, so a goto may leave any
               number of loops. A label already emitted is a backward jump, known now. */
            const auto At = S.GotoLabelAt.find(St.LabelId);
            if (At != S.GotoLabelAt.end()) S.Jump(At->second);
            else S.GotoPatches.push_back({ St.LabelId, S.Jump(0) });
            break;
        }

        case FStmtIR::GotoLabel:
            S.GotoLabelAt[St.LabelId] = S.MemorySize();
            for (const auto& [Label, Patch] : S.GotoPatches)
                if (Label == St.LabelId) S.PatchJumpTarget(Patch, S.MemorySize());
            break;
        }
        /* After a latent call this run of the ubergraph ends; the latent action re-enters right here. BP_LiftPod
           ends it with EX_PopExecutionFlow onto a pushed final return, which is the same thing. */
        if (!S.LatentResumes.empty() || !S.ResumeSinks.empty())
        {
            S.Return();
            for (int32 At : S.LatentResumes) S.PatchJumpTarget(At, S.MemorySize());
            for (const std::shared_ptr<int32>& Sink : S.ResumeSinks) *Sink = S.MemorySize();
            S.LatentResumes.clear();
            S.ResumeSinks.clear();
        }
    }
}

/* Renames the locals a list of statements names (a latent function's, moving into the ubergraph frame). Lowered
   trees share nodes, so each one is renamed once. */
struct FLocalRenamer
{
    const std::map<std::string, std::string>& To;
    std::set<const void*> Seen;

    void Name(std::string& N) { if (auto It = To.find(N); It != To.end()) N = It->second; }
    void Arg(FArgIR& A)
    {
        if (!Seen.insert(&A).second) return;
        if (A.K == FArgIR::Local || A.K == FArgIR::LocalOut
            || (A.K == FArgIR::Call && A.Sub && A.Sub->Intrinsic == "__RefAtInline__")) Name(A.S);
        if (A.Sub) Call(*A.Sub);
        if (A.Base) Arg(*A.Base);
    }
    void Call(FCallIR& C)
    {
        if (!Seen.insert(&C).second) return;
        for (FArgIR& A : C.Args) Arg(A);
        if (C.Target) Arg(*C.Target);
        if (C.Inline) List(*C.Inline);
        Name(C.InlineResult);
    }
    void List(std::vector<FStmtIR>& Stmts)
    {
        if (!Seen.insert(&Stmts).second) return;
        for (FStmtIR& St : Stmts)
        {
            Call(St.Target);
            Call(St.Call);
            for (FArgIR* A : { &St.Var, &St.Value, &St.Cond, &St.SwitchValue }) Arg(*A);
            for (FArgIR& A : St.CaseTests) Arg(A);
            for (auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer }) if (*L) List(**L);
        }
    }
};

/* A Blueprint UE_CLASS package must be the asset path (/Game/Mods/Lib/Lib), not its folder: the
   folder form names a package that does not exist, and the first symptom is a null UFunction at call time. */
void BadClassMeta(const FRecord& R, std::string* Err, bool* bOk)
{
    if (!*bOk || R.UePackage.compare(0, 6, "/Game/") != 0) return;
    if (R.UeName.size() < 3 || R.UeName.compare(R.UeName.size() - 2, 2, "_C") != 0) return;

    const size_t Slash = R.UePackage.rfind('/');
    const std::string Asset = Slash == std::string::npos ? R.UePackage : R.UePackage.substr(Slash + 1);
    if (Asset + "_C" == R.UeName) return;

    *Err = "UE_CLASS on " + R.CppName + " names the package \"" + R.UePackage + "\", which does"
           " not end in the asset that declares " + R.UeName + ". Did you mean \"" + R.UePackage
         + "/" + R.UeName.substr(0, R.UeName.size() - 2) + "\"?";
    *bOk = false;
}

bool FCompiler::Collect(std::string* Err)
{
    bool bMetaOk = true;
    std::set<std::string> Ambiguous;
    std::map<std::string, std::string> EnumUnderlying;
    std::vector<std::pair<std::string, std::string>> EnumMarks;      // enum, owning mod ("" for this one)

    /* UeApi headers put Blueprint classes in namespaces mirroring their /Game path. */
    std::function<void(const Json&, const std::string&)> Walk =
        [&](const Json& Scope, const std::string& Ns)
    {
    ForEach(Scope, [&](const Json& N) {
        if (Kind(N) == "NamespaceDecl" && N.contains("name"))
        {
            Walk(N, Ns + Name(N) + "::");
            return;
        }
        if ((Kind(N) == "TypeAliasDecl" || Kind(N) == "TypedefDecl") && N.contains("name"))
            Aliases[Ns + Name(N)] = Aliases[Name(N)] = StripTypeKeywords(TypeOf(N));
        if (Kind(N) == "VarDecl" && Name(N) == "UeModPackage") FindLiteral(N, ModPackage);
        if (Kind(N) == "VarDecl" && BracedInit(N)) AssetDecls.push_back(&N);
        if (Kind(N) == "VarDecl" && Name(N).size() > 9 && Name(N).compare(Name(N).size() - 9, 9, "__UeAsset") == 0)
        {
            FindLiteral(N, AssetPaths[Name(N).substr(0, Name(N).size() - 9)]);
            return;
        }
        if (Kind(N) == "EnumDecl")
        {
            int64 Next = 0;
            auto& Mine = EnumDecls[Ns + N.value("name", std::string())];
            Mine.clear();
            ForEach(N, [&](const Json& C) {
                if (Kind(C) != "EnumConstantDecl") return;
                const Json* V = First(C);
                if (V && V->contains("value")) Next = std::stoll((*V)["value"].get<std::string>());
                Mine.push_back({ Name(C), Next });
                EnumValues[C.value("id", std::string())] = Next++;
            });
            const Json& U = N.contains("fixedUnderlyingType") ? N["fixedUnderlyingType"] : Json::object();
            const std::string Under = U.value("desugaredQualType", U.value("qualType", std::string()));
            const std::string Canon = Under == "unsigned char" || Under == "uint8" ? "uint8"
                                    : Under == "int" || Under == "int32" ? "int32"
                                    : Under == "long long" || Under == "int64" ? "int64" : Under;
            EnumUnderlying[Ns + N.value("name", std::string())] = Canon;
            const int32 Width = Under.empty() || Canon == "uint8" ? 1 : Canon == "int64" ? 8 : 4;
            ForEach(N, [&](const Json& C) { if (Kind(C) == "EnumConstantDecl") EnumConstWidth[C.value("id", std::string())] = Width; });
            return;
        }
        if (Kind(N) == "VarDecl" && N.contains("name") && Name(N).size() > 8 && Name(N).compare(Name(N).size() - 8, 8, "__UeEnum") == 0)
        {
            std::string Owner;
            FindLiteral(N, Owner);
            EnumMarks.push_back({ Ns + Name(N).substr(0, Name(N).size() - 8), Owner });
            return;
        }
        /* Out-of-line method definition: previousDecl points at the in-class decl, already collected. */
        if (Kind(N) == "CXXMethodDecl" && N.contains("previousDecl") && N.contains("inner"))
        {
            const std::string PrevId = N.value("previousDecl", std::string());
            auto OwnerIt = MethodOwner.find(PrevId);
            if (OwnerIt == MethodOwner.end()) return;
            auto RecIt = Records.find(OwnerIt->second);
            if (RecIt == Records.end()) return;
            RecIt->second.MethodDefs[Name(N)] = &N;
            MethodOwner[N.value("id", std::string())] = OwnerIt->second;
            return;
        }
        if (Kind(N) != "CXXRecordDecl" || !N.contains("name") || !N.contains("inner")) return;

        FRecord R;
        R.CppName = Ns + Name(N);
        if (auto SI = Structs.find(Name(N)); Ns.empty() && SI != Structs.end())
        {
            R.UePackage = SI->second.Package;
            R.UeName = SI->second.UeName;
            R.bIsStruct = true;
        }
        /* `class Foo : public AActor, public ITargetable`: the UE parent first, then implemented interfaces. */
        auto Bases = N.find("bases");
        if (Bases != N.end())
            for (const Json& B : *Bases)
            {
                const std::string T = B["type"].value("qualType", std::string());
                if (R.Base.empty()) R.Base = T;
                else R.Interfaces.push_back(T);
            }

        uint32 Access = N.value("tagUsed", std::string()) == "struct" ? FUNC_Public : FUNC_Private;
        ForEach(N, [&](const Json& C) {
            if (Kind(C) == "AccessSpecDecl")
            {
                const std::string A = C.value("access", std::string());
                Access = A == "public" ? FUNC_Public : A == "protected" ? FUNC_Protected : FUNC_Private;
            }
            if (Kind(C) == "VarDecl" && Name(C) == "UeClassMeta")
            {
                std::string Meta;
                if (FindLiteral(C, Meta))
                {
                    const size_t Colon = Meta.find(':');
                    if (Colon != std::string::npos)
                    {
                        R.UePackage = Meta.substr(0, Colon);
                        R.UeName = Meta.substr(Colon + 1);
                    }
                    BadClassMeta(R, Err, &bMetaOk);
                }
            }
            else if (Kind(C) == "VarDecl" && Name(C) == "UeInterfaceMeta")
            {
                R.bIsInterface = true;
            }
            else if (Kind(C) == "VarDecl" && Name(C) == "UeStructMeta")
            {
                R.bIsStruct = true;
                std::string Owner;
                if (FindLiteral(C, Owner))
                {
                    R.UePackage = Owner + "/" + R.CppName;
                    R.UeName = R.CppName;
                }
            }
            else if (Kind(C) == "VarDecl" && Name(C).size() > 8 && Name(C).compare(Name(C).size() - 8, 8, "__UeName") == 0)
            {
                std::string Real;
                if (FindLiteral(C, Real)) R.UeNames[Name(C).substr(0, Name(C).size() - 8)] = Real;
            }
            else if (Kind(C) == "VarDecl" && Name(C).size() > 11 && Name(C).compare(Name(C).size() - 11, 11, "__UeScsNode") == 0)
            {
                std::string Guid;
                if (FindLiteral(C, Guid)) R.ScsNodes[Name(C).substr(0, Name(C).size() - 11)] = Guid;
            }
            else if (Kind(C) == "VarDecl" && Name(C).size() > 12 && Name(C).compare(Name(C).size() - 12, 12, "__Replicated") == 0)
            {
                std::string Spec;
                if (FindLiteral(C, Spec)) R.Replicated[Name(C).substr(0, Name(C).size() - 12)] = Spec;
            }
            else if (Kind(C) == "VarDecl" && Name(C).size() > 13
                     && Name(C).compare(Name(C).size() - 13, 13, "__UeComponent") == 0)
            {
                R.Components.insert(Name(C).substr(0, Name(C).size() - 13));
            }
            else if (Kind(C) == "CXXMethodDecl" && Name(C) == "UeDefaults__")
            {
                /* Not a UFunction: AssetGen reads its assignments as defaults, never lowers them. */
                R.Defaults = &C;
            }
            else if (Kind(C) == "CXXMethodDecl" && C.contains("name"))
            {
                /* genueapi's overload without the world context shares the name; the longer one is the UFunction. */
                const Json*& Slot = R.Methods[Name(C)];
                if (!Slot || ParmNames(C).size() > ParmNames(*Slot).size()) Slot = &C;
                MethodOwner[C.value("id", std::string())] = R.CppName;
                R.MethodAccess[Name(C)] = Access;
            }
            else if (Kind(C) == "FieldDecl" && C.contains("name"))
            {
                R.Fields.push_back(&C);
                FieldOwner[C.value("id", std::string())] = R.CppName;
                if (Access == FUNC_Private) R.PrivateFields.insert(Name(C));
            }
            else if ((Kind(C) == "TypeAliasDecl" || Kind(C) == "TypedefDecl") && C.contains("name"))
                R.TypeAliases[Name(C)] = StripTypeKeywords(TypeOf(C));
        });
        /* A set is looked up in Replicated by the name it is cooked under, so the marker's C++ key follows. */
        for (const auto& N2 : R.UeNames)
            if (auto Rep = R.Replicated.find(N2.first); Rep != R.Replicated.end())
            {
                R.Replicated[N2.second] = Rep->second;
                R.Replicated.erase(N2.first);
            }
        /* genueapi opens a Blueprint class with `using JSONValue_C = Game::...::JSONValue_C;` so its signatures stay
           readable, and a mod class deriving it writes `JSONValue_C*` through the same names. The nearer one wins. */
        std::map<std::string, std::string> InScope = R.TypeAliases;
        for (const FRecord* A = R.Base.empty() ? nullptr : Find(R.Base); A; A = A->Base.empty() ? nullptr : Find(A->Base))
            InScope.insert(A->TypeAliases.begin(), A->TypeAliases.end());
        if (!InScope.empty()) ExpandAliases(const_cast<Json&>(N), InScope);
        /* A leaf name claimed by a second class is withdrawn, not overwritten. */
        const std::string Leaf = Name(N);
        if (!Ns.empty() && !Ambiguous.count(Leaf))
        {
            auto Prev = Bare.find(Leaf);
            if (Prev == Bare.end()) Bare.emplace(Leaf, R.CppName);
            else { Bare.erase(Prev); Ambiguous.insert(Leaf); }
        }
        Records[R.CppName] = R;
    });
    };
    Walk(Doc, std::string());
    if (!bMetaOk) return false;

    if (ModPackage.empty())
    {
        *Err = "the source declares no UE_MOD_PACKAGE, so its classes have no /Game path";
        return false;
    }

    /* UE_ENUM / UE_ENUM_IN: a UserDefinedEnum at <owner>/<Name>, typed like UeApi's native enums. */
    for (const auto& [Enum, Owner] : EnumMarks)
    {
        auto D = EnumDecls.find(Enum);
        if (D == EnumDecls.end() || D->second.empty()) { *Err = "UE_ENUM(" + Enum + ") names no enum with enumerators"; return false; }
        const std::string U = EnumUnderlying[Enum];
        if (U != "uint8" && U != "int32" && U != "int64")
        { *Err = "UE_ENUM(" + Enum + "): declare it `: uint8` (a Blueprint enum), `: int32` or `: int64`"; return false; }
        const int64 Top = U == "uint8" ? 254 : U == "int32" ? int64(INT32_MAX) - 1 : INT64_MAX - 1;
        const int64 Bottom = U == "uint8" ? 0 : U == "int32" ? int64(INT32_MIN) : INT64_MIN;
        for (const auto& En : D->second)
            if (En.second < Bottom || En.second > Top)
            { *Err = "UE_ENUM(" + Enum + "): " + En.first + " is out of range (the largest value is _MAX's)"; return false; }
        const std::string Leaf = Enum.substr(Enum.rfind(':') == std::string::npos ? 0 : Enum.rfind(':') + 1);
        std::string First = D->second.front().first;
        for (const auto& En : D->second) if (En.second == 0) { First = En.first; break; }
        Enums[Enum] = { (Owner.empty() ? ModPackage : Owner) + "/" + Leaf, Leaf, U, First };
        if (Owner.empty() || Owner == ModPackage) ModEnums[Leaf] = D->second;
    }

    for (auto& It : Records)
    {
        FRecord& R = It.second;
        if (R.UePackage.empty()) continue;
        if (R.UePackage != ModPackage + "/" + R.CppName) continue;
        if (!R.bIsStruct && R.UeName != R.CppName + "_C")
        {
            *Err = "UE_CLASS on " + R.CppName + " says \"" + R.UeName
                 + "\", but cooking it here requires \"" + R.CppName + "_C\"";
            return false;
        }
        R.bIsLocal = true;
    }
    return true;
}

FIndex FCompiler::FindEvent(FBlueprintClass& BP, const std::string& FromRecord, const std::string& Method,
                            uint32* InheritedFlags, bool bFlagsOnly)
{
    /* Events.json keys a function by its package's last path segment, as Dumper-7's comments name it. */
    *InheritedFlags = 0;
    const FRecord* Self = Find(FromRecord);
    /* Events.json and the Super import know the function by the engine's name (`Set is Extruded`). */
    const std::string UeMethod = UeNameOf(Self, Method);
    auto FlagsOf = [&](const FRecord& R) {
        auto It = EventFlags.find(R.UePackage.substr(R.UePackage.rfind('/') + 1) + "." + R.UeName + "." + UeMethod);
        return It == EventFlags.end() ? uint32(0) : It->second;
    };
    for (const FRecord* R = Self; R; R = R->Base.empty() ? nullptr : Find(R->Base))
    {
        /* A mod ancestor's function is a super like a native one, and the nearest wins, as the Kismet compiler takes
           ParentClass->FindFunctionByName. What matters most is its net flags: an override of an RPC is that RPC, and
           a mismatch "will trigger an assert in Link()" (KismetCompiler.cpp:2019). An inline method is no UFunction. */
        if (R != Self && !R->IsNative() && !R->bIsInterface)
            if (auto M = R->Methods.find(Method); M != R->Methods.end() && !IsStaticDecl(*M->second) && !IsInlineMethod(*R, Method))
            {
                *InheritedFlags = ModMethodFlags(*R, Method, BP);
                return bFlagsOnly ? Null() : BP.EngineFunction(ModPackage + "/" + R->CppName, R->CppName + "_C", UeMethod);
            }
        if (R->IsNative() && R->Methods.count(Method))
        {
            *InheritedFlags = FlagsOf(*R);
            return bFlagsOnly ? Null() : BP.EngineFunction(R->UePackage, R->UeName, UeMethod);
        }
        /* Measured on BP_SentryGun_MoveMarker: an interface implementation has no Super. */
        for (const std::string& I : R->Interfaces)
            if (const FRecord* IR = Find(I); IR && IR->IsNative() && IR->Methods.count(Method))
            {
                *InheritedFlags = FlagsOf(*IR);
                return Null();
            }
    }
    return Null();      // not an override
}

/* The flags Generate gives a mod class's own method, less the ones that depend on its parameters: what an
   override of it inherits. Nothing is imported on the way. */
uint32 FCompiler::ModMethodFlags(const FRecord& Owner, const std::string& Method, FBlueprintClass& BP)
{
    const Json& Decl = *Owner.Methods.at(Method);
    const auto DefIt = Owner.MethodDefs.find(Method);
    const Json& Def = DefIt != Owner.MethodDefs.end() ? *DefIt->second : Decl;
    uint32 Inherited = 0;
    FindEvent(BP, Owner.CppName, Method, &Inherited, /*bFlagsOnly=*/true);
    uint32 Flags = Inherited ? Inherited & kOverrideInherits : FFunctionDef().FunctionFlags;
    if (IsPureDecl(Def)) Flags |= FUNC_BlueprintPure | FUNC_BlueprintCallable;
    const std::string DeclType = TypeOf(Decl);
    if (DeclType.size() > 6 && DeclType.compare(DeclType.size() - 6, 6, " const") == 0) Flags |= FUNC_Const;
    return Flags | NetFlagsOf(Decl) | NetFlagsOf(Def) | AccessFlagsOf(Decl) | AccessFlagsOf(Def);
}

/* Measured on shipped DRG classes: BP_JetBootsBurnTrigger (Actor) 0x00840814, BP_ThornsComponent
   (ThornsPerkComponent) 0x00A40814, STE_Thorns (StatusEffect) 0x00841810. Everything beyond Base is
   inherited from the native parent (CLASS_Inherit), which the dump does not carry; unknown parents
   get CLASS_HasInstancedReference since omitting it on a parent that needs it breaks instancing. */
uint32 ClassFlagsFor(const std::vector<std::string>& Ancestry)
{
    const uint32 Base = CLASS_Parsed | CLASS_ReplicationDataIsSetUp | CLASS_CompiledFromBlueprint;
    auto Derives = [&](const char* Name) {
        return std::find(Ancestry.begin(), Ancestry.end(), Name) != Ancestry.end();
    };

    if (Derives("Actor")) return Base | CLASS_Config | CLASS_HasInstancedReference;
    if (Derives("ActorComponent"))
        return Base | CLASS_Config | CLASS_HasInstancedReference | CLASS_DefaultToInstanced;
    if (Derives("BlueprintFunctionLibrary")) return Base;
    return Base | CLASS_HasInstancedReference;
}

bool TemplateArg(const std::string& T, const char* Tpl, std::string* Inner)
{
    const std::string S = StripTypeKeywords(T);
    const size_t L = strlen(Tpl);
    if (S.compare(0, L, Tpl) != 0 || S.size() < L + 2 || S[L] != '<' || S.back() != '>') return false;
    *Inner = StripTypeKeywords(S.substr(L + 1, S.size() - L - 2));
    return true;
}

EExprToken LetOpFor(const std::string& QualType)
{
    std::string Inner;
    if (QualType == "bool") return EX_LetBool;
    if (TemplateArg(QualType, "TSubclassOf", &Inner)) return EX_LetObj;
    if (IsContainerType(QualType)) return EX_Let;
    if (!QualType.empty() && QualType.back() == '*') return EX_LetObj;
    return EX_Let;
}

/* clang may canonicalise `int64` to `long long`. */
bool IsInt64Type(const std::string& QualType)
{
    return QualType.find("int64") != std::string::npos
        || QualType.find("long long") != std::string::npos
        || QualType.find("__int64") != std::string::npos;
}

bool IsObjectType(const std::string& QualType)
{
    return !QualType.empty() && QualType.back() == '*';
}

std::string MathFuncFor(const std::string& Op, const std::string& Flavour)
{
    if (Flavour == "ObjectObject")
    {
        /* KismetMathLibrary defines only ==/!= on object pointers. */
        if (Op == "==") return "EqualEqual_ObjectObject";
        if (Op == "!=") return "NotEqual_ObjectObject";
        return "";
    }
    if (Op == "+") return "Add_" + Flavour;
    if (Op == "-") return "Subtract_" + Flavour;
    if (Op == "*") return "Multiply_" + Flavour;
    if (Op == "/") return "Divide_" + Flavour;
    if (Op == "%") return Flavour == "Int64Int64" ? "" : "Percent_" + Flavour;     // no Percent_Int64Int64 in 4.27
    if (Op == "&") return "And_" + Flavour;
    if (Op == "|") return "Or_" + Flavour;
    if (Op == "^") return "Xor_" + Flavour;
    if (Op == "==") return "EqualEqual_" + Flavour;
    if (Op == "!=") return "NotEqual_" + Flavour;
    if (Op == "<") return "Less_" + Flavour;
    if (Op == ">") return "Greater_" + Flavour;
    if (Op == "<=") return "LessEqual_" + Flavour;
    if (Op == ">=") return "GreaterEqual_" + Flavour;
    return "";
}

EStrKind StrKindOf(const std::string& QualType)
{
    std::string T = QualType;
    for (const char* Prefix : { "const ", "struct ", "class " })
        if (T.compare(0, strlen(Prefix), Prefix) == 0) T = T.substr(strlen(Prefix));
    while (!T.empty() && (T.back() == ' ' || T.back() == '\t')) T.pop_back();
    if (T == "FString" || T == "char *" || T == "char*" || T.compare(0, 5, "char[") == 0
        || T.compare(0, 8, "wchar_t[") == 0 || T == "wchar_t *")     return SK_Str;
    if (T == "FName")                                                 return SK_Name;
    if (T == "FText")                                                 return SK_Text;
    if (T == "int" || T == "int32")                                   return SK_Int;
    if (IsInt64Type(T))                                               return SK_Int64;
    if (T == "float")                                                 return SK_Float;
    if (T == "bool")                                                  return SK_Bool;
    if (T == "uint8" || T == "unsigned char")                         return SK_Byte;
    if (!T.empty() && T.back() == '*')                                return SK_Object;
    return SK_None;
}

EStrKind KindOfLowered(const FArgIR& A, const std::string& InnerType)
{
    switch (A.K)
    {
    case FArgIR::Str:   return SK_Str;
    case FArgIR::Name:  return SK_Name;
    case FArgIR::Text:  return SK_Text;
    case FArgIR::Int:   return SK_Int;
    case FArgIR::Int64: return SK_Int64;
    case FArgIR::Float: return SK_Float;
    case FArgIR::Bool:  return SK_Bool;
    case FArgIR::Byte:  return SK_Byte;
    case FArgIR::DynCast: return A.CastOp == EX_ObjToInterfaceCast || A.CastOp == EX_CrossInterfaceCast
                              || A.CastOp == EX_PrimitiveCast
                                 ? StrKindOf(InnerType) : SK_Object;
    case FArgIR::Self:
    case FArgIR::ObjConst:
    case FArgIR::NullObj: return SK_Object;
    default: return StrKindOf(InnerType);
    }
}

/* A switch value's storage: 1 for uint8 / char / an enum Blueprint stores as a byte, 8 for int64, else 4. */
int32 SwitchWidth(const std::string& ClangType)
{
    const std::string T = StripTypeKeywords(ClangType);
    if (T == "uint8" || T == "unsigned char" || T == "char" || T == "int8" || T == "signed char" || T == "bool") return 1;
    if (T.compare(0, 5, "enum ") == 0 || (T.size() > 1 && T[0] == 'E' && std::isupper((unsigned char)T[1]))) return 1;
    if (T == "int64" || T == "long long" || T == "uint64" || T == "unsigned long long") return 8;
    return 4;
}

void WrapInCall(FArgIR& Arg, FIndex Fn)
{
    FArgIR Inner = Arg;
    Arg = FArgIR();
    Arg.K = FArgIR::Call;
    Arg.Sub = std::make_shared<FCallIR>();
    Arg.Sub->Fn = Fn;
    Arg.Sub->bPure = true;      // every caller wraps a Kismet conversion, comparison or arithmetic
    Arg.Sub->Args.push_back(Inner);
}

const FConv* FCompiler::FindConv(const std::string& From, const std::string& To) const
{
    for (const FConv& C : Convs)
        if (C.From == From && C.To == To) return &C;
    return nullptr;
}

const FOpInfo* FCompiler::FindOp(const std::string& Op, const std::string& Lhs, const std::string& Rhs) const
{
    for (const FOpInfo& O : Ops)
        if (O.Op == Op && O.Lhs == Lhs && O.Rhs == Rhs) return &O;
    return nullptr;
}

void FCompiler::ApplyConv(const FConv& C, FBlueprintClass& BP, FArgIR& Arg)
{
    WrapInCall(Arg, BP.EngineFunction(C.Package, C.Class, C.Fn));
    for (const Json& E : C.Extra) Arg.Sub->Args.push_back(ConstArg(E));
    Arg.InnerType = C.To;
}

std::string FCompiler::Canon(std::string T) const
{
    T = StripTypeKeywords(T);
    while (!T.empty() && (T.back() == '&' || T.back() == ' ')) T.pop_back();
    if (T.size() > 6 && T.compare(T.size() - 6, 6, "*const") == 0) T.erase(T.size() - 5);  // `const T&` of a pointer T
    T = StripTypeKeywords(T);
    const EStrKind K = StrKindOf(T);
    if (K != SK_None) return TypeNameOf(K);
    if (auto E = Enums.find(T); E != Enums.end())
        return E->second.Underlying == "int32" ? "int" : E->second.Underlying == "int64" ? "int64" : "uint8";
    std::string Inner;
    if (TemplateArg(T, "TSubclassOf", &Inner)) return TypeNameOf(SK_Object);
    for (const char* Tpl : { "TArray", "TSet", "TMap" })
        if (TemplateArg(T, Tpl, &Inner))
        {
            std::string Out = std::string(Tpl) + "<";
            for (const std::string& A : SplitTemplateArgs(Inner)) Out += (Out.back() == '<' ? "" : ", ") + Canon(A);
            return Out + ">";
        }
    return T;
}

/* Converts Arg to the slot's type: a literal folds, else a Conv_XToY from UeApi/Conv.json, else
   two of them through FString or FText. Two structs with no conversion are left alone (derived to base). */
bool FCompiler::ConvertArg(const std::string& ToType, FBlueprintClass& BP, FArgIR& Arg, std::string* Err)
{
    const std::string To = Canon(ToType);
    const EStrKind FromKind = KindOfLowered(Arg, Arg.InnerType);
    const std::string From = FromKind != SK_None ? TypeNameOf(FromKind) : Canon(Arg.InnerType);
    const EStrKind ToKind = StrKindOf(To);

    /* Addresses, raw pointers being int64 by now: nullptr is 0, an object pointer is its object's
       address, an address read back as an object pointer is the same 8 bytes, and an address is true
       when nonzero. C++ only reaches the object <-> int64 pairs through a pointer cast. */
    auto Reinterpret = [&](const char* Intrinsic, FIndex Donor, const std::string& Type) {
        FArgIR Operand = Arg;
        Arg = FArgIR();
        Arg.K = FArgIR::Call;
        Arg.InnerType = Type;
        Arg.Sub = std::make_shared<FCallIR>();
        Arg.Sub->Intrinsic = Intrinsic;
        Arg.Sub->Extra = Donor;
        Arg.Sub->Args.push_back(Operand);
    };
    if (ToKind == SK_Int64 && Arg.K == FArgIR::NullObj) { Arg.K = FArgIR::Int64; Arg.I64 = 0; Arg.InnerType = "int64"; return true; }
    if (ToKind == SK_Int64 && FromKind == SK_Object)
    {
        Reinterpret("__AddrOf__", BP.ScriptStruct("/Script/Engine", "ScreenMessageString"), "int64");
        return true;
    }
    if (ToKind == SK_Object && Arg.K == FArgIR::Int)
    {
        if (Arg.I == 0) { Arg = FArgIR(); Arg.K = FArgIR::NullObj; return true; }
        Arg.K = FArgIR::Int64;
        Arg.I64 = Arg.I;
    }
    else if (ToKind == SK_Object && FromKind == SK_Int) { *Err = "an int becomes a pointer through int64: " + ToType; return false; }
    if (ToKind == SK_Object && KindOfLowered(Arg, Arg.InnerType) == SK_Int64)
    {
        Reinterpret("__AsObject__", BP.ScriptStruct("/Script/Engine", "DebugDisplayProperty"), ToType);
        return true;
    }
    if (ToKind == SK_Bool && FromKind == SK_Object)
    {
        /* `if (Obj)` tests the object the way a Blueprint does, so a pending-kill object is false too. */
        WrapInCall(Arg, BP.EngineFunction("/Script/Engine", "KismetSystemLibrary", "IsValid"));
        Arg.InnerType = "bool";
        return true;
    }
    if (std::string TestedIface; ToKind == SK_Bool && TemplateArg(From, "TScriptInterface", &TestedIface))
    {
        /* Measured on ENE_Flea's K2Node_DynamicCast_bSuccess: EX_PrimitiveCast CST_InterfaceToBool over the
           interface value. execInterfaceToBool (ScriptCore.cpp) answers GetObject() != null. */
        FArgIR Operand = Arg;
        Arg = FArgIR();
        Arg.K = FArgIR::DynCast;
        Arg.CastOp = EX_PrimitiveCast;
        Arg.I = CST_InterfaceToBool;
        Arg.InnerType = "bool";
        Arg.Sub = std::make_shared<FCallIR>();
        Arg.Sub->Args.push_back(Operand);
        return true;
    }
    if (ToKind == SK_Bool && FromKind == SK_Int64)
    {
        WrapInCall(Arg, BP.EngineFunction("/Script/Engine", "KismetMathLibrary", "NotEqual_Int64Int64"));
        FArgIR Zero;
        Zero.K = FArgIR::Int64;
        Arg.Sub->Args.push_back(Zero);
        Arg.InnerType = "bool";
        return true;
    }

    if (To == From || From.empty() || To.empty() || ToKind == SK_Object) return true;

    std::string ToIface, FromIface;
    if (TemplateArg(To, "TScriptInterface", &ToIface))
    {
        /* Measured on ENE_Flea: an object becomes an interface through EX_ObjToInterfaceCast. */
        /* `nullptr` for an interface is EX_NoInterface, as the Kismet compiler writes a null interface literal
           (KismetCompilerVMBackend.cpp:1025): EX_NoObject would set 8 of FScriptInterface's 16 bytes. It needs no
           record of the interface, which a header may only have forward-declared. */
        if (Arg.K == FArgIR::NullObj) { Arg.CastOp = EX_NoInterface; Arg.InnerType = To; return true; }
        const bool bFromIface = TemplateArg(From, "TScriptInterface", &FromIface);
        const FRecord* IR = Find(ToIface);
        if (!IR || (!bFromIface && FromKind != SK_Object)) { *Err = "no conversion from " + From + " to " + To; return false; }
        if (bFromIface && Find(FromIface) == IR) return true;
        FArgIR Operand = Arg;
        Arg = FArgIR();
        Arg.K = FArgIR::DynCast;
        Arg.CastOp = bFromIface ? EX_CrossInterfaceCast : EX_ObjToInterfaceCast;
        Arg.Owner = ClassImportOf(*IR, BP);
        Arg.InnerType = To;
        Arg.Sub = std::make_shared<FCallIR>();
        Arg.Sub->Args.push_back(Operand);
        return true;
    }

    if (Arg.K == FArgIR::Str && !Arg.bWide && ToKind == SK_Name) { Arg.K = FArgIR::Name; return true; }
    if (Arg.K == FArgIR::Str && ToKind == SK_Text) { Arg.K = FArgIR::Text; return true; }
    if (Arg.K == FArgIR::Int && ToKind == SK_Int64) { Arg.K = FArgIR::Int64; Arg.I64 = Arg.I; return true; }
    if (Arg.K == FArgIR::Int && ToKind == SK_Float) { Arg.K = FArgIR::Float; Arg.F = float(Arg.I); return true; }
    if (Arg.K == FArgIR::Int && ToKind == SK_Bool)  { Arg.K = FArgIR::Bool;  Arg.B = Arg.I != 0; return true; }
    if (Arg.K == FArgIR::Int && ToKind == SK_Byte)  { Arg.K = FArgIR::Byte; return true; }
    if (To.compare(0, 5, "TSoft") == 0)
    {
        if (Arg.K == FArgIR::Str) { Arg.K = FArgIR::SoftPath; Arg.InnerType = To; return true; }
        if (From.compare(0, 5, "TSoft") == 0) return true;
    }

    if (const FConv* Direct = FindConv(From, To)) { ApplyConv(*Direct, BP, Arg); return true; }
    for (const char* Via : {"FString", "FText"})     // FText: int64 has no engine Conv_Int64ToString
    {
        const FConv* In  = FindConv(From, Via);
        const FConv* Out = FindConv(Via, To);
        if (In && Out) { ApplyConv(*In, BP, Arg); ApplyConv(*Out, BP, Arg); return true; }
    }
    if (Structs.count(From) && Structs.count(To)) return true;
    *Err = "no Kismet conversion from " + From + " to " + To;
    return false;
}

bool FCompiler::ZeroArg(const std::string& Type, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    const std::string T = Canon(Type);
    switch (StrKindOf(T))
    {
    case SK_Int:    Out.K = FArgIR::Int; return true;
    case SK_Int64:  Out.K = FArgIR::Int64; return true;
    case SK_Float:  Out.K = FArgIR::Float; return true;
    case SK_Bool:   Out.K = FArgIR::Bool; return true;
    case SK_Byte:   Out.K = FArgIR::Byte; return true;
    case SK_Str:    Out.K = FArgIR::Str; return true;
    case SK_Name:   Out.K = FArgIR::Name; Out.S = "None"; return true;
    case SK_Text:   Out.K = FArgIR::Text; return true;
    case SK_Object: Out.K = FArgIR::NullObj; return true;
    default: break;
    }
    auto SI = Structs.find(T);
    if (SI == Structs.end() || !SI->second.bComplete) { *Err = "no zero literal for " + Type; return false; }
    Out.K = FArgIR::StructLit;
    Out.Owner = BP.ScriptStruct(SI->second.Package, SI->second.UeName);
    Out.I = SI->second.Size;
    Out.InnerType = T;
    Out.Sub = std::make_shared<FCallIR>();
    for (const auto& F : SI->second.Fields)
    {
        FArgIR M;
        if (!ZeroArg(F.first, BP, M, Err)) return false;
        Out.Sub->Args.push_back(M);
    }
    return true;
}

/* `FVector(1, 2, 3)`: EX_StructConst wants one value per reflected field, in property order, so
   only a struct whose every field is known can be written; argless means all zeros. */
bool FCompiler::LowerStructLiteral(const Json& CtorNode, const FStructInfo& SI, FBlueprintClass& BP,
                                   FArgIR& Out, std::string* Err)
{
    const std::string T = StripTypeKeywords(TypeOf(CtorNode));
    std::vector<const Json*> Args;
    ForEach(CtorNode, [&](const Json& C) { Args.push_back(&C); });
    /* EX_StructConst cannot say a field it cannot write, so `T()` of such a struct is a Make Struct with nothing set. */
    if (!SI.bComplete && Args.empty()) return LowerMakeStruct(T, nullptr, BP, Out, Err);
    if (!SI.bComplete) { *Err = T + " has fields AssetGen cannot write, so it takes no whole-struct literal: `" + T + " V = { .Field = value };`"; return false; }
    if (Args.empty()) return ZeroArg(T, BP, Out, Err);
    if (Args.size() != SI.Fields.size())
    { *Err = T + " literal must give every field (" + std::to_string(SI.Fields.size()) + ")"; return false; }

    Out.K = FArgIR::StructLit;
    Out.Owner = BP.ScriptStruct(SI.Package, SI.UeName);
    Out.I = SI.Size;
    Out.InnerType = T;
    Out.Sub = std::make_shared<FCallIR>();
    for (const Json* A : Args)
    {
        FArgIR M;
        if (!LowerArg(*A, BP, M, Err)) return false;
        Out.Sub->Args.push_back(M);
    }
    return true;
}

/* `T()` of a struct EX_StructConst cannot write whole (a third of the dump: weak pointers, delegates, bitfields), and
   any braced aggregate, `T{ .Time = 0.5f }`. This is the editor's Make Struct node (FKCHandler_MakeStruct): a temp the
   frame default-constructs, then one member store per value given. What the braces leave out keeps the struct's own
   default, which EX_StructConst's zeros cannot say (FHitResult::Time is 1). List is the InitListExpr, one inner per
   field in declaration order, or null for none given. */
bool FCompiler::LowerMakeStruct(const std::string& Type, const Json* List, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    const FRecord* R = Find(Type);
    if (!R || !R->bIsStruct) { *Err = "a braced value needs a struct type, not " + Type; return false; }
    if (!CurLocals) { *Err = "internal: a struct value outside a function body"; return false; }

    const std::string Tmp = "__Make" + std::to_string(ReadTmpCounter++) + "__";
    FPropertyDef PD;
    if (!TypeToProperty(Type, Tmp, 0, "a " + Type + " value", BP, &PD, Err)) return false;
    PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
    CurLocals->push_back(PD);

    const bool bPlainName = R->IsNative() || IsInternalViewStruct(R->CppName);
    const FIndex Struct = R->IsNative() ? BP.ScriptStruct(R->UePackage, R->UeName)
                                        : BP.ScriptStruct(ModPackage + "/" + R->CppName, R->CppName);
    auto Body = std::make_shared<std::vector<FStmtIR>>();
    size_t I = 0;
    bool bOk = true;
    if (List)
        ForEach(*List, [&](const Json& Value) {
            const size_t At = I++;
            if (!bOk || IsUnsetInit(Value)) return;
            if (At >= R->Fields.size()) { *Err = Type + " has no member for value " + std::to_string(At + 1); bOk = false; return; }
            const Json& F = *R->Fields[At];
            FStmtIR Set;
            Set.K = FStmtIR::Assign;
            Set.Var.K = FArgIR::Member;
            Set.Var.S = bPlainName ? UeNameOf(R, Name(F)) : ModFieldName(ModPackage + "/" + R->CppName, Name(F));
            Set.Var.Owner = Struct;
            Set.Var.LetOp = LetOpFor(TypeOf(F));
            Set.Var.Base = std::make_shared<FArgIR>();
            Set.Var.Base->K = FArgIR::Local;
            Set.Var.Base->S = Tmp;
            bOk = LowerArg(Value, BP, Set.Value, Err);
            if (bOk) Body->push_back(std::move(Set));
        });
    if (!bOk) return false;

    auto Block = std::make_shared<std::vector<FStmtIR>>(1);
    (*Block)[0].K = FStmtIR::Block;
    (*Block)[0].Body = Body;
    Out.K = FArgIR::Call;
    Out.InnerType = Type;
    Out.Sub = std::make_shared<FCallIR>();
    Out.Sub->Intrinsic = "__Inline__";
    Out.Sub->Inline = Block;
    Out.Sub->InlineResult = Tmp;
    Out.Sub->InlineType = Type;
    return true;
}

bool FCompiler::LowerField(const Json& MemberNode, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    auto It = FieldOwner.find(MemberNode.value("referencedMemberDecl", std::string()));
    if (It == FieldOwner.end()) { *Err = "access to an unknown property: " + Name(MemberNode); return false; }
    const FRecord* R = Find(It->second);
    if (!R) { *Err = "access to a property of an unknown class: " + Name(MemberNode); return false; }

    const Json* ObjRaw = First(MemberNode);
    if (R->bIsStruct)
    {
        if (!ObjRaw) { *Err = "struct member access with no object: " + Name(MemberNode); return false; }
        Out.K = FArgIR::Member;
        Out.S = (R->IsNative() || IsInternalViewStruct(R->CppName))
                    ? UeNameOf(R, Name(MemberNode))
                    : ModFieldName(ModPackage + "/" + R->CppName, Name(MemberNode));
        Out.Owner = R->IsNative() ? BP.ScriptStruct(R->UePackage, R->UeName)
                                  : BP.ScriptStruct(ModPackage + "/" + R->CppName, R->CppName);
        Out.LetOp = LetOpFor(TypeOf(MemberNode));
        Out.Base = std::make_shared<FArgIR>();
        /* `P->X` and `(*P).X`: StructMember offsets into the memory P points at. The base is only offset into,
           never copied, so the int64 view serves a struct of any size. */
        const Json* Bare = PeelLvalue(ObjRaw);
        const bool bArrow = MemberNode.value("isArrow", false);
        if (bArrow || IsDerefLvalue(*Bare))
        {
            if (R->IsModStruct())
            {
                int32 Size = 0, Align = 0;
                if (!StructLayout(*R, &Size, &Align, Err)) return false;
                KeepStructLoaded(Out.Owner, R->CppName, Size);
            }
            FArgIR Addr;
            std::string Pointee;
            return (bArrow ? LowerArg(*ObjRaw, BP, Addr, Err) : LowerAddress(*Bare, BP, Addr, &Pointee, Err))
                && RefThrough(std::move(Addr), "int64", *Out.Base, Err);
        }
        return LowerArg(*ObjRaw, BP, *Out.Base, Err);
    }

    if (R->bIsInterface)
    {
        /* Kismet has no property on an interface class, so the object's own class says where it is. */
        const FRecord* Holder = InterfaceVarHolder(*R, RecordOfFieldAccess(MemberNode));
        if (!Holder)
        {
            *Err = Name(MemberNode) + " is a variable of the interface " + R->CppName + ", which holds no state itself: "
                   "read it through an object of a class that implements " + R->CppName + ", not through the interface";
            return false;
        }
        R = Holder;
    }

    /* A sibling class of this mod (a data asset's, a local parent's) is imported the way a UE_CLASS one from
       another mod is. */
    Out.K = FArgIR::Field;
    Out.S = UeNameOf(R, Name(MemberNode));      // `Name_0` is the engine's `Name`: see FRecord::UeNames
    Out.Owner = R->IsNative() ? BP.PropertyOwner(R->UePackage, R->UeName)
              : R == Cur      ? BP.ClassIndex()
                              : BP.PropertyOwner(ModPackage + "/" + R->CppName, R->CppName + "_C");
    Out.LetOp = LetOpFor(TypeOf(MemberNode));
    const Json* Obj = Strip(ObjRaw);
    if (!Obj) { *Err = "property access with no object: " + Name(MemberNode); return false; }
    if (Kind(*Obj) == "CXXThisExpr") return true;
    Out.Base = std::make_shared<FArgIR>();
    return LowerArg(*ObjRaw, BP, *Out.Base, Err);
}

/* `this, &Foo::Handler`: EX_InstanceDelegate binds the frame's object, so only self can be bound. */
bool FCompiler::LowerDelegateValue(const Json& Obj, const Json& Fn, FArgIR& Out, std::string* Err)
{
    const Json* O = Strip(&Obj);
    const Json* F = Strip(&Fn);
    if (!O || Kind(*O) != "CXXThisExpr") { *Err = "TODO: a delegate can only bind a function of `this`"; return false; }
    if (F && Kind(*F) == "UnaryOperator" && F->value("opcode", std::string()) == "&") F = Strip(First(*F));
    if (!F || Kind(*F) != "DeclRefExpr") { *Err = "a delegate binds `&Class::Function`"; return false; }
    Out.K = FArgIR::Delegate;
    Out.S = UeNameOf(Cur, (*F)["referencedDecl"].value("name", std::string()));     // found on self by name at run time
    return true;
}

/* Add / Remove / Clear on any dispatcher; Broadcast needs the signature function, known only for a
   UE_DISPATCHER of the class being generated. */
bool FCompiler::LowerDispatcherCall(const Json& Call, const Json& Callee, const Json& Obj, FBlueprintClass& BP,
                                    FArgIR& Out, std::string* Err)
{
    const std::string Method = Name(Callee);
    std::vector<const Json*> Args;
    ForEach(Call, [&](const Json& A) { Args.push_back(&A); });
    Args.erase(Args.begin());               // the callee

    Out.K = FArgIR::Call;
    Out.Sub = std::make_shared<FCallIR>();
    FCallIR& C = *Out.Sub;
    C.Args.emplace_back();
    if (!LowerArg(Obj, BP, C.Args[0], Err)) return false;
    if (C.Args[0].K != FArgIR::Field) { *Err = "a dispatcher must be a property: " + Method; return false; }

    if (Method == "Add" || Method == "Remove")
    {
        C.Intrinsic = Method == "Add" ? "__AddDelegate__" : "__RemoveDelegate__";
        C.Args.emplace_back();
        return LowerDelegateValue(*Args[0], *Args[1], C.Args[1], Err);   // clang checked the arity
    }
    if (Method == "Clear") { C.Intrinsic = "__ClearDelegate__"; return true; }
    if (Method != "Broadcast") { *Err = "TODO: unimplemented dispatcher method " + Method; return false; }

    auto Sig = CurSignatures.find(C.Args[0].S);
    if (C.Args[0].Owner.V != BP.ClassIndex().V || Sig == CurSignatures.end())
    {
        *Err = "TODO: Broadcast needs the dispatcher's signature function, which only a UE_DISPATCHER of this class has: "
             + C.Args[0].S;
        return false;
    }
    C.Intrinsic = "__Broadcast__";
    C.Fn = Sig->second;
    for (const Json* A : Args)
    {
        C.Args.emplace_back();
        if (!LowerArg(*A, BP, C.Args.back(), Err)) return false;
    }
    return true;
}

/* What a raw pointer expression points at ("int32" for an `int32 *`), from the spelling NormalizePointers kept. */
std::string PointeeOf(const Json& N)
{
    auto T = N.find("type");
    if (T == N.end() || !T->contains("origQualType")) return std::string();
    std::string Q = StripTypeKeywords((*T)["origQualType"].get<std::string>());
    if (Q.find('(') != std::string::npos || Q.find('&') != std::string::npos) return std::string();
    if (Q.size() > 6 && Q.compare(Q.size() - 5, 5, "const") == 0) Q = StripTypeKeywords(Q.substr(0, Q.size() - 5));
    return Q.empty() || Q.back() != '*' ? std::string() : StripTypeKeywords(Q.substr(0, Q.size() - 1));
}

/* An lvalue as written: parentheses and const-adding casts looked through, nothing that copies it. */
const Json* PeelLvalue(const Json* N)
{
    while (N && First(*N) && (Kind(*N) == "ParenExpr" || Kind(*N) == "ExprWithCleanups"
                              || (Kind(*N) == "ImplicitCastExpr" && N->value("castKind", std::string()) == "NoOp")))
        N = First(*N);
    return N;
}

/* `Items[i]` on a TArray: a Kismet ArrayGetByRef, not pointer indexing. */
bool IsTArrayElement(const Json& N)
{
    if (Kind(N) != "CXXOperatorCallExpr") return false;
    const Json* Callee = Strip(First(N));
    const Json* Arr = Nth(N, 1);
    std::string Inner;
    return Callee && Callee->contains("referencedDecl") && Name((*Callee)["referencedDecl"]) == "operator[]"
        && Arr && TemplateArg(TypeOf(*Arr), "TArray", &Inner);
}

/* `Map[Key]` on a TMap: Map_Find for a read, Map_Add for a store. */
bool IsTMapElement(const Json& N)
{
    if (Kind(N) != "CXXOperatorCallExpr") return false;
    const Json* Callee = Strip(First(N));
    const Json* Map = Nth(N, 1);
    std::string Inner;
    return Callee && Callee->contains("referencedDecl") && Name((*Callee)["referencedDecl"]) == "operator[]"
        && Map && TemplateArg(TypeOf(*Map), "TMap", &Inner);
}

/* A TMap or TSet anywhere in a property, a nested one's wrapper included: the engine replicates neither. */
bool HoldsMapOrSet(const FPropertyDef& P)
{
    if (P.Type == "MapProperty" || P.Type == "SetProperty") return true;
    if (P.Type == "StructProperty" && P.StructName.compare(0, 4, "FNC_") == 0
        && (P.StructName.find("TMap") != std::string::npos || P.StructName.find("TSet") != std::string::npos)) return true;
    return P.Inner && HoldsMapOrSet(*P.Inner);
}

/* A variable, a member of one, or a member of this: what `T& R` can be another name for. */
bool IsAliasable(const Json& N)
{
    if (Kind(N) == "DeclRefExpr")
    {
        const std::string K = N["referencedDecl"].value("kind", std::string());
        return K == "VarDecl" || K == "ParmVarDecl";
    }
    if (Kind(N) != "MemberExpr" || !First(N)) return false;
    const Json* Base = PeelLvalue(First(N));
    if (Kind(*Base) == "CXXThisExpr") return true;
    return !N.value("isArrow", false) && IsAliasable(*Base);
}

/* The intrinsics that read their operand's storage through a StructMember donor field. */
bool IsReinterpret(const std::string& Intrinsic)
{
    return Intrinsic == "__AddrOf__" || Intrinsic == "__AsObject__" || Intrinsic == "__NameIndex__";
}

/* Whether an operand leaves MostRecentProperty and its storage behind, as a StructMember reinterpretation
   needs (ScriptCore.cpp execStructMemberContext); a call or a literal leaves neither. */
bool IsBranch(const std::string& Intrinsic)
{
    return Intrinsic == "__AndAlso__" || Intrinsic == "__OrElse__" || Intrinsic == "__Select__";
}

bool IsStored(const FArgIR& A)
{
    return A.K == FArgIR::Local || A.K == FArgIR::LocalOut || A.K == FArgIR::Field || A.K == FArgIR::Member
        || A.K == FArgIR::Index || (A.K == FArgIR::Call && A.Sub && A.Sub->Intrinsic == "__RefAtInline__");
}

bool FCompiler::IsRawPointer(std::string T) const
{
    T = StripTypeKeywords(T);
    if (T.size() > 6 && T.compare(T.size() - 5, 5, "const") == 0 && (T[T.size() - 6] == '*' || T[T.size() - 6] == ' '))
        T = StripTypeKeywords(T.substr(0, T.size() - 5));        // `int32 *const`
    if (T.empty() || T.back() != '*' || T.find('(') != std::string::npos || T.find("::*") != std::string::npos)
        return false;
    const std::string Pointee = StripTypeKeywords(T.substr(0, T.size() - 1));
    for (const char* Char : { "char", "wchar_t", "char16_t", "char32_t", "TCHAR" })
        if (Pointee == Char) return false;                          // a string literal
    if (!Pointee.empty() && Pointee.back() == '*') return true;
    /* A UObject class carries UE_CLASS or a base; FString, FName and the containers are plain C++ records. */
    if (const FRecord* R = Find(Pointee)) return R->bIsStruct || (R->UePackage.empty() && R->Base.empty());
    /* ponytail: a class these headers only forward-declare is still an object when UE's prefix - or a Blueprint
       class's _C - says so, so a property of it stays a reference the GC sees (and an unknown one is refused, not
       cooked as an int64); an undefined class with neither passes for raw. */
    if (Pointee.size() > 2 && Pointee.compare(Pointee.size() - 2, 2, "_C") == 0) return false;
    return !(Pointee.size() > 1 && (Pointee[0] == 'U' || Pointee[0] == 'A') && std::isupper(uint8(Pointee[1])));
}

/* A reference's referent and a function type's return are respelled too: `void *&` is `int64 &`. */
void FCompiler::NormalizePointers(Json& N) const
{
    if (!N.is_structured()) return;
    auto T = N.is_object() ? N.find("type") : N.end();
    if (T != N.end() && T->is_object() && T->contains("qualType") && !T->contains("origQualType"))
    {
        const std::string Q = (*T)["qualType"].get<std::string>();
        size_t Core = std::min(Q.find('('), Q.size());
        while (Core > 0 && (Q[Core - 1] == ' ' || Q[Core - 1] == '&')) --Core;
        if (IsRawPointer(Q.substr(0, Core)))
        {
            const std::string Rest = Q.substr(Core);
            (*T)["origQualType"] = Q;
            (*T)["qualType"] = "int64" + std::string(!Rest.empty() && Rest[0] == '&' ? " " : "") + Rest;
        }
    }
    for (Json& C : N) NormalizePointers(C);
}

bool FCompiler::HasDerefStruct(std::string* Err) const
{
    const FRecord* D = Find("FDeref");
    if (D && D->bIsStruct && !D->IsNative()) return true;
    *Err = "memory through a pointer needs this mod to declare "
           "`struct FDeref { UE_STRUCT; int64 Data; int32 Num; int32 Max; };` (as ReadProperty.cpp does)";
    return false;
}

/* Whether evaluating N can neither fault nor do anything: then `L && N` may run both sides, as one BooleanAND.
   Literals, locals, parameters and self's fields; arithmetic, comparisons and bitwise operators on those; division
   only by a non-zero literal. Not a call, a dereference, `Obj->Field` (a null Obj logs Accessed None), an index, or
   an address-holding reference local. */
bool FCompiler::IsEagerSafe(const Json& N) const
{
    const std::string K = Kind(N);
    auto Sub = [&](size_t I) { const Json* C = Nth(N, I); return C && IsEagerSafe(*C); };
    if (K == "IntegerLiteral" || K == "FloatingLiteral" || K == "CXXBoolLiteralExpr" || K == "CXXNullPtrLiteralExpr") return true;
    if (K == "ParenExpr") return Sub(0);
    if (K == "ImplicitCastExpr")
    {
        const std::string Cast = N.value("castKind", std::string());
        return Cast != "UserDefinedConversion" && Cast != "ConstructorConversion" && Sub(0);
    }
    if (K == "DeclRefExpr")
    {
        const Json& D = N["referencedDecl"];
        const std::string DK = D.value("kind", std::string());
        return DK == "EnumConstantDecl" || ((DK == "VarDecl" || DK == "ParmVarDecl") && !RefAddr.count(D.value("id", std::string())));
    }
    if (K == "MemberExpr")
    {
        const Json* Base = Strip(First(N));
        if (!Base) return false;
        if (N.value("isArrow", false)) return Kind(*Base) == "CXXThisExpr";
        return Sub(0);
    }
    if (K == "UnaryOperator")
    {
        const std::string Op = N.value("opcode", std::string());
        return (Op == "!" || Op == "-" || Op == "~" || Op == "+") && Sub(0);
    }
    if (K == "BinaryOperator")
    {
        static const std::set<std::string> Ops = { "==", "!=", "<", ">", "<=", ">=", "+", "-", "*", "&", "|", "^",
                                                   "<<", ">>", "&&", "||" };
        const std::string Op = N.value("opcode", std::string());
        if (Op == "/" || Op == "%")
        {
            const Json* Rhs = Strip(Nth(N, 1));
            return Rhs && Kind(*Rhs) == "IntegerLiteral" && Rhs->value("value", std::string("0")) != "0" && Sub(0);
        }
        return Ops.count(Op) && Sub(0) && Sub(1);
    }
    return false;
}

/* `*P`, `P[i]` on a raw pointer, `__PtrCast__<T&>(A)`, and a reference local that keeps an address. */
bool FCompiler::IsDerefLvalue(const Json& N) const
{
    const std::string K = Kind(N);
    if (K == "UnaryOperator") return N.value("opcode", std::string()) == "*";
    if (K == "ArraySubscriptExpr") return true;
    if (K == "DeclRefExpr") return RefAddr.count(N["referencedDecl"].value("id", std::string())) != 0;
    if (K != "CallExpr" || N.value("valueCategory", std::string()) != "lvalue") return false;
    const Json* Callee = Strip(First(N));
    return Callee && Kind(*Callee) == "DeclRefExpr" && Name((*Callee)["referencedDecl"]) == "__PtrCast__";
}

/* The __Read*__ that loads a Pointee. It also sizes a __RefAt__'s view: a callee copying the view's element
   into a buffer sized for the Pointee needs the two to agree, so no view means no whole-value access. */
const FReadViewSpec* FCompiler::ViewFor(const std::string& PointeeType) const
{
    const std::string P = StripTypeKeywords(PointeeType);
    const char* Read = nullptr;
    if (!P.empty() && P.back() == '*')
        Read = IsRawPointer(P) ? "__Read64__"
             : StripTypeKeywords(P.substr(0, P.size() - 1)) == "UClass" ? "__ReadClass__" : "__ReadObject__";
    else if (P == "int64" || P == "uint64" || P == "long long" || P == "unsigned long long") Read = "__Read64__";
    else if (P == "int" || P == "int32" || P == "uint32" || P == "unsigned int") Read = "__Read32__";
    else if (P == "float") Read = "__ReadFloat__";
    else if (P == "uint8" || P == "int8" || P == "unsigned char" || P == "signed char" || P == "char" || P == "bool")
        Read = "__ReadByte__";
    else if (auto E = Enums.find(P); E != Enums.end())
        Read = E->second.Underlying == "uint8" ? "__ReadByte__" : E->second.Underlying == "int32" ? "__Read32__"
             : E->second.Underlying == "int64" ? "__Read64__" : nullptr;
    else if (P == "FName") Read = "__ReadName__";
    else if (P == "FString") Read = "__ReadString__";
    else if (P == "FText") Read = "__ReadText__";
    return Read ? FindReadView(Read) : nullptr;
}

/* The value at Addr: a __Read*__, which the hoist pass loads into a temp. */
bool FCompiler::ReadThrough(FArgIR Addr, const std::string& Pointee, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    const FReadViewSpec* V = ViewFor(Pointee);
    if (!V) { *Err = "TODO: a whole " + Pointee + " through a pointer; P->Member reaches its members"; return false; }
    Out = FArgIR();
    Out.K = FArgIR::Call;
    Out.InnerType = V->ResultType;
    Out.Sub = std::make_shared<FCallIR>();
    Out.Sub->Intrinsic = V->Intrinsic;
    Out.Sub->Args.push_back(std::move(Addr));
    if (StripTypeKeywords(Pointee) != "bool") return true;
    WrapInCall(Out, BP.EngineFunction("/Script/Engine", "KismetMathLibrary", "NotEqual_ByteByte"));
    FArgIR Zero;
    Zero.K = FArgIR::Byte;
    Out.Sub->Args.push_back(Zero);
    Out.InnerType = "bool";
    return true;
}

/* The memory at Addr as an lvalue: a __RefAt__, whose hoist types its view by the Pointee. */
bool FCompiler::RefThrough(FArgIR Addr, const std::string& Pointee, FArgIR& Out, std::string* Err)
{
    if (!ViewFor(Pointee)) { *Err = "TODO: a whole " + Pointee + " through a pointer; P->Member reaches its members"; return false; }
    Out = FArgIR();
    Out.K = FArgIR::Call;
    Out.InnerType = StripTypeKeywords(Pointee);
    Out.Sub = std::make_shared<FCallIR>();
    Out.Sub->Intrinsic = "__RefAt__";
    Out.Sub->Args.push_back(std::move(Addr));
    return true;
}

/* An index or pointer offset in bytes: Count * sizeof(Pointee), folded for a literal. */
bool FCompiler::ScaleIndex(const Json& IndexNode, const std::string& Pointee, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    int32 Size = 0, Align = 0;
    if (!LayoutOf(Pointee, &Size, &Align, Err) || !LowerArg(IndexNode, BP, Out, Err)) return false;
    if (Out.K == FArgIR::Int || Out.K == FArgIR::Int64)
    {
        Out.I64 = (Out.K == FArgIR::Int ? int64(Out.I) : Out.I64) * Size;
        Out.K = FArgIR::Int64;
        Out.InnerType = "int64";
        return true;
    }
    if (!ConvertArg("int64", BP, Out, Err)) return false;
    if (Size == 1) return true;
    WrapInCall(Out, BP.EngineFunction("/Script/Engine", "KismetMathLibrary", "Multiply_Int64Int64"));
    FArgIR Bytes;
    Bytes.K = FArgIR::Int64;
    Bytes.I64 = Size;
    Out.Sub->Args.push_back(Bytes);
    Out.InnerType = "int64";
    return true;
}

/* Where an lvalue sits: through a raw pointer (`*P`, `P[i]`), `__PtrCast__<T&>(A)`, a reference local that
   keeps an address, or a TArray element (the array's Data pointer plus the offset). A Blueprint variable
   itself has no address the VM hands out. */
bool FCompiler::LowerAddress(const Json& Lvalue, FBlueprintClass& BP, FArgIR& Out, std::string* Pointee, std::string* Err)
{
    auto PlusOffset = [&](FArgIR Base, FArgIR Offset) {
        Out = std::move(Base);
        if (Offset.K == FArgIR::Int64 && Offset.I64 == 0) return;
        WrapInCall(Out, BP.EngineFunction("/Script/Engine", "KismetMathLibrary", "Add_Int64Int64"));
        Out.Sub->Args.push_back(std::move(Offset));
        Out.InnerType = "int64";
    };
    const Json* N = Strip(&Lvalue);
    const std::string K = N ? Kind(*N) : std::string();
    if (K == "UnaryOperator" && N->value("opcode", std::string()) == "*")
    {
        const Json* Ptr = Nth(*N, 0);
        *Pointee = Ptr ? PointeeOf(*Ptr) : std::string();
        if (Pointee->empty()) { *Err = "TODO: `*` on " + (Ptr ? TypeOf(*Ptr) : std::string()) + ", which is not a raw pointer"; return false; }
        return LowerArg(*Ptr, BP, Out, Err);
    }
    if (K == "ArraySubscriptExpr")
    {
        const Json* Base = Nth(*N, 0);
        const Json* Index = Nth(*N, 1);
        if (Base && Index && PointeeOf(*Base).empty()) std::swap(Base, Index);     // `2[P]`
        *Pointee = Base ? PointeeOf(*Base) : std::string();
        if (Pointee->empty() || !Index) { *Err = "TODO: indexing that is not through a raw pointer: " + TypeOf(*N); return false; }
        FArgIR Ptr, Offset;
        if (!LowerArg(*Base, BP, Ptr, Err) || !ScaleIndex(*Index, *Pointee, BP, Offset, Err)) return false;
        PlusOffset(std::move(Ptr), std::move(Offset));
        return true;
    }
    if (K == "CallExpr" && IsDerefLvalue(*N))
    {
        *Pointee = StripTypeKeywords(TypeOf(*N));
        return LowerPtrCastSource(*N, BP, Out, Err) && ConvertArg("int64", BP, Out, Err);
    }
    if (K == "DeclRefExpr")
    {
        const Json& Ref = (*N)["referencedDecl"];
        if (auto A = RefAddr.find(Ref.value("id", std::string())); A != RefAddr.end())
        {
            *Pointee = A->second;
            Out = FArgIR();
            Out.K = FArgIR::Local;
            Out.S = LocalName(Ref);
            Out.InnerType = "int64";
            return true;
        }
        if (auto A = RefAlias.find(Ref.value("id", std::string())); A != RefAlias.end())
            return LowerAddress(A->second, BP, Out, Pointee, Err);
    }
    if (N && IsTArrayElement(*N))
    {
        const Json* Index = Nth(*N, 2);
        *Pointee = StripTypeKeywords(TypeOf(*N));
        FArgIR Array, Offset;
        if (!Index || !LowerArg(*Nth(*N, 1), BP, Array, Err) || !ScaleIndex(*Index, *Pointee, BP, Offset, Err)) return false;
        if (!IsStored(Array)) { *Err = "the address of an element needs an array variable"; return false; }
        Array.InnerType = "class UObject *";    // a TArray's first 8 bytes are its Data pointer, read as __AddrOf__ reads one
        if (!ConvertArg("int64", BP, Array, Err)) return false;
        PlusOffset(std::move(Array), std::move(Offset));
        return true;
    }
    if (K == "MemberExpr" && N->value("isArrow", false))
    {
        /* `&Obj->Member`: a cooked property carries no offset, so ReadProperty::GetPropertyAddress finds the property
           by name on Obj's class at run time and adds its offset to Obj's address (0 when there is none). */
        const Json* Base = Nth(*N, 0);
        std::string ObjType = Base ? StripTypeKeywords(TypeOf(*Base)) : std::string();
        while (!ObjType.empty() && (ObjType.back() == '*' || ObjType.back() == ' ')) ObjType.pop_back();
        const FRecord* Owner = Find(ObjType);
        if (!Owner || Owner->bIsStruct) { *Err = "TODO: the address of a member of " + ObjType + ", which is not an object class"; return false; }
        const FRecord* Lib = Find("ReadProperty");
        if (!Lib) { *Err = "`&Obj->Member` finds the member at run time through ReadProperty::GetPropertyAddress: include ReadProperty.h"; return false; }
        FArgIR Obj;
        if (!LowerArg(*Base, BP, Obj, Err)) return false;
        const std::string Pkg = Lib->IsNative() ? Lib->UePackage : ModPackage + "/" + Lib->CppName;
        const std::string Cls = Lib->IsNative() ? Lib->UeName : Lib->CppName + "_C";
        Out = FArgIR();
        Out.K = FArgIR::Call;
        Out.InnerType = "int64";
        Out.Sub = std::make_shared<FCallIR>();
        Out.Sub->Fn = BP.EngineFunction(Pkg, Cls, "GetPropertyAddress");
        Out.Sub->bScript = true;
        Out.Sub->Context = BP.ClassDefaultObject(Pkg, Cls);
        FArgIR Name, Wco;
        Name.K = FArgIR::Name;
        Name.S = UeNameOf(Owner, N->value("name", std::string()));      // FindPropertyByName, at run time
        if (!CurrentWco.empty()) { Wco.K = FArgIR::Local; Wco.S = CurrentWco; }
        Out.Sub->Args = { Obj, Name, Wco };
        *Pointee = StripTypeKeywords(TypeOf(*N));
        return true;
    }
    *Err = "TODO: the address of " + (N ? K : std::string("nothing")) + ": a Blueprint variable has none the VM hands "
           "out; point into an object (`(uint8*)Obj`), a TArray element (`&Items[I]`) or memory through a pointer";
    return false;
}

/* `__PtrCast__`'s argument: a reference local that keeps an address stands for that address, a reference to a
   Blueprint variable has none to give, and anything else converts by value. */
bool FCompiler::LowerPtrCastSource(const Json& Call, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    const Json* Arg = Nth(Call, 1);
    if (!Arg) { *Err = "__PtrCast__ takes one argument"; return false; }
    const Json* Src = Strip(Arg);
    const std::string Id = Src && Kind(*Src) == "DeclRefExpr" ? (*Src)["referencedDecl"].value("id", std::string()) : std::string();
    if (RefAlias.count(Id))
    {
        *Err = "__PtrCast__ of " + Name((*Src)["referencedDecl"]) + ", a reference to a Blueprint variable, which has no address";
        return false;
    }
    std::string Pointee;
    return RefAddr.count(Id) ? LowerAddress(*Src, BP, Out, &Pointee, Err) : LowerArg(*Arg, BP, Out, Err);
}

/* The operand of a StructMember reinterpretation (__AddrOf__, __AsObject__, __NameIndex__) that is not stored
   anywhere goes into a temp first: EX_StructMemberContext reads the storage the operand leaves behind. */
bool FCompiler::HoistOperand(FArgIR& Operand, FBlueprintClass& BP, std::vector<FPropertyDef>& Locals,
                             std::vector<FStmtIR>& OutPre, std::string* Err)
{
    if (IsStored(Operand)) return true;
    const std::string Type = Operand.K == FArgIR::Int64 ? std::string("int64")
                           : Operand.K == FArgIR::Self ? std::string("class UObject *") : Operand.InnerType;
    const std::string Tmp = "__PtrTmp" + std::to_string(ReadTmpCounter++) + "__";
    FPropertyDef PD;
    if (!TypeToProperty(Type, Tmp, 0, Tmp, BP, &PD, Err)) return false;
    PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
    Locals.push_back(PD);

    FStmtIR St;
    St.K = FStmtIR::Assign;
    St.Var.K = FArgIR::Local;
    St.Var.S = Tmp;
    St.Var.LetOp = LetOpFor(StripTypeKeywords(Type));
    St.bAssignLocal = true;
    St.Value = std::move(Operand);
    OutPre.push_back(std::move(St));

    Operand = FArgIR();
    Operand.K = FArgIR::Local;
    Operand.S = Tmp;
    Operand.InnerType = Type;
    return true;
}

/* The outer (pre-Strip) type is the slot the value lands in; a string-kind mismatch against the
   value's own type becomes a Kismet conversion, so `FName n = Str + Count` just works. */
bool FCompiler::LowerArg(const Json& ArgNode, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    const std::string OuterType = TypeOf(ArgNode);
    /* Memory through a pointer that is not read (no LValueToRValue above it) binds a reference parameter: the
       memory itself, so the callee's writes land there. */
    if (const Json* Bare = PeelLvalue(&ArgNode); Bare && IsDerefLvalue(*Bare))
    {
        FArgIR Addr;
        std::string Pointee;
        return LowerAddress(*Bare, BP, Addr, &Pointee, Err) && RefThrough(std::move(Addr), Pointee, Out, Err)
            && ConvertArg(OuterType, BP, Out, Err);
    }
    /* A consteval call: clang ran it and left the answer on the ConstantExpr around it, which Strip would drop. */
    for (const Json* W = &ArgNode; W; W = Kind(*W) == "ImplicitCastExpr" || Kind(*W) == "ParenExpr" ? First(*W) : nullptr)
        if (FConstVal V; Kind(*W) == "ConstantExpr" && W->contains("value") && FoldConst(ArgNode, V) && ConstToArg(V, OuterType, Out))
            return true;
    const Json* N = Strip(&ArgNode);
    if (!N) { *Err = "empty argument expression"; return false; }
    if (!LowerArgRaw(*N, OuterType, BP, Out, Err)) return false;
    if (Out.InnerType.empty()) Out.InnerType = TypeOf(*N);
    return ConvertArg(OuterType, BP, Out, Err);
}

bool FCompiler::LowerArgRaw(const Json& Node, const std::string& OuterType, FBlueprintClass& BP,
                            FArgIR& Out, std::string* Err)
{
    const Json* N = &Node;
    const EStrKind Slot = StrKindOf(OuterType);
    const std::string K = Kind(*N);
    if (K == "MemberExpr") return LowerField(*N, BP, Out, Err);
    if (K == "CXXThisExpr") { Out.K = FArgIR::Self; return true; }
    if (K == "CXXNullPtrLiteralExpr") { Out.K = FArgIR::NullObj; return true; }
    if (K == "UnaryExprOrTypeTraitExpr" && (N->value("name", std::string()) == "sizeof" || N->value("name", std::string()) == "alignof"))
    {
        /* The game's layout, not the host compiler's: clang sizes the UeApi stand-ins, which are not the real types. */
        const Json* Operand = First(*N);
        const std::string T = N->contains("argType") ? (*N)["argType"].value("qualType", std::string())
                            : Operand ? TypeOf(*Operand) : std::string();
        int32 Size = 0, Align = 0;
        if (!LayoutOf(T, &Size, &Align, Err)) { *Err = N->value("name", std::string()) + ": " + *Err; return false; }
        Out.K = FArgIR::Int;
        Out.I = N->value("name", std::string()) == "sizeof" ? Size : Align;
        return true;
    }
    if ((K == "CXXConstructExpr" || K == "CXXTemporaryObjectExpr")
        && StripTypeKeywords(TypeOf(*N)).compare(0, 10, "TDelegate<") == 0)
    {
        const Json* Obj = Nth(*N, 0);
        const Json* Fn = Nth(*N, 1);
        if (!Obj || !Fn) { *Err = "a delegate value is {this, &Class::Function}"; return false; }
        return LowerDelegateValue(*Obj, *Fn, Out, Err);
    }
    if (K == "CXXConstructExpr" || K == "CXXTemporaryObjectExpr")
    {
        auto SI = Structs.find(StripTypeKeywords(TypeOf(*N)));
        if (SI != Structs.end()) return LowerStructLiteral(*N, SI->second, BP, Out, Err);
    }
    /* `T()` of an aggregate - a struct with no constructor declared, which is what lets it take `{ .A = 1 }`. */
    if (const FRecord* R = K == "CXXScalarValueInitExpr" ? Find(StripTypeKeywords(TypeOf(*N))) : nullptr; R && R->bIsStruct)
        return LowerMakeStruct(R->CppName, nullptr, BP, Out, Err);
    if ((K == "CXXConstructExpr" || K == "CXXTemporaryObjectExpr")
        && (Slot == SK_Name || Slot == SK_Text || Slot == SK_Str))
    {
        /* `FName()` / `FText()` / `FString()`: an argless ctor node Strip could not descend into. */
        Out.K = Slot == SK_Name ? FArgIR::Name : Slot == SK_Text ? FArgIR::Text : FArgIR::Str;
        Out.S = Slot == SK_Name ? "None" : "";
        return true;
    }
    if (K == "StringLiteral")
    {
        /* Wide literals are detected by type; the value is clang's source spelling. */
        Out.K = FArgIR::Str;
        const std::string Ty = TypeOf(*N);
        Out.bWide = Ty.find("wchar_t") != std::string::npos
                 || Ty.find("char16_t") != std::string::npos
                 || Ty.find("char32_t") != std::string::npos;
        Out.S = Unquote(N->value("value", std::string()));
        return true;
    }
    if (K == "CXXMemberCallExpr")
    {
        const Json* Callee = Strip(First(*N));
        if (Callee && Kind(*Callee) == "MemberExpr" && Name(*Callee).compare(0, 9, "operator ") == 0)
        {
            const Json* Obj = First(*Callee);
            if (!Obj) { *Err = "conversion operator with no object"; return false; }
            return LowerArg(*Obj, BP, Out, Err) && ConvertArg(TypeOf(*N), BP, Out, Err);
        }
        const Json* Obj = Callee ? Strip(First(*Callee)) : nullptr;
        if (!Obj) { *Err = "member call with no object"; return false; }
        const std::string ObjType = StripTypeKeywords(TypeOf(*Obj));
        if (ObjType.compare(0, 10, "TMulticast") == 0) return LowerDispatcherCall(*N, *Callee, *Obj, BP, Out, Err);
        std::string Iface;
        if (TemplateArg(ObjType, "TScriptInterface", &Iface))
        {
            /* GetObject() is its only method: EX_InterfaceToObjCast to UObject. */
            Out.K = FArgIR::DynCast;
            Out.CastOp = EX_InterfaceToObjCast;
            Out.Owner = BP.EngineClass("/Script/CoreUObject", "Object");
            Out.InnerType = "UObject *";
            Out.Sub = std::make_shared<FCallIR>();
            Out.Sub->Args.emplace_back();
            return LowerArg(*Obj, BP, Out.Sub->Args[0], Err);
        }
        if (IsContainerType(TypeOf(*Obj)))
        {
            /* `Items.Add(x)` is KismetArrayLibrary::Array_Add(Items, x): the container variable is the
               first argument, and the library's CustomThunks type themselves from that property. */
            const std::string C = StripTypeKeywords(TypeOf(*Obj));
            const char* Lib = C[1] == 'A' ? "KismetArrayLibrary" : C[1] == 'S' ? "BlueprintSetLibrary" : "BlueprintMapLibrary";
            const std::string Prefix = C[1] == 'A' ? "Array_" : C[1] == 'S' ? "Set_" : "Map_";
            std::string Method = Name(*Callee);
            if (Method == "Num") Method = "Length";
            FArgIR Target;
            if (!LowerArg(*Obj, BP, Target, Err)) return false;
            if (Target.K != FArgIR::Field && Target.K != FArgIR::Local && Target.K != FArgIR::LocalOut && Target.K != FArgIR::Member)
            { *Err = "a container operation needs a variable, not a computed value: " + Method; return false; }
            static const std::set<std::string> Reads = { "Length", "LastIndex", "IsValidIndex", "Contains", "Find", "Get",
                                                         "Keys", "Values", "ToArray", "Identical", "Difference",
                                                         "Intersection", "Union" };
            if (!Reads.count(Method)) WarnRpcRefWrite(Target);
            Out.K = FArgIR::Call;
            Out.Sub = std::make_shared<FCallIR>();
            Out.Sub->Fn = BP.EngineFunction("/Script/Engine", Lib, Prefix + Method);
            Out.Sub->Args.push_back(Target);
            bool bFirst = true, bOk = true;
            std::string LastType;
            ForEach(*N, [&](const Json& A) {
                if (bFirst) { bFirst = false; return; }
                if (!bOk) return;
                FArgIR V;
                bOk = LowerArg(A, BP, V, Err);
                if (bOk) Out.Sub->Args.push_back(V);
                LastType = TypeOf(A);
            });
            if (bOk && Out.Sub->Args.size() == 3 && ((Prefix == "Map_" && Method == "Find") || (Prefix == "Array_" && Method == "Get")))
            {
                while (!LastType.empty() && (LastType.back() == '&' || LastType.back() == ' ')) LastType.pop_back();
                LastType = StripTypeKeywords(LastType);
                if (IsContainerType(LastType))
                {
                    FArgIR Dest = Out.Sub->Args[2];
                    FStmtIR Copy;
                    Copy.K = FStmtIR::Assign;
                    Copy.Var = Dest;
                    Copy.Var.LetOp = LetOpFor(LastType);
                    Copy.bAssignLocal = Dest.K == FArgIR::Local;
                    Copy.bAssignOutParm = Dest.K == FArgIR::LocalOut;
                    return NestedWrapperOut(LastType, 2, Prefix == "Map_" ? "bool" : "", std::move(Copy), BP, Out, Err);
                }
            }
            return bOk;
        }
        Out.K = FArgIR::Call;
        Out.Sub = std::make_shared<FCallIR>();
        if (!LowerCall(*N, BP, *Out.Sub, Err)) return false;
        if (Kind(*Obj) == "CXXThisExpr") return true;
        Out.Sub->Target = std::make_shared<FArgIR>();
        if (!LowerArg(*Obj, BP, *Out.Sub->Target, Err)) return false;
        /* Through an interface the implementation is found by name, as the Blueprint compiler calls it. */
        if (Out.Sub->Target->K == FArgIR::InterfaceCtx)
        {
            auto Owner = MethodOwner.find(Callee->value("referencedMemberDecl", std::string()));
            Out.Sub->VirtualName = UeNameOf(Owner == MethodOwner.end() ? nullptr : Find(Owner->second), Name(*Callee));
        }
        return true;
    }
    if (K == "CXXOperatorCallExpr")
    {
        const Json* Callee = Strip(First(*N));
        const std::string OpName = (Callee && Callee->contains("referencedDecl"))
            ? (*Callee)["referencedDecl"].value("name", std::string()) : std::string();
        const Json* Lhs = Nth(*N, 1);
        const Json* Rhs = Nth(*N, 2);
        std::string Iface;
        if ((OpName == "operator==" || OpName == "operator!=") && Lhs && Rhs
            && TemplateArg(StripTypeKeywords(TypeOf(*Lhs)), "TScriptInterface", &Iface)
            && Kind(*Strip(Rhs)) == "CXXNullPtrLiteralExpr")
        {
            /* `I != nullptr` is `bool(I)`, and `I == nullptr` its negation. */
            if (!LowerArg(*Lhs, BP, Out, Err) || !ConvertArg("bool", BP, Out, Err)) return false;
            if (OpName == "operator==") Out = NotOf(std::move(Out), BP);
            return true;
        }
        if (OpName == "operator->" && Lhs && TemplateArg(TypeOf(*Lhs), "TScriptInterface", &Iface))
        {
            /* `I->Fn()`: the call's object is EX_InterfaceContext(I). */
            Out.K = FArgIR::InterfaceCtx;
            Out.Base = std::make_shared<FArgIR>();
            return LowerArg(*Lhs, BP, *Out.Base, Err);
        }
        if (IsTMapElement(*N) && Rhs)
        {
            /* `Map[Key]` read: Map_Find into a temp, which it resets to the value type's default for a missing key
               (UBlueprintMapLibrary::GenericMap_Find), and the temp is the value. A store is Map_Add, in LowerBody. */
            std::string Value = TypeOf(*N);
            while (!Value.empty() && (Value.back() == '&' || Value.back() == ' ')) Value.pop_back();
            Value = StripTypeKeywords(Value);
            FArgIR Map, Key;
            if (!LowerArg(*Lhs, BP, Map, Err) || !LowerArg(*Rhs, BP, Key, Err)) return false;
            if (Map.K != FArgIR::Field && Map.K != FArgIR::Local && Map.K != FArgIR::LocalOut && Map.K != FArgIR::Member)
            { *Err = "`[]` on a map needs a map variable, not a computed value"; return false; }
            const std::string Tmp = "__MapGet" + std::to_string(ReadTmpCounter++) + "__";
            if (IsContainerType(Value))
            {
                /* The value is copied out of a wrapper temp into Tmp, which is then the value. */
                FPropertyDef Holder;
                if (!TypeToProperty(Value, Tmp, 0, "a map element", BP, &Holder, Err)) return false;
                Holder.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
                CurLocals->push_back(Holder);
                Out.K = FArgIR::Call;
                Out.Sub = std::make_shared<FCallIR>();
                Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "BlueprintMapLibrary", "Map_Find");
                FArgIR Into;
                Into.K = FArgIR::Local;
                Into.S = Tmp;
                Out.Sub->Args = { Map, Key, Into };
                FStmtIR Copy;
                Copy.K = FStmtIR::Assign;
                Copy.Var = Into;
                Copy.Var.LetOp = LetOpFor(Value);
                Copy.bAssignLocal = true;
                if (!NestedWrapperOut(Value, 2, "", std::move(Copy), BP, Out, Err)) return false;
                Out.K = FArgIR::Call;
                Out.InnerType = Value;
                Out.Sub->InlineResult = Tmp;
                Out.Sub->InlineType = Value;
                return true;
            }
            FPropertyDef PD;
            if (!TypeToProperty(Value, Tmp, 0, "a map element", BP, &PD, Err)) return false;
            PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
            CurLocals->push_back(PD);
            FArgIR Into;
            Into.K = FArgIR::Local;
            Into.S = Tmp;
            auto Body = std::make_shared<std::vector<FStmtIR>>(1);
            (*Body)[0].K = FStmtIR::StaticCall;
            (*Body)[0].Call.Fn = BP.EngineFunction("/Script/Engine", "BlueprintMapLibrary", "Map_Find");
            (*Body)[0].Call.Args = { Map, Key, Into };
            auto Block = std::make_shared<std::vector<FStmtIR>>(1);
            (*Block)[0].K = FStmtIR::Block;
            (*Block)[0].Body = Body;
            Out.K = FArgIR::Call;
            Out.InnerType = Value;
            Out.Sub = std::make_shared<FCallIR>();
            Out.Sub->Intrinsic = "__Inline__";
            Out.Sub->Inline = Block;
            Out.Sub->InlineResult = Tmp;
            Out.Sub->InlineType = Value;
            return true;
        }
        if (OpName == "operator[]" && Lhs && Rhs && IsContainerType(TypeOf(*Lhs)))
        {
            /* `Items[i]`: EX_ArrayGetByRef, an lvalue or rvalue of the element type. */
            Out.K = FArgIR::Index;
            Out.Base = std::make_shared<FArgIR>();
            if (!LowerArg(*Lhs, BP, *Out.Base, Err)) return false;
            if (Out.Base->K != FArgIR::Field && Out.Base->K != FArgIR::Local && Out.Base->K != FArgIR::LocalOut && Out.Base->K != FArgIR::Member)
            { *Err = "indexing needs an array variable, not a computed value"; return false; }
            Out.Sub = std::make_shared<FCallIR>();
            FArgIR Idx;
            if (!LowerArg(*Rhs, BP, Idx, Err)) return false;
            Out.Sub->Args.push_back(Idx);
            std::string Elem = TypeOf(*N);
            while (!Elem.empty() && (Elem.back() == '&' || Elem.back() == ' ')) Elem.pop_back();
            Out.S = Out.Base->S;
            Out.Owner = Out.Base->Owner;
            Out.LetOp = LetOpFor(Elem);
            Out.InnerType = Elem;
            if (IsContainerType(Elem))
            {
                /* An element that is itself a container is a wrapper struct: the container is its Value member, which
                   leaves the FArrayProperty the Array_ functions and a further [] need. */
                auto Element = std::make_shared<FArgIR>(Out);
                Out = FArgIR();
                Out.K = FArgIR::Member;
                Out.S = "Value";
                Out.Owner = NestedWrapperImport(Elem, BP);
                Out.Base = Element;
                Out.LetOp = Element->LetOp;
                Out.InnerType = Elem;
            }
            return true;
        }
        if (Lhs && Rhs && OpName.compare(0, 8, "operator") == 0)
        {
            /* A Kismet operator over a struct, from UeApi/Ops.json. */
            if (const FOpInfo* O = FindOp(OpName.substr(8), Canon(TypeOf(*Lhs)), Canon(TypeOf(*Rhs))))
            {
                Out.K = FArgIR::Call;
                Out.Sub = std::make_shared<FCallIR>();
                Out.Sub->Fn = BP.EngineFunction(O->Package, O->Class, O->Fn);
                Out.Sub->bPure = true;
                for (const Json* Side : { Lhs, Rhs })
                {
                    FArgIR A;
                    if (!LowerArg(*Side, BP, A, Err)) return false;
                    Out.Sub->Args.push_back(A);
                }
                for (const Json& E : O->Extra) Out.Sub->Args.push_back(ConstArg(E));
                Out.InnerType = O->Ret;
                return true;
            }
        }
        if (OpName != "operator+" || !Lhs || !Rhs || StrKindOf(TypeOf(*N)) != SK_Str)
        { *Err = "TODO: unimplemented operator overload " + OpName + " yielding " + TypeOf(*N); return false; }

        /* String `+`: both sides to FString, then Concat_StrStr. */
        Out.K = FArgIR::Call;
        Out.Sub = std::make_shared<FCallIR>();
        Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetStringLibrary", "Concat_StrStr");
        for (const Json* Side : { Lhs, Rhs })
        {
            FArgIR A;
            if (!LowerArg(*Side, BP, A, Err) || !ConvertArg("FString", BP, A, Err)) return false;
            Out.Sub->Args.push_back(A);
        }
        return true;
    }
    if (K == "IntegerLiteral")
    {
        /*
        A literal wider than int32 must stay Int64 or the value truncates: the LoadLibrary
        import walk's packed name constants (0x32336C656E72656B, "kernel32") compared as
        0x6E72656B and matched nothing. Fill both fields and pick the width by magnitude,
        the EnumConstantDecl path below does the same.
        */
        const int64 V = std::stoll(N->value("value", std::string("0")));
        Out.I = int32(V);
        Out.I64 = V;
        Out.K = int64(Out.I) == V ? FArgIR::Int : FArgIR::Int64;
        return true;
    }
    if (K == "FloatingLiteral") { Out.K = FArgIR::Float; Out.F = std::stof(N->value("value", std::string("0"))); return true; }
    if (K == "CXXBoolLiteralExpr") { Out.K = FArgIR::Bool; Out.B = N->value("value", false); return true; }
    if (K == "DeclRefExpr")
    {
        /* A T& out-parm needs EX_LocalOutVariable so writes reach the caller's storage. */
        const Json& Ref = (*N)["referencedDecl"];
        const std::string RefKind = Ref.value("kind", std::string());
        if (RefKind == "EnumConstantDecl")
        {
            auto V = EnumValues.find(Ref.value("id", std::string()));
            if (V == EnumValues.end()) { *Err = "enum constant with no value: " + Name(Ref); return false; }
            const auto W = EnumConstWidth.find(Ref.value("id", std::string()));
            const int32 Width = W == EnumConstWidth.end() ? 1 : W->second;
            Out.K = Width == 8 ? FArgIR::Int64 : Width == 4 ? FArgIR::Int : FArgIR::Byte;
            Out.I = int32(V->second);
            Out.I64 = V->second;
            return true;
        }
        /* A reference local (or a range-for binding) is the variable it names, or the value at the address it keeps. */
        if (auto A = RefAlias.find(Ref.value("id", std::string())); A != RefAlias.end()) return LowerArg(A->second, BP, Out, Err);
        if (auto C = ParmConst.find(Ref.value("id", std::string())); C != ParmConst.end()) { Out = C->second; return true; }
        if (auto G = ConstVars.find(Ref.value("id", std::string())); G != ConstVars.end())
        {
            /* No storage behind it in a Blueprint: the use is the value. */
            const Json* Init = First(*G->second);
            if (const Json* Lit = Init ? Strip(Init) : nullptr; Lit && Kind(*Lit) == "StringLiteral") return LowerArg(*Lit, BP, Out, Err);
            FConstVal V;
            if (!Init || !FoldConst(*N, V))
            { *Err = Name(Ref) + " is not a constant AssetGen can work out: literals, enum constants, consteval calls and arithmetic over them"; return false; }
            if (!ConstToArg(V, TypeOf(*N), Out))
            { *Err = Name(Ref) + ": a constant of type " + TypeOf(*N) + " has no literal in Kismet"; return false; }
            return true;
        }
        if (RefKind != "ParmVarDecl" && RefKind != "VarDecl")
        {
            *Err = "TODO: DeclRefExpr to " + RefKind;
            return false;
        }
        if (IsDerefLvalue(*N))
        {
            FArgIR Addr;
            std::string Pointee;
            return LowerAddress(*N, BP, Addr, &Pointee, Err) && ReadThrough(std::move(Addr), Pointee, BP, Out, Err);
        }
        const std::string RefName = LocalName(Ref);
        Out.K = CurrentOutParms.count(RefName) ? FArgIR::LocalOut : FArgIR::Local;
        Out.S = RefName;
        return true;
    }
    if (K == "CallExpr")
    {
        const Json* CalleeNode = Strip(First(*N));
        const std::string CalleeName = (CalleeNode && Kind(*CalleeNode) == "DeclRefExpr")
            ? (*CalleeNode)["referencedDecl"].value("name", std::string())
            : std::string();
        if (CalleeName == "StaticClass")
        {
            auto Owner = MethodOwner.find((*CalleeNode)["referencedDecl"].value("id", std::string()));
            const FRecord* R = Owner == MethodOwner.end() ? nullptr : Find(Owner->second);
            if (!R) { *Err = "StaticClass() on an unknown class"; return false; }
            Out.K = FArgIR::ObjConst;
            Out.Owner = ClassImportOf(*R, BP);
            Out.InnerType = "UClass *";
            return true;
        }
        if (CalleeName == "Cast")
        {
            std::string Target = StripTypeKeywords(TypeOf(*N));
            while (!Target.empty() && (Target.back() == '*' || Target.back() == ' ')) Target.pop_back();
            const FRecord* R = Find(Target);
            const Json* Operand = Nth(*N, 1);
            if (!R || R->bIsStruct || !Operand) { *Err = "Cast<> to an unknown class: " + Target; return false; }
            Out.K = FArgIR::DynCast;
            Out.Owner = ClassImportOf(*R, BP);
            Out.Sub = std::make_shared<FCallIR>();
            FArgIR A;
            if (!LowerArg(*Operand, BP, A, Err)) return false;
            Out.Sub->Args.push_back(A);
            return true;
        }
        if (CalleeName == "__ClassOf__")
        {
            /* `__ClassOf__(x)` lowers to Cur::GetParmClassName(<this function>, FName("x")); x is never
               evaluated. The enclosing class must declare GetParmClassName (see ReadProperty.cpp). */
            const Json* Arg = Nth(*N, 1);       // inner[0] is the callee
            if (!Arg) { *Err = "__ClassOf__ requires an argument"; return false; }
            const Json* AS = Strip(Arg);
            if (!AS || Kind(*AS) != "DeclRefExpr")
            { *Err = "__ClassOf__ argument must be a bare parameter or local reference"; return false; }
            const std::string ParmName = (*AS)["referencedDecl"].value("name", std::string());
            if (ParmName.empty())
            { *Err = "__ClassOf__: argument DeclRefExpr has no name"; return false; }
            if (!Cur)
            { *Err = "__ClassOf__: no enclosing class in scope"; return false; }

            const std::string CalleePkg = ModPackage + "/" + Cur->CppName;
            const std::string CalleeCls = Cur->CppName + "_C";
            Out.K = FArgIR::Call;
            Out.Sub = std::make_shared<FCallIR>();
            Out.Sub->Fn = BP.EngineFunction(CalleePkg, CalleeCls, "GetParmClassName");
            Out.Sub->bScript = true;
            Out.Sub->Context = BP.ClassDefaultObject(CalleePkg, CalleeCls);

            FArgIR FnArg;
            FnArg.K = FArgIR::Call;
            FnArg.Sub = std::make_shared<FCallIR>();
            FnArg.Sub->Intrinsic = "__CurrentFunction__";
            Out.Sub->Args.push_back(FnArg);

            FArgIR NameArg;
            NameArg.K = FArgIR::Name;
            NameArg.S = ParmName;
            Out.Sub->Args.push_back(NameArg);
            return true;
        }

        if (CalleeName == "__PtrCast__")
        {
            /* `__PtrCast__<T&>(A)` is the T at address A; any other target converts A by value (see Intrin.h). */
            if (IsDerefLvalue(*N))
            {
                FArgIR Addr;
                std::string Pointee;
                return LowerAddress(*N, BP, Addr, &Pointee, Err) && ReadThrough(std::move(Addr), Pointee, BP, Out, Err);
            }
            return LowerPtrCastSource(*N, BP, Out, Err) && ConvertArg(TypeOf(*N), BP, Out, Err);
        }

        Out.K = FArgIR::Call;
        Out.Sub = std::make_shared<FCallIR>();
        return LowerCall(*N, BP, *Out.Sub, Err);
    }
    if (K == "ArraySubscriptExpr" || (K == "UnaryOperator" && N->value("opcode", std::string()) == "*"))
    {
        FArgIR Addr;
        std::string Pointee;
        return LowerAddress(*N, BP, Addr, &Pointee, Err) && ReadThrough(std::move(Addr), Pointee, BP, Out, Err);
    }
    if (FIndex Asset; AssetRef(*N, BP, &Asset))
    {
        Out.K = FArgIR::ObjConst;
        Out.Owner = Asset;
        Out.InnerType = TypeOf(*N);
        return true;
    }
    if (K == "UnaryOperator" && N->value("opcode", std::string()) == "&")
    {
        std::string Pointee;
        const Json* Operand = Nth(*N, 0);
        if (!Operand) { *Err = "unary `&` with a missing operand"; return false; }
        if (!LowerAddress(*Operand, BP, Out, &Pointee, Err)) return false;
        Out.InnerType = "int64";
        return true;
    }
    if (K == "UnaryOperator")
    {
        const std::string Op = N->value("opcode", std::string());
        const Json* Operand = Nth(*N, 0);
        if (!Operand) { *Err = "unary `" + Op + "` with a missing operand"; return false; }
        if (Op == "+") return LowerArg(*Operand, BP, Out, Err);
        if (Op == "++" || Op == "--")
        { *Err = "`" + Op + "` works as a statement (or a `for` increment), not inside an expression"; return false; }
        if (Op == "-" || Op == "~")
        {
            /* A literal folds; otherwise 0 - X, or Kismet's bitwise Not_Int / Not_Int64. */
            FArgIR A;
            if (!LowerArg(*Operand, BP, A, Err)) return false;
            const bool bFloat = Canon(TypeOf(*N)) == "float", bInt64 = IsInt64Type(TypeOf(*N));
            if (Op == "-" && A.K == FArgIR::Int)   { Out = A; Out.I = int32(-int64(A.I)); return true; }
            if (Op == "-" && A.K == FArgIR::Int64) { Out = A; Out.I64 = -A.I64; return true; }
            if (Op == "-" && A.K == FArgIR::Float) { Out = A; Out.F = -A.F; return true; }
            if (Op == "~" && bFloat) { *Err = "`~` on a float"; return false; }
            Out.K = FArgIR::Call;
            Out.Sub = std::make_shared<FCallIR>();
            if (Op == "~")
            {
                Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", bInt64 ? "Not_Int64" : "Not_Int");
                Out.Sub->Args = { A };
                return true;
            }
            FArgIR Zero;
            Zero.K = bFloat ? FArgIR::Float : bInt64 ? FArgIR::Int64 : FArgIR::Int;
            Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary",
                                            bFloat ? "Subtract_FloatFloat" : bInt64 ? "Subtract_Int64Int64" : "Subtract_IntInt");
            Out.Sub->Args = { Zero, A };
            return true;
        }
        if (Op != "!") { *Err = "TODO: unimplemented unary operator " + Op; return false; }

        Out.K = FArgIR::Call;
        Out.Sub = std::make_shared<FCallIR>();
        Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", "Not_PreBool");
        Out.Sub->bScript = false;

        FArgIR A;
        if (!LowerArg(*Operand, BP, A, Err)) return false;
        Out.Sub->Args.push_back(A);
        return true;
    }
    if (K == "BinaryOperator")
    {
        const std::string Op = N->value("opcode", std::string());
        const Json* LhsRaw = Nth(*N, 0);
        const Json* RhsRaw = Nth(*N, 1);
        if (!LhsRaw || !RhsRaw) { *Err = "binary operator with a missing side"; return false; }
        /* `"Kills: " + N`: C++ reads an address N characters into the literal (clang warns, -Wstring-plus-int, and
           compiles it), which nothing in a Blueprint could mean. It is the concat the author wrote. A float or an
           object on the other side never gets here - clang refuses those, so they stay `FString("lit") + X`. */
        if (const Json *L = Strip(LhsRaw), *R = Strip(RhsRaw); Op == "+" && L && R
            && (Kind(*L) == "StringLiteral") != (Kind(*R) == "StringLiteral"))
        {
            Out.K = FArgIR::Call;
            Out.InnerType = "FString";
            Out.Sub = std::make_shared<FCallIR>();
            Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetStringLibrary", "Concat_StrStr");
            for (const Json* Side : { LhsRaw, RhsRaw })
            {
                FArgIR A;
                if (!LowerArg(*Side, BP, A, Err) || !ConvertArg("FString", BP, A, Err)) return false;
                Out.Sub->Args.push_back(A);
            }
            return true;
        }
        /* Pointer arithmetic counts elements: P + N moves N * sizeof(*P) bytes, P - Q counts the elements between. */
        const std::string LP = PointeeOf(*LhsRaw), RP = PointeeOf(*RhsRaw);
        if ((Op == "+" || Op == "-") && (!LP.empty() || !RP.empty()))
        {
            auto Math = [&](const char* Fn, FArgIR A, FArgIR B) {
                FArgIR C;
                C.K = FArgIR::Call;
                C.InnerType = "int64";
                C.Sub = std::make_shared<FCallIR>();
                C.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", Fn);
                C.Sub->Args = { std::move(A), std::move(B) };
                return C;
            };
            FArgIR Ptr, Other;
            if (!LP.empty() && !RP.empty())
            {
                int32 Size = 0, Align = 0;
                if (!LayoutOf(LP, &Size, &Align, Err) || !LowerArg(*LhsRaw, BP, Ptr, Err) || !LowerArg(*RhsRaw, BP, Other, Err))
                    return false;
                Out = Math("Subtract_Int64Int64", std::move(Ptr), std::move(Other));
                if (Size == 1) return true;
                FArgIR Bytes;
                Bytes.K = FArgIR::Int64;
                Bytes.I64 = Size;
                Out = Math("Divide_Int64Int64", std::move(Out), std::move(Bytes));
                return true;
            }
            const bool bPtrLeft = !LP.empty();
            if (!LowerArg(bPtrLeft ? *LhsRaw : *RhsRaw, BP, Ptr, Err)
                || !ScaleIndex(bPtrLeft ? *RhsRaw : *LhsRaw, bPtrLeft ? LP : RP, BP, Other, Err)) return false;
            Out = Math(Op == "+" ? "Add_Int64Int64" : "Subtract_Int64Int64", std::move(Ptr), std::move(Other));
            return true;
        }
        if (Op == "&&" || Op == "||")
        {
            /* Short-circuit: the hoist pass turns this into `T = L; if (T) T = R;` (or `if (!T)`), so R's calls
               and reads only run when C++ would run them. BooleanAND / BooleanOR evaluate both operands first, so
               they are used only when IsEagerSafe proves R harmless: "pure" is not enough (`A && *A`,
               `X != 0 && 10 / X`, an out-of-range Get). FlowTest SafeRatio / EitherZero / WhileAnd divide by zero
               in runscript if the right side runs eagerly. */
            Out.K = FArgIR::Call;
            Out.InnerType = "bool";
            Out.Sub = std::make_shared<FCallIR>();
            if (!bCurNoOpt && IsEagerSafe(*RhsRaw))
            {
                /* Nothing on the right can fault or act, so running it anyway is unobservable: one native call
                   instead of a temp, a store and a jump. */
                Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", Op == "&&" ? "BooleanAND" : "BooleanOR");
                Out.Sub->bScript = false;
            }
            else
                Out.Sub->Intrinsic = Op == "&&" ? "__AndAlso__" : "__OrElse__";
            for (const Json* Side : { LhsRaw, RhsRaw })
            {
                FArgIR A;
                if (!LowerArg(*Side, BP, A, Err) || !ConvertArg("bool", BP, A, Err)) return false;
                Out.Sub->Args.push_back(std::move(A));
            }
            return true;
        }
        /* Flavour is read from the UNSTRIPPED sides: clang's promotion cast carries the common type. */
        const std::string LhsTy = TypeOf(*LhsRaw), RhsTy = TypeOf(*RhsRaw);
        std::string Flavour;
        const std::string LhsC = Canon(LhsTy), RhsC = Canon(RhsTy);
        if (IsObjectType(LhsTy) || IsObjectType(RhsTy))      Flavour = "ObjectObject";
        else if (LhsC == "float" || RhsC == "float")         Flavour = "FloatFloat";
        else if (IsInt64Type(LhsTy) || IsInt64Type(RhsTy))   Flavour = "Int64Int64";
        else if (LhsC == "uint8" && RhsC == "uint8")         Flavour = "ByteByte";
        else if (LhsC == "bool" && RhsC == "bool")           Flavour = "BoolBool";
        else                                                 Flavour = "IntInt";
        if (Op == "<<" || Op == ">>")
        {
            if (Kind(*RhsRaw) != "IntegerLiteral")
            { *Err = "bit shift with a non-constant amount (Kismet has no shift op; use explicit multiply/divide)"; return false; }
            const int N = std::stoi(RhsRaw->value("value", std::string("0")));
            if (N < 0 || N > 63)
            { *Err = "bit shift amount out of range: " + std::to_string(N); return false; }
            const bool bWide = Flavour == "Int64Int64";
            const std::string MathFn = (Op == "<<" ? "Multiply_" : "Divide_") + std::string(bWide ? "Int64Int64" : "IntInt");
            Out.K = FArgIR::Call;
            Out.Sub = std::make_shared<FCallIR>();
            Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", MathFn);
            Out.Sub->bScript = false;
            Out.Sub->bPure = true;
            FArgIR LA, RA;
            if (!LowerArg(*LhsRaw, BP, LA, Err)) return false;
            RA.K = bWide ? FArgIR::Int64 : FArgIR::Int;
            RA.I = int32(1LL << N);
            RA.I64 = 1LL << N;
            Out.Sub->Args.push_back(std::move(LA));
            Out.Sub->Args.push_back(std::move(RA));
            return true;
        }
        const std::string MathFn = MathFuncFor(Op, Flavour);
        if (MathFn.empty())
        { *Err = "TODO: unimplemented binary operator " + Op + " on " + Flavour; return false; }

        Out.K = FArgIR::Call;
        Out.Sub = std::make_shared<FCallIR>();
        Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", MathFn);
        Out.Sub->bScript = false;
        Out.Sub->bPure = true;

        FArgIR LA, RA;
        if (!LowerArg(*LhsRaw, BP, LA, Err)) return false;
        if (!LowerArg(*RhsRaw, BP, RA, Err)) return false;
        Out.Sub->Args.push_back(LA);
        Out.Sub->Args.push_back(RA);
        return true;
    }

    if (K == "ConditionalOperator")
    {
        /* `C ? A : B`: the hoist pass turns this into `if (C) T = A; else T = B;`. */
        Out.K = FArgIR::Call;
        Out.InnerType = TypeOf(*N);
        Out.Sub = std::make_shared<FCallIR>();
        Out.Sub->Intrinsic = "__Select__";
        for (size_t I = 0; I < 3; ++I)
        {
            const Json* Part = Nth(*N, I);
            FArgIR A;
            if (!Part || !LowerArg(*Part, BP, A, Err)) return false;
            if (I == 0 && !ConvertArg("bool", BP, A, Err)) return false;
            Out.Sub->Args.push_back(std::move(A));
        }
        return true;
    }

    if (K == "InitListExpr") return LowerMakeStruct(StripTypeKeywords(TypeOf(*N)), N, BP, Out, Err);

    *Err = "TODO: unimplemented argument " + K;
    return false;
}

bool FCompiler::LowerCall(const Json& CallExprNode, FBlueprintClass& BP, FCallIR& Out, std::string* Err)
{
    const std::string K = Kind(CallExprNode);
    const Json* Callee = Strip(First(CallExprNode));
    if (!Callee) { *Err = "call with no callee"; return false; }

    std::string DeclId, MethodName;
    const Json* FullDecl = nullptr;     // the UFunction's own signature, whichever overload was called
    if (K == "CXXMemberCallExpr")
    {
        if (Kind(*Callee) != "MemberExpr") { *Err = "TODO: unimplemented callee " + Kind(*Callee); return false; }
        DeclId = Callee->value("referencedMemberDecl", std::string());
        MethodName = Name(*Callee);
    }
    else
    {
        if (Kind(*Callee) != "DeclRefExpr") { *Err = "TODO: unimplemented callee " + Kind(*Callee); return false; }
        const Json& Ref = (*Callee)["referencedDecl"];
        DeclId = Ref.value("id", std::string());
        MethodName = Name(Ref);
    }

    /* __NAME__ free functions are compiler intrinsics; each resolves its imports here (Extra/Extra2). */
    const bool bIntrinsic = MethodName.size() >= 5
        && MethodName.compare(0, 2, "__") == 0
        && MethodName.compare(MethodName.size() - 2, 2, "__") == 0;
    if (bIntrinsic)
    {
        Out.Intrinsic = MethodName;
        if (MethodName == "__AddrOf__")
        {
            /* Donor: any struct with an 8-byte scalar at offset 0. FDateTime.Ticks is not reflected in shipping. */
            Out.Extra = BP.ScriptStruct("/Script/Engine", "ScreenMessageString");
        }
        else if (MethodName == "__RefAt__") {}   // resolved by HoistRefAt
        else if (MethodName == "__Asm__") {}     // bytes and MemBytes ride in Out.Args
        else if (MethodName == "__Await__") return LowerAwait(CallExprNode, BP, Out, Err);
        else if (FindReadView(MethodName))
        {
            /* Placeholder: the hoist pass rewrites each __Read*__ call into two statements
               (write scratch.Data; then __DerefRead*__ into a fresh temp), setting Extra/Extra2
               to the view struct and __DerefScratch__ import there. Nothing to resolve here. */
        }
        else if (MethodName == "__NameIndex__")
        {
            /* First 4 bytes of the FName slot = ComparisonIndex; no class check involved. */
            Out.Extra = BP.ScriptStruct("/Script/CoreUObject", "IntPoint");
        }
        else
        {
            *Err = "TODO: unimplemented intrinsic " + MethodName;
            return false;
        }
    }
    else
    {
        if (auto Free = FreeInlines.find(DeclId); Free != FreeInlines.end())
            return ExpandInline(CallExprNode, *Free->second, MethodName, false, BP, Out, Err);
        auto Owner = MethodOwner.find(DeclId);
        if (Owner == MethodOwner.end()) { *Err = "call to an unknown function: " + MethodName; return false; }
        const FRecord* R = Find(Owner->second);
        if (!R) { *Err = "call to a function on an unknown class: " + Owner->second; return false; }

        auto Decl = R->Methods.find(MethodName);
        const bool bStatic = Decl != R->Methods.end() && IsStaticDecl(*Decl->second);
        if (Decl != R->Methods.end()) FullDecl = Decl->second;
        if (Decl != R->Methods.end())
        {
            auto Def = R->MethodDefs.find(MethodName);
            bool bOutParm = false;
            ForEach(*Decl->second, [&](const Json& C) {
                const std::string T = TypeOf(C);
                if (Kind(C) == "ParmVarDecl" && !T.empty() && T.back() == '&' && T.compare(0, 6, "const ") != 0) bOutParm = true;
            });
            Out.bPure = !bOutParm && (IsPureDecl(*Decl->second) || (Def != R->MethodDefs.end() && IsPureDecl(*Def->second)));
        }
        if (Decl != R->Methods.end() && IsInlineMethod(*R, MethodName))
        {
            auto DefIt = R->MethodDefs.find(MethodName);
            return ExpandInline(CallExprNode, DefIt != R->MethodDefs.end() ? *DefIt->second : *Decl->second,
                                R->CppName + "::" + MethodName, true, BP, Out, Err);
        }

        if (!R->IsNative() && !bStatic)
        {
            Out.VirtualName = UeNameOf(R, MethodName);      // an override of `Set is Extruded` is found by that name
            /* KismetCompilerVMBackend.cpp picks the local form unless the callee is native, a net function, authority
               only or cosmetic. A method declared only by mod classes, without an RPC marker, is none of those; an
               override of a native function keeps whatever flags it inherits, so it stays EX_VirtualFunction. */
            bool bLocal = true;
            for (const FRecord* A = R; A && bLocal; A = A->Base.empty() ? nullptr : Find(A->Base))
            {
                auto M = A->Methods.find(MethodName);
                if (M == A->Methods.end()) continue;
                if (A->IsNative() || NetFlagsOf(*M->second) || AccessFlagsOf(*M->second)) bLocal = false;
                if (auto D = A->MethodDefs.find(MethodName); D != A->MethodDefs.end() && (NetFlagsOf(*D->second) || AccessFlagsOf(*D->second)))
                    bLocal = false;
            }
            Out.bLocalVirtual = bLocal;
        }

        const std::string CalleePackage = R->IsNative() ? R->UePackage
                                                        : ModPackage + "/" + R->CppName;
        const std::string CalleeName    = R->IsNative() ? R->UeName
                                                        : R->CppName + "_C";
        Out.Fn = BP.EngineFunction(CalleePackage, CalleeName, UeNameOf(R, MethodName));
        Out.bScript = CalleePackage.compare(0, 6, "/Game/") == 0;
        Out.bInstance = !bStatic;
        /* A Blueprint static needs the CDO context; EX_CallMath finds it itself for a native. */
        if (Out.bScript && bStatic)
            Out.Context = BP.ClassDefaultObject(CalleePackage, CalleeName);
    }

    /* inner[0] is the callee. */
    bool bFirst = true, bOk = true;
    std::vector<bool> Defaulted;
    ForEach(CallExprNode, [&](const Json& C) {
        if (bFirst) { bFirst = false; return; }
        if (!bOk) return;
        FArgIR A;
        bOk = LowerArg(C, BP, A, Err);
        if (bOk) Out.Args.push_back(A);
        Defaulted.push_back(Kind(C) == "CXXDefaultArgExpr");
    });
    if (!bOk || !FullDecl) return bOk;

    /* A world context the call leaves out - to its default, or through genueapi's overload without
       it - is wired like the Blueprint editor wires the hidden pin: self, or the enclosing static's
       own world context parameter, since a static's self is a CDO with no world. */
    const std::vector<std::string> Parms = ParmNames(*FullDecl);
    /* The mod never writes a latent call's FLatentActionInfo: genueapi's overloads leave it out, and it is filled in
       here, after the world context. */
    size_t LatentAt = Parms.size();
    {
        size_t I = 0;
        ForEach(*FullDecl, [&](const Json& C) {
            if (Kind(C) != "ParmVarDecl") return;
            if (StripTypeKeywords(TypeOf(C)) == "FLatentActionInfo") LatentAt = I;
            ++I;
        });
    }
    const size_t Hidden = LatentAt < Parms.size() ? 1 : 0;
    if (Hidden && Out.Args.size() == Parms.size())
    { *Err = MethodName + ": leave the FLatentActionInfo argument out, the compiler supplies it"; return false; }
    /* `auto Cls = LoadAssetClass(Soft)`: genueapi's overload that leaves out a one-parameter completion delegate
       returns that parameter, where the UFunction returns nothing. clang's type drops the parameter's name. */
    size_t ResultAt = Parms.size();
    std::string ResultType;
    if (Hidden && StripTypeKeywords(TypeOf(CallExprNode)) != "void")
    {
        size_t I = 0;
        ForEach(*FullDecl, [&](const Json& C) {
            if (Kind(C) != "ParmVarDecl") return;
            const std::string T = StripTypeKeywords(TypeOf(C));
            const size_t Open = T.find('('), Close = T.rfind(")>");
            if (T.compare(0, 10, "TDelegate<") == 0 && Open != std::string::npos && Close != std::string::npos && Close > Open + 1)
            { ResultAt = I; ResultType = T.substr(Open + 1, Close - Open - 1); }
            ++I;
        });
        if (ResultAt == Parms.size()) { *Err = "internal: " + MethodName + " returns a value but has no completion delegate"; return false; }
    }
    const size_t Omitted = Hidden + (ResultAt < Parms.size() ? 1 : 0);
    for (size_t I = 0; I < Parms.size(); ++I)
    {
        if (!IsWcoName(Parms[I])) continue;
        FArgIR Wco;
        if (!CurrentWco.empty()) { Wco.K = FArgIR::Local; Wco.S = CurrentWco; }
        if (Out.Args.size() + 1 + Omitted == Parms.size()) Out.Args.insert(Out.Args.begin() + I, Wco);
        else if (I < Defaulted.size() && Defaulted[I]) Out.Args[I] = Wco;
        break;
    }
    if (Hidden)
    {
        if (!LatentRefusal.empty()) { *Err = "latent call " + MethodName + ": " + LatentRefusal; return false; }
        std::vector<std::pair<size_t, FArgIR>> Fill;
        FArgIR L;
        L.K = FArgIR::LatentInfo;
        L.Owner = BP.ScriptStruct("/Script/Engine", "LatentActionInfo");
        L.S = "ExecuteUbergraph_" + Cur->CppName;
        /* The latent action manager keys pending actions by UUID per callback target: one per call site. */
        L.I = int32(std::hash<std::string>{}(Cur->CppName + "." + CurFnName + "#" + std::to_string(++LatentCount)));
        Fill.emplace_back(LatentAt, L);

        std::string ResultLocal;
        if (ResultAt < Parms.size())
        {
            /* The event is found by name on self, so it must not be a function of the class or an ancestor. */
            const std::string Event = FreshEventName(CurFnName + "_" + Parms[ResultAt]);
            FCompletion C;
            C.Event = Event;
            ResultLocal = "__Async" + std::to_string(ReadTmpCounter++) + "__";
            C.Local = ResultLocal;
            FPropertyDef Local;
            C.Parms.emplace_back();
            if (!TypeToProperty(ResultType, "Value", 0, "the result of " + MethodName, BP, &C.Parms[0], Err)
                || !TypeToProperty(ResultType, ResultLocal, 0, "the result of " + MethodName, BP, &Local, Err)) return false;
            Local.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
            CurLocals->push_back(Local);
            Completions.push_back(C);
            FArgIR D;
            D.K = FArgIR::Delegate;
            D.S = Event;
            Fill.emplace_back(ResultAt, D);
        }
        std::sort(Fill.begin(), Fill.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
        for (const auto& [At, Arg] : Fill) Out.Args.insert(Out.Args.begin() + std::min(At, Out.Args.size()), Arg);
        bMadeLatentCall = true;

        if (!ResultLocal.empty())
        {
            /* The call, then its value: the local the completion event filled in before the resume. */
            auto Block = std::make_shared<std::vector<FStmtIR>>(1);
            (*Block)[0].K = FStmtIR::Block;
            (*Block)[0].Body = std::make_shared<std::vector<FStmtIR>>(1);
            (*(*Block)[0].Body)[0].K = FStmtIR::StaticCall;
            (*(*Block)[0].Body)[0].Call = Out;
            Out = FCallIR();
            Out.Intrinsic = "__Inline__";
            Out.Inline = Block;
            Out.InlineResult = ResultLocal;
            Out.InlineType = ResultType;
        }
    }
    return true;
}

namespace
{
/*
A constant worth using in place of a variable that holds it. Scalars only: their const opcode is smaller than the
field path an EX_LocalVariable read carries and needs no property indirection at run time, while a string or a
container const would be rebuilt (and reallocated) at every use, which is what the variable was saving.
*/
bool IsFoldableConst(const FArgIR& A)
{
    switch (A.K)
    {
    case FArgIR::Int: case FArgIR::Int64: case FArgIR::Float: case FArgIR::Bool:
    case FArgIR::Byte: case FArgIR::Self: case FArgIR::NullObj: case FArgIR::ObjConst:
        return true;
    default:
        return false;
    }
}

/*
A value a statement may throw away. An unused statement value goes to the VM's 64-byte scratch buffer
(ProcessLocalScriptFunction: "No POD struct can ever be stored in this buffer"), which is raw stack memory that
nothing constructs, so only a trivially assignable type may land there; an FString or a container would be assigned
over garbage. Every type below fits the buffer.
*/
bool IsDiscardable(const FPropertyDef& P)
{
    static const std::set<std::string> Simple = { "IntProperty", "Int64Property", "FloatProperty", "BoolProperty",
                                                  "ByteProperty", "EnumProperty", "NameProperty", "ObjectProperty",
                                                  "ClassProperty" };
    return Simple.count(P.Type) != 0;
}

/* Whether evaluating an expression can change anything: a call that is not bPure, an intrinsic, a delegate. */
bool CallsImpure(const FArgIR& A);
bool CallsImpure(const FCallIR& C)
{
    if (!C.bPure || !C.Intrinsic.empty() || C.Inline) return true;
    if (C.Target && CallsImpure(*C.Target)) return true;
    return std::any_of(C.Args.begin(), C.Args.end(), [](const FArgIR& A) { return CallsImpure(A); });
}
bool CallsImpure(const FArgIR& A)
{
    switch (A.K)
    {
    case FArgIR::Call: return !A.Sub || CallsImpure(*A.Sub);
    case FArgIR::Delegate: case FArgIR::InterfaceCtx: case FArgIR::LatentInfo: case FArgIR::StructLit: return true;
    default: break;
    }
    if (A.Base && CallsImpure(*A.Base)) return true;
    return A.K == FArgIR::Index && A.Sub
        && std::any_of(A.Sub->Args.begin(), A.Sub->Args.end(), [](const FArgIR& I) { return CallsImpure(I); });
}
/* Every name a statement list may read. A whole-variable store to a Local (Assign / Decl) is the one mention that is
   not a read; anything else that names it, an index store, an out-argument, a string an intrinsic carries, is. */
void NamesRead(const FArgIR& A, std::set<std::string>& Out);
void NamesRead(const std::vector<FStmtIR>& Stmts, std::set<std::string>& Out, bool bStoresRead = false);
void NamesRead(const FCallIR& C, std::set<std::string>& Out)
{
    if (C.Inline) NamesRead(*C.Inline, Out, true);     // DropStores does not reach an expression's inline body
    Out.insert(C.InlineResult);
    Out.insert(C.View);
    if (C.Target) NamesRead(*C.Target, Out);
    for (const FArgIR& A : C.Args) NamesRead(A, Out);
}
void NamesRead(const FArgIR& A, std::set<std::string>& Out)
{
    Out.insert(A.S);
    if (A.Sub) NamesRead(*A.Sub, Out);
    if (A.Base) NamesRead(*A.Base, Out);
}
void NamesRead(const std::vector<FStmtIR>& Stmts, std::set<std::string>& Out, bool bStoresRead)
{
    for (const FStmtIR& St : Stmts)
    {
        NamesRead(St.Target, Out);
        NamesRead(St.Call, Out);
        if (bStoresRead || !((St.K == FStmtIR::Assign || St.K == FStmtIR::Decl) && St.Var.K == FArgIR::Local)) NamesRead(St.Var, Out);
        for (const FArgIR* A : { &St.Value, &St.Cond, &St.SwitchValue }) NamesRead(*A, Out);
        for (const FArgIR& A : St.CaseTests) NamesRead(A, Out);
        for (const auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer }) if (*L) NamesRead(**L, Out, bStoresRead);
    }
}

/* Removes the stores to a local in Unread; a value that calls something impure keeps its call as a statement,
   unless its result is one the VM cannot throw away (Constructed), in which case the whole store stays. */
void DropStores(std::vector<FStmtIR>& Stmts, const std::set<std::string>& Unread, const std::set<std::string>& Constructed)
{
    for (size_t I = 0; I < Stmts.size(); ++I)
    {
        FStmtIR& St = Stmts[I];
        for (auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer })
            if (*L)
            {
                *L = std::make_shared<std::vector<FStmtIR>>(**L);
                DropStores(**L, Unread, Constructed);
            }
        if (!(St.K == FStmtIR::Assign || St.K == FStmtIR::Decl) || St.Var.K != FArgIR::Local || !Unread.count(St.Var.S)) continue;
        if (!(St.K == FStmtIR::Decl && !St.bHasValue) && CallsImpure(St.Value))
        {
            if (St.Value.K != FArgIR::Call || !St.Value.Sub) continue;      // ponytail: kept whole; only a direct call is unwrapped
            if (Constructed.count(St.Var.S)) continue;                      // nowhere for the result to go but the local
            FStmtIR Call;
            Call.K = FStmtIR::StaticCall;
            Call.Call = *St.Value.Sub;
            St = std::move(Call);
            continue;
        }
        Stmts.erase(Stmts.begin() + I--);
    }
}
}   // namespace

/*
A local nothing reads is not compiled: its stores go, and the property with them. Only locals; a store to a property
of self or another object is never dropped, since something outside the function may read it.
*/
void FCompiler::DropUnusedLocals(std::vector<FStmtIR>& Stmts, std::vector<FPropertyDef>& Locals)
{
    std::set<std::string> Constructed;
    for (const FPropertyDef& L : Locals) if (!IsDiscardable(L)) Constructed.insert(L.Name);
    for (;;)    // dropping `X = Y` can leave Y unread
    {
        std::set<std::string> Read;
        NamesRead(Stmts, Read);
        for (const FCompletion& C : Completions) Read.insert(C.Local);     // its event stores there
        std::set<std::string> Unread;
        for (const FPropertyDef& L : Locals) if (!Read.count(L.Name)) Unread.insert(L.Name);
        if (Unread.empty()) return;
        DropStores(Stmts, Unread, Constructed);
        auto Mentioned = [&](const FPropertyDef& L) {
            if (!Unread.count(L.Name)) return true;
            bool bStored = false;
            std::function<void(const std::vector<FStmtIR>&)> Scan = [&](const std::vector<FStmtIR>& List) {
                for (const FStmtIR& St : List)
                {
                    if (St.Var.K == FArgIR::Local && St.Var.S == L.Name) bStored = true;
                    for (const auto* B : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer }) if (*B) Scan(**B);
                }
            };
            Scan(Stmts);
            return bStored;     // an impure store kept whole still needs its property
        };
        const size_t Before = Locals.size();
        Locals.erase(std::remove_if(Locals.begin(), Locals.end(), [&](const FPropertyDef& L) { return !Mentioned(L); }), Locals.end());
        if (Locals.size() == Before) return;
    }
}

/*
Drops a statement that only calls a pure function whose arguments call nothing impure: its value is unused, so
the call does nothing. Repeated pure calls are left alone; a C++ local already says "compute this once".
*/
void FCompiler::DropUnusedPure(std::vector<FStmtIR>& Stmts)
{
    for (size_t I = 0; I < Stmts.size(); ++I)
    {
        for (auto* L : { &Stmts[I].Then, &Stmts[I].Else, &Stmts[I].Body, &Stmts[I].Inc, &Stmts[I].Trailer })
            if (*L)
            {
                *L = std::make_shared<std::vector<FStmtIR>>(**L);
                DropUnusedPure(**L);
            }
        if (Stmts[I].K != FStmtIR::StaticCall) continue;
        if (!CallsImpure(Stmts[I].Call)) Stmts.erase(Stmts.begin() + I--);
    }
}

/* A generated event's name, <Stem>_<N>: found by name on self, so not a function of the class or an ancestor. */
std::string FCompiler::FreshEventName(const std::string& Stem)
{
    auto Taken = [&](const std::string& E) {
        if (GeneratedEvents.count(E)) return true;
        for (const FRecord* A = Cur; A; A = A->Base.empty() ? nullptr : Find(A->Base))
            if (A->Methods.count(E)) return true;
        return false;
    };
    std::string Event;
    for (int32 N = 0; Taken(Event = Stem + "_" + std::to_string(N)); ++N) {}
    GeneratedEvents.insert(Event);
    return Event;
}

/* UE_AWAIT(Obj->Dispatcher), UeMeta.h: bind a generated event, activate an async action, end the run. The event
   stores the payload and re-enters the ubergraph after the await, as BP_LiftPod's OnCompleted_* do. */
bool FCompiler::LowerAwait(const Json& CallNode, FBlueprintClass& BP, FCallIR& Out, std::string* Err)
{
    if (!LatentRefusal.empty()) { *Err = "UE_AWAIT: " + LatentRefusal; return false; }
    const Json* Arg = Strip(Nth(CallNode, 1));
    FArgIR Disp;
    if (!Arg || !LowerArg(*Arg, BP, Disp, Err)) return false;
    if (Disp.K != FArgIR::Field) { *Err = "UE_AWAIT takes a dispatcher property"; return false; }
    if (Disp.Base && Disp.Base->K != FArgIR::Local && Disp.Base->K != FArgIR::Field && Disp.Base->K != FArgIR::Self)
    { *Err = "UE_AWAIT: keep the object in a variable, it is used twice"; return false; }

    const std::string SigType = StripTypeKeywords(TypeOf(*Arg));
    const size_t Open = SigType.find('('), Close = SigType.rfind(")>");
    if (Open == std::string::npos || Close == std::string::npos || Close < Open)
    { *Err = "UE_AWAIT: cannot read the signature of " + SigType; return false; }
    const std::string ParmList = SigType.substr(Open + 1, Close - Open - 1);

    FCompletion C;
    C.Event = FreshEventName(CurFnName + "_" + Disp.S);
    C.Resume = std::make_shared<int32>(0);
    if (!ParmList.empty() && ParmList != "void")
        for (const std::string& T : SplitTemplateArgs(ParmList))
        {
            C.Parms.emplace_back();
            if (!TypeToProperty(T, "Value" + std::to_string(C.Parms.size() - 1), 0, "UE_AWAIT on " + Disp.S, BP, &C.Parms.back(), Err))
                return false;
        }
    const std::string ResultType = StripTypeKeywords(TypeOf(CallNode));
    if (ResultType != "void")
    {
        C.Local = "__Await" + std::to_string(ReadTmpCounter++) + "__";
        FPropertyDef Local;
        if (!TypeToProperty(ResultType, C.Local, 0, "UE_AWAIT on " + Disp.S, BP, &Local, Err)) return false;
        Local.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
        CurLocals->push_back(Local);
    }

    auto Body = std::make_shared<std::vector<FStmtIR>>();
    auto Add = [&](FCallIR Call) { Body->emplace_back(); Body->back().K = FStmtIR::StaticCall; Body->back().Call = std::move(Call); };
    FCallIR Bind;
    Bind.Intrinsic = "__AddDelegate__";
    Bind.Args.push_back(Disp);
    Bind.Args.emplace_back();
    Bind.Args.back().K = FArgIR::Delegate;
    Bind.Args.back().S = C.Event;
    Add(Bind);

    /* K2Node_AsyncAction activates a UBlueprintAsyncActionBase once its outputs are bound. Only the first await on a
       variable does: a second one waits on the action already running. */
    std::string ObjType = StripTypeKeywords(TypeOf(*Strip(First(*Arg))));
    while (!ObjType.empty() && (ObjType.back() == '*' || ObjType.back() == ' ')) ObjType.pop_back();
    bool bAction = false;
    for (const FRecord* A = Find(ObjType); A; A = A->Base.empty() ? nullptr : Find(A->Base))
        if (A->UeName == "BlueprintAsyncActionBase") bAction = true;
    if (bAction && ActivatedActions.insert(Disp.Base ? Disp.Base->S : std::string("this")).second)
    {
        FCallIR Activate;
        Activate.Fn = BP.EngineFunction("/Script/Engine", "BlueprintAsyncActionBase", "Activate");
        Activate.bInstance = true;
        if (Disp.Base) Activate.Target = Disp.Base;
        Add(Activate);
    }
    FCallIR Point;
    Point.Intrinsic = "__AwaitPoint__";
    Point.Resume = C.Resume;
    Add(Point);

    Completions.push_back(C);
    bMadeLatentCall = true;
    auto Block = std::make_shared<std::vector<FStmtIR>>(1);
    (*Block)[0].K = FStmtIR::Block;
    (*Block)[0].Body = Body;
    Out.Intrinsic = "__Inline__";
    Out.Inline = Block;
    Out.InlineResult = C.Local;
    Out.InlineType = ResultType;
    return true;
}

bool OnlyRead(const Json& N, const std::string& Id);
int32 UsesOf(const Json& N, const std::string& Id);

/*
CurBody is this body, restored on the way out: C++ scoping puts every use of a local inside the body that
declares it, so that is the whole scope ReadOnlyLocal has to read.
*/
bool FCompiler::LowerBody(const Json& Body, FBlueprintClass& BP, std::vector<FStmtIR>& Out,
                          std::vector<FPropertyDef>& Locals, std::string* Err)
{
    const Json* OuterBody = CurBody;
    CurBody = &Body;
    struct FRestore { const Json** Slot; const Json* Old; ~FRestore() { *Slot = Old; } } Restore{ &CurBody, OuterBody };

    bool bOk = true;
    ForEach(Body, [&](const Json& Raw) {
        if (!bOk) return;
        const Json* S = Strip(&Raw);
        if (!S) return;

        FStmtIR St;
        const FRecord* SetOn = nullptr;                 // an assignment to a property: the class it is read from,
        std::string SetField;                           // the property,
        std::shared_ptr<FArgIR> SetObject;              // and the object, null for self
        const std::string K = Kind(*S);
        if (K == "DeclStmt")
        {
            /* clang groups comma-declared vars under one DeclStmt. */
            bool bAny = false;
            ForEach(*S, [&](const Json& D) {
                const std::string DK = Kind(D);
                if (DK == "TypeAliasDecl" || DK == "TypedefDecl" || DK == "UsingDecl" || DK == "UsingEnumDecl"
                    || DK == "StaticAssertDecl") { bAny = true; return; }   // compile-time names only
                if (!bOk || DK != "VarDecl") return;
                bAny = true;
                const std::string VarName = LocalName(D);
                std::string VarType = TypeOf(D);
                FStmtIR Ds;
                const Json* RefInit = First(D) ? PeelLvalue(First(D)) : nullptr;
                if (!VarType.empty() && VarType.back() == '&' && RefInit && Kind(*RefInit) != "MaterializeTemporaryExpr")
                {
                    /* `T& R = V` is another name for V; `T& R = *P` keeps the address in an int64 local. A `const T& R`
                       bound to a temporary falls through to a copy, which lives as long. */
                    if (IsAliasable(*RefInit) && !IsDerefLvalue(*RefInit))
                    {
                        RefAlias[D.value("id", std::string())] = *RefInit;
                        return;
                    }
                    std::string Pointee;
                    if (!LowerAddress(*RefInit, BP, Ds.Value, &Pointee, Err))
                    { *Err = "reference " + VarName + ": " + *Err; bOk = false; return; }
                    RefAddr[D.value("id", std::string())] = Pointee;
                    VarType = "int64";
                    Ds.bHasValue = true;
                }
                while (!VarType.empty() && (VarType.back() == '&' || VarType.back() == ' ')) VarType.pop_back();
                VarType = StripTypeKeywords(VarType);
                FPropertyDef PD;
                if (!TypeToProperty(VarType, VarName, 0, "local " + VarName, BP, &PD, Err))
                { bOk = false; return; }
                PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);

                Ds.K = FStmtIR::Decl;
                Ds.Var.K = FArgIR::Local;
                Ds.Var.S = VarName;
                Ds.Var.LetOp = LetOpFor(VarType);
                const Json* Init = Ds.bHasValue ? nullptr : Strip(First(D));
                /* `FStats S;` carries an implicit argless CXXConstructExpr: no initialiser. */
                if (Init && Kind(*Init) == "CXXConstructExpr" && !First(*Init)) Init = nullptr;
                if (Init)
                {
                    Ds.bHasValue = true;
                    if (!LowerArg(*First(D), BP, Ds.Value, Err)) { bOk = false; return; }
                    /* A constant nothing writes is used in place, the way an inlined parameter is (ExpandInline):
                       the reads become the const and neither the property nor its store is compiled. A repeated
                       expansion of the same inline function reaches this declaration again, so the binding a
                       previous one left goes first. */
                    const std::string DeclId = D.value("id", std::string());
                    ParmConst.erase(DeclId);
                    if (!bCurNoOpt && IsFoldableConst(Ds.Value) && ReadOnlyLocal(DeclId))
                    {
                        ParmConst[DeclId] = Ds.Value;
                        return;
                    }
                }
                else if ((LoopDepth > 0 || bBodyHasGoto) && !Ds.bHasValue)
                {
                    /* The frame initialises a local once, on entry, but C++ constructs it again each time a loop
                       reaches the declaration. A twin nothing writes keeps the entry value to copy back. */
                    const std::string Fresh = "__Fresh" + VarName + "__";
                    if (std::none_of(Locals.begin(), Locals.end(), [&](const FPropertyDef& L) { return L.Name == Fresh; }))
                    {
                        FPropertyDef Twin;
                        if (!TypeToProperty(VarType, Fresh, 0, "local " + VarName, BP, &Twin, Err)) { bOk = false; return; }
                        Twin.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
                        Locals.push_back(Twin);
                    }
                    Ds.bHasValue = true;
                    Ds.Value.K = FArgIR::Local;
                    Ds.Value.S = Fresh;
                }
                Locals.push_back(PD);
                Out.push_back(Ds);
            });
            if (!bAny && bOk) { *Err = "a declaration statement declares nothing usable"; bOk = false; }
            return;
        }
        if (K == "CXXMemberCallExpr")
        {
            /* Same lowering as a call used for its value; the target rides in FCallIR::Target. */
            FArgIR V;
            St.K = FStmtIR::StaticCall;
            bOk = LowerArgRaw(*S, TypeOf(*S), BP, V, Err);
            if (bOk) St.Call = *V.Sub;
            if (bOk && St.Call.Inline) { for (FStmtIR& B : *St.Call.Inline) Out.push_back(std::move(B)); return; }
        }
        else if (K == "CallExpr")
        {
            bOk = LowerCall(*S, BP, St.Call, Err);
            if (bOk && St.Call.Inline) { for (FStmtIR& B : *St.Call.Inline) Out.push_back(std::move(B)); return; }
        }
        else if (K == "CompoundAssignOperator"
              || (K == "UnaryOperator" && (S->value("opcode", std::string()) == "++" || S->value("opcode", std::string()) == "--")))
        {
            /* `X op= Y` / `++X` / `X--` as statements are `X = X op Y` / `X = X +- 1`. X is evaluated twice, which only
               matters for a destination with side effects (`Items[Next()] += 1`). */
            const std::string Op = S->value("opcode", std::string());
            const bool bStep = K == "UnaryOperator";
            const Json* Lhs = Nth(*S, 0);
            if (!Lhs) { *Err = "`" + Op + "` with no destination"; bOk = false; return; }
            const Json LhsType = Lhs->value("type", Json::object());
            const bool bPointer = !PointeeOf(*Lhs).empty();
            std::string OpType = bStep ? TypeOf(*Lhs) : S->value("computeLHSType", Json::object()).value("qualType", TypeOf(*Lhs));
            if (bStep && !bPointer)
            {
                const std::string C = Canon(OpType);
                OpType = C == "float" ? "float" : IsInt64Type(OpType) ? "int64" : "int";
            }
            Json Read = { {"kind", "ImplicitCastExpr"}, {"castKind", "LValueToRValue"}, {"type", LhsType}, {"inner", Json::array({*Lhs})} };
            if (!bPointer && StripTypeKeywords(OpType) != StripTypeKeywords(TypeOf(*Lhs)))
                Read = { {"kind", "ImplicitCastExpr"}, {"castKind", "IntegralCast"}, {"type", {{"qualType", OpType}}}, {"inner", Json::array({Read})} };
            Json Rhs = bStep ? Json{ {"kind", "IntegerLiteral"}, {"type", {{"qualType", "int"}}}, {"value", "1"} } : *Nth(*S, 1);
            if (bStep && !bPointer && OpType != "int")
                Rhs = { {"kind", "ImplicitCastExpr"}, {"castKind", "IntegralCast"}, {"type", {{"qualType", OpType}}}, {"inner", Json::array({Rhs})} };
            const std::string BinOp = bStep ? std::string(Op == "++" ? "+" : "-") : Op.substr(0, Op.size() - 1);
            Json Value = { {"kind", "BinaryOperator"}, {"opcode", BinOp}, {"type", bPointer ? LhsType : Json{{"qualType", OpType}}},
                           {"inner", Json::array({Read, Rhs})} };
            if (!bPointer && StripTypeKeywords(OpType) != StripTypeKeywords(TypeOf(*Lhs)))
                Value = { {"kind", "ImplicitCastExpr"}, {"castKind", "IntegralCast"}, {"type", LhsType}, {"inner", Json::array({Value})} };
            Json Assign = { {"kind", "BinaryOperator"}, {"opcode", "="}, {"type", LhsType}, {"inner", Json::array({*Lhs, Value})} };
            Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({Assign})} };
            bOk = LowerBody(Wrap, BP, Out, Locals, Err);
            return;
        }
        else if ((K == "BinaryOperator" && S->value("opcode", std::string()) == "=")
              || K == "CXXOperatorCallExpr")
        {
            /* Scalar `=` is a BinaryOperator [LHS, RHS]; class-type `=` is a CXXOperatorCallExpr
               [operator=, LHS, RHS]. */
            const Json* Lhs = nullptr;
            const Json* Rhs = nullptr;
            if (K == "BinaryOperator")
            {
                Lhs = Strip(Nth(*S, 0));
                Rhs = Nth(*S, 1);           // unstripped: its outer type is the destination's
            }
            else
            {
                const Json* Callee = Strip(Nth(*S, 0));
                const std::string OpName = (Callee && Callee->contains("referencedDecl"))
                    ? (*Callee)["referencedDecl"].value("name", std::string())
                    : std::string();
                if (OpName != "operator=")
                { *Err = "TODO: unimplemented operator overload " + OpName; bOk = false; return; }
                Lhs = Strip(Nth(*S, 1));
                Rhs = Nth(*S, 2);
            }
            if (!Lhs || !Rhs) { *Err = "assignment with a missing side"; bOk = false; return; }

            /* A reference local is the variable it names, or the memory at the address it keeps. */
            while (Kind(*Lhs) == "DeclRefExpr")
            {
                auto A = RefAlias.find((*Lhs)["referencedDecl"].value("id", std::string()));
                if (A == RefAlias.end()) break;
                Lhs = Strip(&A->second);
            }
            const std::string LK = Kind(*Lhs);
            if (IsDerefLvalue(*Lhs))
            {
                FArgIR Addr;
                std::string Pointee;
                St.K = FStmtIR::Assign;
                bOk = LowerAddress(*Lhs, BP, Addr, &Pointee, Err) && RefThrough(std::move(Addr), Pointee, St.Var, Err)
                   && LowerArg(*Rhs, BP, St.Value, Err);
            }
            else if (LK == "MemberExpr" && First(*Lhs) && IsTMapElement(*Strip(First(*Lhs))))
            {
                *Err = "`Map[Key].Member = v` would change a copy: read Map[Key] into a local, change it, store it back";
                bOk = false;
                return;
            }
            else if (LK == "MemberExpr")
            {
                St.K = FStmtIR::Assign;
                bOk = LowerField(*Lhs, BP, St.Var, Err) && LowerArg(*Rhs, BP, St.Value, Err);
                if (bOk) { SetOn = RecordOfFieldAccess(*Lhs); SetField = St.Var.S; SetObject = St.Var.Base; }
            }
            else if (LK == "DeclRefExpr")
            {
                const Json& Ref = (*Lhs)["referencedDecl"];
                const std::string RefKind = Ref.value("kind", std::string());
                if (RefKind != "ParmVarDecl" && RefKind != "VarDecl")
                { *Err = "TODO: assignment to a DeclRefExpr of kind " + RefKind; bOk = false; return; }
                const std::string RefName = LocalName(Ref);
                const bool bOut = CurrentOutParms.count(RefName) != 0;
                St.K = FStmtIR::Assign;
                St.Var.K = bOut ? FArgIR::LocalOut : FArgIR::Local;
                St.Var.S = RefName;
                St.Var.LetOp = LetOpFor(TypeOf(*Lhs));
                St.bAssignLocal = !bOut;
                St.bAssignOutParm = bOut;
                bOk = LowerArg(*Rhs, BP, St.Value, Err);
            }
            else if (IsTMapElement(*Lhs))
            {
                /* `Map[Key] = v`: Map_Add, which replaces the value of a key already there. */
                St.K = FStmtIR::StaticCall;
                St.Call.Fn = BP.EngineFunction("/Script/Engine", "BlueprintMapLibrary", "Map_Add");
                St.Call.Args.resize(3);
                bOk = LowerArg(*Nth(*Lhs, 1), BP, St.Call.Args[0], Err) && LowerArg(*Nth(*Lhs, 2), BP, St.Call.Args[1], Err)
                   && LowerArg(*Rhs, BP, St.Call.Args[2], Err);
                if (bOk && St.Call.Args[0].K != FArgIR::Field && St.Call.Args[0].K != FArgIR::Local
                    && St.Call.Args[0].K != FArgIR::LocalOut && St.Call.Args[0].K != FArgIR::Member)
                { *Err = "`[]` on a map needs a map variable, not a computed value"; bOk = false; }
                if (bOk) WarnRpcRefWrite(St.Call.Args[0]);
                if (bOk && St.Call.Args[0].K == FArgIR::Field && Strip(Nth(*Lhs, 1)) && Kind(*Strip(Nth(*Lhs, 1))) == "MemberExpr")
                { SetOn = RecordOfFieldAccess(*Strip(Nth(*Lhs, 1))); SetField = St.Call.Args[0].S; SetObject = St.Call.Args[0].Base; }
            }
            else if (LK == "CXXOperatorCallExpr")
            {
                /* `Items[i] = v`: the destination is an array element. */
                St.K = FStmtIR::Assign;
                if (!LowerArg(*Lhs, BP, St.Var, Err)) { bOk = false; return; }
                /* A nested container element is written whole: the wrapper has the container's layout. */
                if (St.Var.K == FArgIR::Member && St.Var.S == "Value" && St.Var.Base && St.Var.Base->K == FArgIR::Index)
                { const FArgIR Element = *St.Var.Base; St.Var = Element; }
                if (St.Var.K != FArgIR::Index)
                { *Err = "TODO: assignment to an operator call that is not an array element"; bOk = false; return; }
                if (St.Var.Base->K == FArgIR::Field && Strip(Nth(*Lhs, 1)) && Kind(*Strip(Nth(*Lhs, 1))) == "MemberExpr")
                { SetOn = RecordOfFieldAccess(*Strip(Nth(*Lhs, 1))); SetField = St.Var.Base->S; SetObject = St.Var.Base->Base; }
                St.bAssignLocal = St.Var.Base->K == FArgIR::Local;
                St.bAssignOutParm = St.Var.Base->K == FArgIR::LocalOut;
                bOk = LowerArg(*Rhs, BP, St.Value, Err);
            }
            else
            {
                *Err = "TODO: assignment to " + LK + ", not a property or local";
                bOk = false;
                return;
            }
        }
        else if (K == "ReturnStmt" && !InlineResults.empty())
        {
            /* In an inline body: store the value, then jump past the body. */
            if (First(*S))
            {
                FStmtIR Store;
                Store.K = FStmtIR::Assign;
                Store.Var.K = FArgIR::Local;
                Store.Var.S = InlineResults.back().first;
                Store.Var.LetOp = LetOpFor(InlineResults.back().second);
                Store.bAssignLocal = true;
                if (!LowerArg(*First(*S), BP, Store.Value, Err)) { bOk = false; return; }
                Out.push_back(std::move(Store));
            }
            St.K = FStmtIR::InlineReturn;
        }
        else if (K == "ReturnStmt")
        {
            St.K = FStmtIR::Return;
            if (First(*S))
            {
                St.bHasValue = true;
                bOk = LowerArg(*First(*S), BP, St.Value, Err);
            }
        }
        else if (K == "IfStmt")
        {
            /* IfStmt inner is [cond, then, else?]; an init-stmt would prepend one. */
            if (S->value("hasInit", false) || S->value("hasVar", false))
            {
                if (!LowerWithoutPrefix(*S, BP, Out, Locals, Err)) bOk = false;
                return;
            }
            const Json* Cond = Nth(*S, 0);
            const Json* Then = Nth(*S, 1);
            const Json* Else = Nth(*S, 2);
            if (!Cond || !Then) { *Err = "`if` with a missing condition or then-branch"; bOk = false; return; }

            /* `if (A && B) S` with no else is `if (A) if (B) S`: two jumps, no temp, no call, whatever B is. With an
               else, the else would be duplicated, so that one goes through the && lowering. */
            if (const Json* And = Strip(Cond); !Else && Kind(*And) == "BinaryOperator" && And->value("opcode", std::string()) == "&&")
            {
                Json Inner = { {"kind", "IfStmt"}, {"inner", Json::array({ *Nth(*And, 1), *Then })} };
                Json Outer = { {"kind", "IfStmt"}, {"inner", Json::array({ *Nth(*And, 0), Inner })} };
                Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({ Outer })} };
                if (!LowerBody(Wrap, BP, Out, Locals, Err)) bOk = false;
                return;
            }

            St.K = FStmtIR::If;
            if (!LowerArg(*Cond, BP, St.Cond, Err)) { bOk = false; return; }

            /* A single-statement branch is wrapped in a synthetic CompoundStmt. */
            auto LowerBranch = [&](const Json& Branch, std::shared_ptr<std::vector<FStmtIR>>& OutBody) -> bool
            {
                OutBody = std::make_shared<std::vector<FStmtIR>>();
                if (Kind(Branch) == "CompoundStmt") return LowerBody(Branch, BP, *OutBody, Locals, Err);
                Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({Branch})} };
                return LowerBody(Wrap, BP, *OutBody, Locals, Err);
            };
            if (!LowerBranch(*Then, St.Then)) { bOk = false; return; }
            if (Else && !LowerBranch(*Else, St.Else)) { bOk = false; return; }
        }
        else if (K == "BreakStmt" || K == "ContinueStmt")
        {
            if (LoopDepth == 0 && (K == "ContinueStmt" || SwitchDepth == 0))
            { *Err = K == "BreakStmt" ? "`break` outside a loop or switch" : "`continue` outside a loop"; bOk = false; return; }
            St.K = K == "BreakStmt" ? FStmtIR::Break : FStmtIR::Continue;
        }
        else if (K == "SwitchStmt")
        {
            /* SwitchStmt inner is [cond, body]. The value lands in a temp once. Each CaseStmt [ConstantExpr, sub]
               or DefaultStmt [sub] becomes a Label followed by its first statement, so every label sits at the top
               level of the body, where the emitter looks for them. The default gets the label after the cases. */
            if (S->value("hasInit", false) || S->value("hasVar", false))
            {
                if (!LowerWithoutPrefix(*S, BP, Out, Locals, Err)) bOk = false;
                return;
            }
            const Json* Cond = Nth(*S, 0);
            const Json* Body = Nth(*S, 1);
            if (!Cond || !Body || Kind(*Body) != "CompoundStmt") { *Err = "`switch` needs a braced body"; bOk = false; return; }

            /* UE_NAME_SWITCH(N): the value is N itself, and each UE_NAME_CASE("Text") compares against FName Text. */
            const Json* CondCall = Strip(Cond);
            const Json* CondCallee = CondCall && Kind(*CondCall) == "CallExpr" ? Strip(First(*CondCall)) : nullptr;
            const bool bName = CondCallee && Kind(*CondCallee) == "DeclRefExpr"
                            && (*CondCallee)["referencedDecl"].value("name", std::string()) == "__NameSwitch__";
            if (bName) Cond = Nth(*CondCall, 1);
            if (!Cond) { *Err = "UE_NAME_SWITCH needs a name"; bOk = false; return; }
            int32 Width = bName ? 0 : SwitchWidth(TypeOf(*Strip(Cond)));
            if (auto E = bName ? Enums.end() : Enums.find(StripTypeKeywords(TypeOf(*Strip(Cond)))); E != Enums.end())
                Width = E->second.Underlying == "int32" ? 4 : E->second.Underlying == "int64" ? 8 : 1;
            const std::string TempTy = bName ? "FName" : Width == 1 ? "uint8" : Width == 8 ? "int64" : "int32";
            const char* NotEqual = bName ? "NotEqual_NameName" : Width == 1 ? "NotEqual_ByteByte"
                                 : Width == 8 ? "NotEqual_Int64Int64" : "NotEqual_IntInt";

            const std::string Temp = "__Switch" + std::to_string(ReadTmpCounter++) + "__";
            FPropertyDef PD;
            if (!TypeToProperty(TempTy, Temp, 0, "switch value", BP, &PD, Err)) { bOk = false; return; }
            PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
            Locals.push_back(PD);
            FStmtIR Store;
            Store.K = FStmtIR::Assign;
            Store.Var.K = FArgIR::Local;
            Store.Var.S = Temp;
            Store.Var.LetOp = LetOpFor(TempTy);
            Store.bAssignLocal = true;
            if (!LowerArg(*Cond, BP, Store.Value, Err)) { bOk = false; return; }
            Out.push_back(Store);

            St.K = FStmtIR::Switch;
            St.Body = std::make_shared<std::vector<FStmtIR>>();
            ++SwitchDepth;
            std::function<bool(const Json&)> Flatten = [&](const Json& Node) -> bool
            {
                const Json* N = Strip(&Node);
                const std::string NK = N ? Kind(*N) : std::string();
                if (NK != "CaseStmt" && NK != "DefaultStmt")
                {
                    Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({Node})} };
                    return LowerBody(Wrap, BP, *St.Body, Locals, Err);
                }
                FStmtIR L;
                L.K = FStmtIR::Label;
                const Json* Sub = nullptr;
                if (NK == "DefaultStmt")
                {
                    L.LabelId = -2;                         // numbered once every case is known
                    Sub = Nth(*N, 0);
                }
                else
                {
                    const Json* Value = Nth(*N, 0);
                    Sub = Nth(*N, 1);
                    if (!Value || !Value->contains("value") || N->value("hasRange", false))
                    { *Err = "a `case` needs one constant value"; return false; }
                    FArgIR Const;
                    const int64 V = std::stoll((*Value)["value"].get<std::string>());
                    if (bName)
                    {
                        Const.K = FArgIR::Name;
                        if (!FindLiteral(*Value, Const.S)) { *Err = "a case of UE_NAME_SWITCH needs UE_NAME_CASE(\"Text\")"; return false; }
                    }
                    else if (Width == 1) { Const.K = FArgIR::Byte; Const.I = int32(V); }
                    else if (Width == 8) { Const.K = FArgIR::Int64; Const.I64 = V; }
                    else { Const.K = FArgIR::Int; Const.I = int32(V); }
                    FArgIR Value0;
                    Value0.K = FArgIR::Local;
                    Value0.S = Temp;
                    FArgIR Test;
                    Test.K = FArgIR::Call;
                    Test.InnerType = "bool";
                    Test.Sub = std::make_shared<FCallIR>();
                    Test.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", NotEqual);
                    Test.Sub->Args = { Value0, Const };
                    L.LabelId = int32(St.CaseTests.size());
                    St.CaseTests.push_back(std::move(Test));
                    St.CaseValues.push_back(V);
                }
                St.Body->push_back(L);
                return !Sub || Flatten(*Sub);
            };
            ForEach(*Body, [&](const Json& C) { if (bOk && !Flatten(C)) bOk = false; });
            --SwitchDepth;
            if (!bOk) return;
            for (FStmtIR& B : *St.Body)
                if (B.K == FStmtIR::Label && B.LabelId == -2) B.LabelId = St.LabelId = int32(St.CaseTests.size());
            /* The helpers a dense table needs, imported whether or not the emitter picks the table. ponytail: an
               unused import is a name-map entry, not a load. */
            St.SwitchWidth = Width;
            St.SwitchValue.K = FArgIR::Local;
            St.SwitchValue.S = Temp;
            St.Call.Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", "Conv_ByteToInt");
            St.Call.Extra = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", "InRange_IntInt");
            St.Call.Extra2 = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", "Add_IntInt");
            St.Target.Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", "Multiply_IntInt");
        }
        else if (K == "CXXForRangeStmt")
        {
            if (!LowerRangeFor(*S, BP, Out, Locals, Err)) { bOk = false; return; }
            return;
        }
        else if (K == "AttributedStmt")
        {
            /* `[[likely]] return X;`: a hint to a compiler that is not this one. The statement is the last inner. */
            const Json* Sub = nullptr;
            ForEach(*S, [&](const Json& C) { Sub = &C; });
            const Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({ Sub ? *Sub : Json::object() })} };
            if (!Sub || !LowerBody(Wrap, BP, Out, Locals, Err)) bOk = false;
            return;
        }
        else if (K == "CompoundStmt")
        {
            /* A bare `{ ... }`: a Blueprint local has no scope to end, so its statements join this list. */
            if (!LowerBody(*S, BP, Out, Locals, Err)) bOk = false;
            return;
        }
        else if (K == "DoStmt")
        {
            /* DoStmt inner is [body, cond]: a While whose test follows the body. */
            const Json* Body = Nth(*S, 0);
            const Json* Cond = Nth(*S, 1);
            if (!Cond || !Body) { *Err = "`do` with a missing body or condition"; bOk = false; return; }

            St.K = FStmtIR::While;
            St.bPostTest = true;
            if (!LowerArg(*Cond, BP, St.Cond, Err)) { bOk = false; return; }
            St.Body = std::make_shared<std::vector<FStmtIR>>();
            Json Wrap = Kind(*Body) == "CompoundStmt" ? *Body
                      : Json{ {"kind", "CompoundStmt"}, {"inner", Json::array({*Body})} };
            ++LoopDepth;
            if (!LowerBody(Wrap, BP, *St.Body, Locals, Err)) { bOk = false; return; }
            --LoopDepth;
        }
        else if (K == "GotoStmt")
        {
            if (WriteBackDepth > 0)
            {
                *Err = "a goto inside a TMap range-for that writes its value back would skip the write: "
                       "bind `const auto& [Key, Value]`, or leave with break";
                bOk = false;
                return;
            }
            St.K = FStmtIR::Goto;
            St.LabelId = GotoLabelOf(S->value("targetLabelDeclId", std::string()));
        }
        else if (K == "LabelStmt")
        {
            /* LabelStmt inner is [the statement it marks]. */
            St.K = FStmtIR::GotoLabel;
            St.LabelId = GotoLabelOf(S->value("declId", std::string()));
            Out.push_back(St);
            if (const Json* Sub = First(*S))
            {
                Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({*Sub})} };
                if (!LowerBody(Wrap, BP, Out, Locals, Err)) bOk = false;
            }
            return;
        }
        else if (K == "WhileStmt" && S->value("hasVar", false))
        {
            /* `while (T V = Init) Body` is [var, cond, body], and V is made again each time round:
               `while (true) { T V = Init; if (!V) break; Body }`. */
            const Json* Var = Nth(*S, 0);
            const Json* Cond = Nth(*S, 1);
            const Json* Body = Nth(*S, 2);
            if (!Var || !Cond || !Body) { *Err = "`while` with a missing condition or body"; bOk = false; return; }
            Json Not = { {"kind", "UnaryOperator"}, {"opcode", "!"}, {"type", { {"qualType", "bool"} }}, {"inner", Json::array({*Cond})} };
            Json Exit = { {"kind", "IfStmt"}, {"inner", Json::array({ Not, Json{ {"kind", "BreakStmt"} } })} };
            Json True = { {"kind", "CXXBoolLiteralExpr"}, {"type", { {"qualType", "bool"} }}, {"value", true} };
            Json Loop = { {"kind", "WhileStmt"},
                          {"inner", Json::array({ True, Json{ {"kind", "CompoundStmt"}, {"inner", Json::array({ *Var, Exit, *Body })} } })} };
            Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({ Loop })} };
            if (!LowerBody(Wrap, BP, Out, Locals, Err)) bOk = false;
            return;
        }
        else if (K == "WhileStmt")
        {
            /* WhileStmt inner is [cond, body]. */
            const Json* Cond = Nth(*S, 0);
            const Json* Body = Nth(*S, 1);
            if (!Cond || !Body) { *Err = "`while` with a missing condition or body"; bOk = false; return; }

            St.K = FStmtIR::While;
            if (!LowerArg(*Cond, BP, St.Cond, Err)) { bOk = false; return; }
            St.Body = std::make_shared<std::vector<FStmtIR>>();
            ++LoopDepth;
            if (Kind(*Body) == "CompoundStmt")
            {
                if (!LowerBody(*Body, BP, *St.Body, Locals, Err)) { bOk = false; return; }
            }
            else
            {
                Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({*Body})} };
                if (!LowerBody(Wrap, BP, *St.Body, Locals, Err)) { bOk = false; return; }
            }
            --LoopDepth;
        }
        else if (K == "ForStmt")
        {
            /* Desugars to `{ init; while (cond) { body; inc; } }`. ForStmt inner is
               [init, condVar, cond, inc, body] with absent parts kept as inline JSON nulls. */
            auto AtOr = [&](size_t I) -> const Json* {
                const Json* P = Nth(*S, I);
                return (P && !P->is_null()) ? P : nullptr;
            };
            const Json* Init = AtOr(0);
            const Json* Cond = AtOr(2);
            const Json* Inc = AtOr(3);
            const Json* Body = AtOr(4);
            if (!Cond || !Body) { *Err = "TODO: `for` needs a condition and a body"; bOk = false; return; }

            if (Init)
            {
                Json WrapInit = { {"kind", "CompoundStmt"}, {"inner", Json::array({*Init})} };
                if (!LowerBody(WrapInit, BP, Out, Locals, Err)) { bOk = false; return; }
            }

            St.K = FStmtIR::While;
            if (!LowerArg(*Cond, BP, St.Cond, Err)) { bOk = false; return; }
            St.Body = std::make_shared<std::vector<FStmtIR>>();

            Json WrapBody = Kind(*Body) == "CompoundStmt" ? *Body
                          : Json{ {"kind", "CompoundStmt"}, {"inner", Json::array({*Body})} };
            ++LoopDepth;
            if (!LowerBody(WrapBody, BP, *St.Body, Locals, Err)) { bOk = false; return; }
            if (Inc)
            {
                St.Inc = std::make_shared<std::vector<FStmtIR>>();
                Json WrapInc = { {"kind", "CompoundStmt"}, {"inner", Json::array({*Inc})} };
                if (!LowerBody(WrapInc, BP, *St.Inc, Locals, Err)) { bOk = false; return; }
            }
            --LoopDepth;
        }
        else
        {
            *Err = "TODO: unimplemented statement " + K;
            bOk = false;
            return;
        }
        /* The editor's Set node on a replicated variable (FKCHandler_VariableSet::Transform): on an Actor it calls
           FlushNetDormancy on the object first, or a dormant actor never sends the change; after the write it calls the
           RepNotify function, but only for a property a Blueprint class declares (PropertyHasLocalRepNotify: native
           ones are meant for clients). Its MarkPropertyDirtyFromRepIndex is left out: push model is compiled out of
           DRG. The replicated properties of engine and game classes come from UeApi's `<Field>__Replicated`. */
        std::string Notify;
        bool bFlush = false;
        if (bOk && SetOn)
        {
            bool bActor = false;
            for (const FRecord* A = SetOn; A; A = A->Base.empty() ? nullptr : Find(A->Base))
                if (A->UeName == "Actor") bActor = true;
            for (const FRecord* A = SetOn; A; A = A->Base.empty() ? nullptr : Find(A->Base))
            {
                /* The marker of a mod interface's variable sits on the interface the class implements. */
                const FRecord* Marked = A->Replicated.count(SetField) ? A : nullptr;
                for (const std::string& I : A->Interfaces)
                    for (const FRecord* Link : InterfaceChain(Find(I)))
                        if (!Marked && Link->bIsInterface && Link->Replicated.count(SetField)) Marked = Link;
                if (!Marked) continue;
                const std::string& Spec = Marked->Replicated.find(SetField)->second;
                bFlush = bActor;
                if (!A->IsNative() || A->UePackage.compare(0, 6, "/Game/") == 0) Notify = Spec.substr(0, Spec.find(':'));
                break;
            }
        }
        if (bFlush)
        {
            FStmtIR Flush;
            Flush.K = FStmtIR::StaticCall;
            Flush.Call.Fn = BP.EngineFunction("/Script/Engine", "Actor", "FlushNetDormancy");
            Flush.Call.bInstance = true;
            Flush.Call.Target = SetObject;
            Out.push_back(std::move(Flush));
        }
        if (bOk && St.K == FStmtIR::Assign) WarnRpcRefWrite(St.Var);
        if (bOk) Out.push_back(St);
        if (!Notify.empty())
        {
            FStmtIR Call;
            Call.K = FStmtIR::StaticCall;
            Call.Call.VirtualName = Notify;
            Call.Call.Target = SetObject;
            Call.Call.bLocalVirtual = !SetObject;
            Out.push_back(std::move(Call));
        }
    });
    return bOk;
}

/* Every use of the declaration Id in N is a plain read (an lvalue-to-rvalue conversion), so nothing writes it,
   binds a reference to it or takes its address. */
bool OnlyRead(const Json& N, const std::string& Id)
{
    bool bOk = true;
    std::function<void(const Json&, const Json*)> Walk = [&](const Json& X, const Json* Parent) {
        if (!bOk || !X.is_object()) return;
        if (Kind(X) == "DeclRefExpr" && X.contains("referencedDecl") && X["referencedDecl"].value("id", std::string()) == Id)
            bOk = Parent && Kind(*Parent) == "ImplicitCastExpr" && Parent->value("castKind", std::string()) == "LValueToRValue";
        ForEach(X, [&](const Json& C) { Walk(C, &X); });
    };
    Walk(N, nullptr);
    return bOk;
}

/* How many times N names the declaration Id. */
int32 UsesOf(const Json& N, const std::string& Id)
{
    int32 Count = 0;
    std::function<void(const Json&)> Walk = [&](const Json& X) {
        if (!X.is_object()) return;
        if (Kind(X) == "DeclRefExpr" && X.contains("referencedDecl") && X["referencedDecl"].value("id", std::string()) == Id)
            ++Count;
        ForEach(X, [&](const Json& C) { Walk(C); });
    };
    Walk(N);
    return Count;
}

/*
Folding a local into its uses needs all of them in hand: OnlyRead is vacuously true for a declaration whose uses
are somewhere else, which is exactly a synthetic wrapper body (a `for` initialiser holds its counter alone, and the
loop that increments it is a separate LowerBody). So the body must name the declaration at least once.
*/
bool FCompiler::ReadOnlyLocal(const std::string& DeclId) const
{
    return CurBody && UsesOf(*CurBody, DeclId) > 0 && OnlyRead(*CurBody, DeclId);
}

/* Every reference to Id only reads it: a copy (LValueToRValue, a copy construction) or a const view. A mutating method,
   a member access, `&`, or a non-const reference argument all fail. */
bool OnlyReadValue(const Json& N, const std::string& Id)
{
    bool bOk = true;
    std::function<void(const Json&, const Json*)> Walk = [&](const Json& X, const Json* Parent) {
        if (!bOk || !X.is_object()) return;
        if (Kind(X) == "DeclRefExpr" && X.contains("referencedDecl") && X["referencedDecl"].value("id", std::string()) == Id)
        {
            const std::string PK = Parent ? Kind(*Parent) : "";
            const std::string Cast = Parent ? Parent->value("castKind", std::string()) : "";
            const std::string To = Parent && Parent->contains("type") ? (*Parent)["type"].value("qualType", std::string()) : "";
            bOk = (PK == "ImplicitCastExpr" && (Cast == "LValueToRValue" || (Cast == "NoOp" && To.compare(0, 6, "const ") == 0)))
               || PK == "CXXConstructExpr";
        }
        ForEach(X, [&](const Json& C) { Walk(C, &X); });
    };
    Walk(N, nullptr);
    return bOk;
}

namespace
{
/* How many times a statement list or expression names a variable, stores included. */
int32 Mentions(const std::vector<FStmtIR>& Stmts, const std::string& Name);
int32 Mentions(const FArgIR& A, const std::string& Name);
int32 Mentions(const FCallIR& C, const std::string& Name)
{
    int32 N = (C.InlineResult == Name) + (C.View == Name) + (C.Inline ? Mentions(*C.Inline, Name) : 0) + (C.Target ? Mentions(*C.Target, Name) : 0);
    for (const FArgIR& A : C.Args) N += Mentions(A, Name);
    return N;
}
int32 Mentions(const FArgIR& A, const std::string& Name)
{
    return (A.S == Name) + (A.Sub ? Mentions(*A.Sub, Name) : 0) + (A.Base ? Mentions(*A.Base, Name) : 0);
}
int32 Mentions(const std::vector<FStmtIR>& Stmts, const std::string& Name)
{
    int32 N = 0;
    for (const FStmtIR& St : Stmts)
    {
        N += Mentions(St.Target, Name) + Mentions(St.Call, Name) + Mentions(St.Var, Name);
        for (const FArgIR* A : { &St.Value, &St.Cond, &St.SwitchValue }) N += Mentions(*A, Name);
        for (const FArgIR& A : St.CaseTests) N += Mentions(A, Name);
        for (const auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer }) if (*L) N += Mentions(**L, Name);
    }
    return N;
}

/* The read of local Name that runs exactly once whenever A does: not under a branch's later operands, an inline
   body, an object or struct base (which may need a variable), or a call's target. */
FArgIR* FindPlainRead(FArgIR& A, const std::string& Name)
{
    if (A.K == FArgIR::Local && A.S == Name && !A.Base) return &A;
    if (A.K != FArgIR::Call || !A.Sub || A.Sub->Inline) return nullptr;
    const size_t Count = IsBranch(A.Sub->Intrinsic) ? std::min<size_t>(1, A.Sub->Args.size()) : A.Sub->Args.size();
    for (size_t I = 0; I < Count; ++I)
        if (FArgIR* F = FindPlainRead(A.Sub->Args[I], Name)) return F;
    return nullptr;
}

bool Contains(const FArgIR& A, const FArgIR* M)
{
    if (&A == M) return true;
    if (A.Base && Contains(*A.Base, M)) return true;
    if (!A.Sub) return false;
    if (A.Sub->Target && Contains(*A.Sub->Target, M)) return true;
    return std::any_of(A.Sub->Args.begin(), A.Sub->Args.end(), [&](const FArgIR& X) { return Contains(X, M); });
}

/* Everything A evaluates besides the path down to M satisfies Pred. The calls on the path run after M. */
bool OffPath(const FArgIR& A, const FArgIR* M, const std::function<bool(const FArgIR&)>& Pred)
{
    if (&A == M) return true;
    if (!Contains(A, M)) return Pred(A);
    if (A.Sub && A.Sub->Target && !OffPath(*A.Sub->Target, M, Pred)) return false;
    if (A.Sub) for (const FArgIR& X : A.Sub->Args) if (!OffPath(X, M, Pred)) return false;
    return true;
}

/* Reads no variable and calls only pure functions of such: nothing an argument's side effects could change. */
bool ReadsNothing(const FArgIR& A)
{
    switch (A.K)
    {
    case FArgIR::Int: case FArgIR::Int64: case FArgIR::Float: case FArgIR::Bool: case FArgIR::Byte: case FArgIR::Str:
    case FArgIR::Name: case FArgIR::Text: case FArgIR::Self: case FArgIR::NullObj: case FArgIR::ObjConst: case FArgIR::SoftPath:
        return true;
    case FArgIR::Call:
        return A.Sub && A.Sub->bPure && A.Sub->Intrinsic.empty() && !A.Sub->Inline && (!A.Sub->Target || ReadsNothing(*A.Sub->Target))
            && std::all_of(A.Sub->Args.begin(), A.Sub->Args.end(), ReadsNothing);
    default:
        return false;
    }
}
}   // namespace

/* `inline void Say(FString Msg) { Post(Msg); }`: the argument goes where Msg is read instead of into a local first.
   Only for a parameter read once, by value, in the body's first statement, and only when running the argument there
   instead of before the body cannot be seen: what that statement evaluates besides the path to the read is pure, and
   when the argument itself acts, reads no variable either. Later parameters must have gone the same way, or their
   arguments would now run first. Binds[i] is the local of the i-th bind statement at the front of Body. */
void FCompiler::ArgumentsInPlace(std::vector<FStmtIR>& Body, const std::vector<std::string>& Binds, const Json& Def,
                                 const std::vector<std::string>& BindIds, std::vector<FPropertyDef>& Locals)
{
    for (size_t K = Binds.size(); K-- > 0;)
    {
        if (Body.size() <= K + 1 || Body.size() - K - 1 < 1) return;
        const std::string& Name = Binds[K];
        FStmtIR& First = Body[K + 1];
        if (!OnlyReadValue(Def, BindIds[K])) return;
        const std::vector<FStmtIR> Rest(Body.begin() + K + 1, Body.end());
        if (Mentions(Rest, Name) != 1) return;

        FArgIR* Read = nullptr;
        FArgIR* Scope = nullptr;
        if (First.K == FStmtIR::StaticCall && !First.Target.Target && First.Target.Args.empty())
        {
            for (FArgIR& A : First.Call.Args) if (!Read && (Read = FindPlainRead(A, Name))) Scope = &A;
        }
        else if ((First.K == FStmtIR::Assign || First.K == FStmtIR::Decl || First.K == FStmtIR::Return) && !First.Var.Base)
            Read = FindPlainRead(*(Scope = &First.Value), Name);
        else if (First.K == FStmtIR::If)
            Read = FindPlainRead(*(Scope = &First.Cond), Name);
        if (!Read) return;

        const FArgIR& Arg = Body[K].Value;
        const bool bActs = CallsImpure(Arg);
        const std::function<bool(const FArgIR&)> Pred = [&](const FArgIR& X) { return bActs ? ReadsNothing(X) : !CallsImpure(X); };
        bool bOk = OffPath(*Scope, Read, Pred);
        if (First.K == FStmtIR::StaticCall)
        {
            if (First.Call.Target) bOk = bOk && Pred(*First.Call.Target);
            for (const FArgIR& A : First.Call.Args) if (&A != Scope) bOk = bOk && Pred(A);
        }
        if (!bOk) return;

        *Read = Arg;
        Body.erase(Body.begin() + K);
        Locals.erase(std::remove_if(Locals.begin(), Locals.end(), [&](const FPropertyDef& L) { return L.Name == Name; }), Locals.end());
    }
}

bool FCompiler::IsInlineMethod(const FRecord& R, const std::string& Method) const
{
    auto Decl = R.Methods.find(Method);
    auto Def = R.MethodDefs.find(Method);
    return (Decl != R.Methods.end() && Decl->second->value("inline", false))
        || (Def != R.MethodDefs.end() && Def->second->value("inline", false));
}

/* Whether a body holds a `goto`, which can run a declaration again the way a loop does. */
bool HasGoto(const Json& N)
{
    if (Kind(N) == "GotoStmt") return true;
    bool bFound = false;
    ForEach(N, [&](const Json& C) { bFound = bFound || HasGoto(C); });
    return bFound;
}

/* The call becomes one Block statement in Out.Inline:
       <each by-value parameter> = <its argument>;
       <the body, locals renamed __Inl<N>_<name>, `return X` as `__Inl<N>_ReturnValue = X` + a jump to the end>
   A reference parameter bound to a variable is another name for it; bound to anything else it is a copy.
   Only calls on `this` (or a static) expand, since the body's `this` stays the caller's self. */
bool FCompiler::ExpandInline(const Json& CallNode, const Json& Def, const std::string& Method, bool bMethod, FBlueprintClass& BP,
                             FCallIR& Out, std::string* Err)
{
    if (!CurLocals) { *Err = "internal: an inline call outside a function body"; return false; }
    if (std::find(InlineStack.begin(), InlineStack.end(), Method) != InlineStack.end())
    { *Err = "inline function " + Method + " calls itself"; return false; }
    if (bMethod && Kind(CallNode) == "CXXMemberCallExpr")
    {
        const Json* Callee = Strip(First(CallNode));
        const Json* Obj = Callee ? Strip(First(*Callee)) : nullptr;
        if (!Obj || Kind(*Obj) != "CXXThisExpr")
        { *Err = "TODO: inline function " + Method + " called on another object (only this)"; return false; }
    }
    const Json* Body = nullptr;
    ForEach(Def, [&](const Json& C) { if (Kind(C) == "CompoundStmt") Body = &C; });
    if (!Body) { *Err = "inline function " + Method + " has no body"; return false; }

    const std::string Prefix = "__Inl" + std::to_string(ReadTmpCounter++) + "_";
    std::vector<FPropertyDef>& Locals = *CurLocals;
    auto AddLocal = [&](const std::string& Name, const std::string& Type) {
        FPropertyDef PD;
        if (!TypeToProperty(Type, Name, 0, "inline " + Method, BP, &PD, Err)) return false;
        PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
        Locals.push_back(PD);
        return true;
    };

    auto Block = std::make_shared<std::vector<FStmtIR>>(1);
    FStmtIR& B = (*Block)[0];
    B.K = FStmtIR::Block;
    B.Body = std::make_shared<std::vector<FStmtIR>>();

    /* Parameters, in order against the call's arguments (inner[0] is the callee). */
    std::vector<const Json*> Parms, Args;
    ForEach(Def, [&](const Json& C) { if (Kind(C) == "ParmVarDecl") Parms.push_back(&C); });
    std::vector<std::string> Binds, BindIds;
    bool bFirst = true;
    ForEach(CallNode, [&](const Json& C) { if (bFirst) { bFirst = false; return; } Args.push_back(&C); });
    if (Args.size() != Parms.size()) { *Err = "inline call to " + Method + " with " + std::to_string(Args.size()) + " arguments"; return false; }
    for (size_t I = 0; I < Parms.size(); ++I)
    {
        const std::string Id = Parms[I]->value("id", std::string());
        std::string Type = TypeOf(*Parms[I]);
        const bool bRef = !Type.empty() && Type.back() == '&';
        const Json* Bare = PeelLvalue(Args[I]);
        /* A previous expansion of the same function left its own binding for this parameter. */
        RefAlias.erase(Id);
        LocalRename.erase(Id);
        ParmConst.erase(Id);
        if (bRef && Bare && IsAliasable(*Bare) && !IsDerefLvalue(*Bare)) { RefAlias[Id] = *Bare; continue; }
        while (!Type.empty() && (Type.back() == '&' || Type.back() == ' ')) Type.pop_back();
        Type = StripTypeKeywords(Type);
        const std::string Local = Prefix + Name(*Parms[I]);
        FStmtIR Bind;
        if (!LowerArg(*Args[I], BP, Bind.Value, Err)) return false;
        /* A constant the body only reads is used in place: no local, no copy. */
        if (IsFoldableConst(Bind.Value) && OnlyRead(*Body, Id)) { ParmConst[Id] = Bind.Value; continue; }
        if (!AddLocal(Local, Type)) return false;
        Bind.K = FStmtIR::Assign;
        Bind.Var.K = FArgIR::Local;
        Bind.Var.S = Local;
        Bind.Var.LetOp = LetOpFor(Type);
        Bind.bAssignLocal = true;
        B.Body->push_back(std::move(Bind));
        Binds.push_back(Local);
        BindIds.push_back(Id);
        LocalRename[Id] = Local;
    }

    /* Every local the body declares gets the expansion's prefix. */
    std::function<void(const Json&)> Rename = [&](const Json& N) {
        if (!N.is_object()) return;
        if ((Kind(N) == "VarDecl" || Kind(N) == "DecompositionDecl") && N.contains("name"))
            LocalRename[N.value("id", std::string())] = Prefix + Name(N);
        ForEach(N, Rename);
    };
    Rename(*Body);

    std::string RetType = Def["type"].value("qualType", std::string());
    RetType = RetType.substr(0, RetType.find('('));
    while (!RetType.empty() && (RetType.back() == ' ' || RetType.back() == '&')) RetType.pop_back();
    RetType = StripTypeKeywords(RetType);
    const bool bValue = !RetType.empty() && RetType != "void";
    if (bValue)
    {
        Out.InlineResult = Prefix + "ReturnValue";
        Out.InlineType = RetType;
        if (!AddLocal(Out.InlineResult, RetType)) return false;
    }

    InlineStack.push_back(Method);
    InlineResults.emplace_back(Out.InlineResult, RetType);
    const int32 SavedLoops = LoopDepth, SavedSwitches = SwitchDepth, SavedWriteBacks = WriteBackDepth;
    LoopDepth = SwitchDepth = WriteBackDepth = 0;
    /* Each expansion lowers the body again, so its labels get ids of their own. */
    std::map<std::string, int32> SavedLabels;
    SavedLabels.swap(GotoLabels);
    const bool bSavedHasGoto = bBodyHasGoto;
    bBodyHasGoto = HasGoto(*Body);
    const bool bOk = LowerBody(*Body, BP, *B.Body, Locals, Err);
    LoopDepth = SavedLoops;
    SwitchDepth = SavedSwitches;
    WriteBackDepth = SavedWriteBacks;
    GotoLabels.swap(SavedLabels);
    bBodyHasGoto = bSavedHasGoto;
    InlineResults.pop_back();
    InlineStack.pop_back();
    if (!bOk) { *Err = "inline " + Method + ": " + *Err; return false; }

    /* A return that is the body's last statement already falls through to the end. */
    if (!B.Body->empty() && B.Body->back().K == FStmtIR::InlineReturn) B.Body->pop_back();
    ArgumentsInPlace(*B.Body, Binds, Def, BindIds, Locals);
    Out.Intrinsic = "__Inline__";
    Out.Inline = Block;
    return true;
}

/* A DeclRefExpr to a compiler-made local, for lowering synthetic statements through the ordinary paths. */
Json RefToLocal(const std::string& Name, const std::string& Type)
{
    return { {"kind", "DeclRefExpr"}, {"type", {{"qualType", Type}}},
             {"referencedDecl", {{"kind", "VarDecl"}, {"name", Name}, {"id", "synthetic:" + Name}}} };
}

/* `if (Init; Cond)`, `if (T V = Init)` and the same two on a `switch`: clang puts the init-statement and then the
   condition variable's DeclStmt before the condition, which already reads V. Both run once, so they are lowered
   as statements of their own and the rest as the plain statement; a Blueprint local has no scope to end. */
bool FCompiler::LowerWithoutPrefix(const Json& Stmt, FBlueprintClass& BP, std::vector<FStmtIR>& Out,
                                   std::vector<FPropertyDef>& Locals, std::string* Err)
{
    const size_t Skip = (Stmt.value("hasInit", false) ? 1 : 0) + (Stmt.value("hasVar", false) ? 1 : 0);
    const Json& In = Stmt["inner"];
    Json Seq = Json::array(), Rest = Json::array();
    for (size_t I = 0; I < In.size(); ++I) (I < Skip ? Seq : Rest).push_back(In[I]);
    Json Plain = Stmt;
    Plain["hasInit"] = false;
    Plain["hasVar"] = false;
    Plain["inner"] = Rest;
    Seq.push_back(Plain);
    Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Seq} };
    return LowerBody(Wrap, BP, Out, Locals, Err);
}

/* `for (Elem : Range)` over a TArray / TSet / TMap. CXXForRangeStmt inner is
   [init, __range1, __begin1, __end1, cond, inc, loop variable, body].
     TArray: in place, by index. `T& E` is another name for `Range[Idx]`, so writes land in the array;
             a by-value `T E` is a copy made each iteration. The length is read once, as C++ reads end() once.
     TSet:   over a Set_ToArray copy; elements are copies.
     TMap:   over a Map_Keys copy, each value fetched with Map_Find. `auto [K, V]` binds K to a copy of the key and
             V to a local; unless the binding is const, V goes back with Map_Add after each iteration, `break`
             included, so the body can change values in place.
   ponytail: TSet / TMap iterate a copy, not the sparse array in place; the in-place walk needs the container's
   address, which a Blueprint variable does not have (ROADMAP.md, Phase 3). */
bool FCompiler::LowerRangeFor(const Json& ForNode, FBlueprintClass& BP, std::vector<FStmtIR>& Out,
                              std::vector<FPropertyDef>& Locals, std::string* Err)
{
    const Json* RangeStmt = Nth(ForNode, 1);
    const Json* RangeDecl = RangeStmt ? First(*RangeStmt) : nullptr;
    const Json* RangeExpr = RangeDecl ? First(*RangeDecl) : nullptr;
    const Json* LoopStmt = Nth(ForNode, 6);
    const Json* LoopDecl = LoopStmt ? First(*LoopStmt) : nullptr;
    const Json* Body = Nth(ForNode, 7);
    if (!RangeExpr || !LoopDecl || !Body) { *Err = "a range-for with a missing part"; return false; }
    if (const Json* Init = Nth(ForNode, 0); Init && Init->is_object() && Init->contains("kind"))
    {
        /* `for (Init; T E : Range)`: the init-statement runs once, before the loop. */
        Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({ *Init })} };
        if (!LowerBody(Wrap, BP, Out, Locals, Err)) return false;
    }

    std::string RangeTy = TypeOf(*RangeExpr);
    while (!RangeTy.empty() && (RangeTy.back() == '&' || RangeTy.back() == ' ')) RangeTy.pop_back();
    RangeTy = StripTypeKeywords(RangeTy);
    if (!IsContainerType(RangeTy)) { *Err = "TODO: range-for over " + RangeTy + " (TArray, TSet and TMap only)"; return false; }
    const char Which = RangeTy[1];                       // 'A'rray, 'S'et, 'M'ap
    const size_t Open = RangeTy.find('<');
    const std::vector<std::string> Args = SplitTemplateArgs(RangeTy.substr(Open + 1, RangeTy.rfind('>') - Open - 1));

    const std::string N = std::to_string(ReadTmpCounter++);
    auto AddLocal = [&](const std::string& Name, const std::string& Type) {
        FPropertyDef PD;
        if (!TypeToProperty(Type, Name, 0, "range-for", BP, &PD, Err)) return false;
        PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
        Locals.push_back(PD);
        return true;
    };
    auto LocalArg = [](const std::string& Name) { FArgIR A; A.K = FArgIR::Local; A.S = Name; return A; };
    auto CallStmt = [&](const char* Lib, const char* Fn, std::vector<FArgIR> CallArgs) {
        FStmtIR St;
        St.K = FStmtIR::StaticCall;
        St.Call.Fn = BP.EngineFunction("/Script/Engine", Lib, Fn);
        St.Call.Args = std::move(CallArgs);
        return St;
    };
    auto Math = [&](const char* Fn, FArgIR A, FArgIR B) {
        FArgIR C;
        C.K = FArgIR::Call;
        C.Sub = std::make_shared<FCallIR>();
        C.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", Fn);
        C.Sub->Args = { std::move(A), std::move(B) };
        return C;
    };
    auto AssignStmt = [&](const std::string& Name, const std::string& Type, FArgIR Value) {
        FStmtIR St;
        St.K = FStmtIR::Assign;
        St.Var = LocalArg(Name);
        St.Var.LetOp = LetOpFor(Type);
        St.bAssignLocal = true;
        St.Value = std::move(Value);
        return St;
    };

    FArgIR Range;
    if (!LowerArg(*RangeExpr, BP, Range, Err)) return false;
    if (Range.K != FArgIR::Field && Range.K != FArgIR::Local && Range.K != FArgIR::LocalOut && Range.K != FArgIR::Member)
    { *Err = "range-for needs a container variable, not a computed value"; return false; }

    /* The array walked by index: the container itself, or a copy of its elements / keys. */
    Json IterJson = *RangeExpr;
    std::string ElemTy = Args[0];
    if (Which != 'A')
    {
        const std::string Copy = "__RangeCopy" + N + "__";
        if (!AddLocal(Copy, "TArray<" + Args[0] + ">")) return false;
        Out.push_back(Which == 'S' ? CallStmt("BlueprintSetLibrary", "Set_ToArray", { Range, LocalArg(Copy) })
                                   : CallStmt("BlueprintMapLibrary", "Map_Keys", { Range, LocalArg(Copy) }));
        IterJson = RefToLocal(Copy, "TArray<" + Args[0] + ">");
    }
    FArgIR Iter;
    if (!LowerArg(IterJson, BP, Iter, Err)) return false;

    const std::string Idx = "__RangeIdx" + N + "__", Len = "__RangeLen" + N + "__";
    if (!AddLocal(Idx, "int32") || !AddLocal(Len, "int32")) return false;
    FArgIR Zero;
    Zero.K = FArgIR::Int;
    Out.push_back(AssignStmt(Idx, "int32", Zero));
    FArgIR Length;
    Length.K = FArgIR::Call;
    Length.Sub = std::make_shared<FCallIR>();
    Length.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetArrayLibrary", "Array_Length");
    Length.Sub->Args = { Iter };
    Out.push_back(AssignStmt(Len, "int32", std::move(Length)));

    FStmtIR Loop;
    Loop.K = FStmtIR::While;
    Loop.Cond = Math("Less_IntInt", LocalArg(Idx), LocalArg(Len));
    Loop.Body = std::make_shared<std::vector<FStmtIR>>();
    Loop.Inc = std::make_shared<std::vector<FStmtIR>>();
    FArgIR One;
    One.K = FArgIR::Int;
    One.I = 1;

    const Json Elem = { {"kind", "CXXOperatorCallExpr"}, {"type", {{"qualType", ElemTy}}},
                        {"inner", Json::array({ Json{ {"kind", "DeclRefExpr"}, {"referencedDecl", {{"kind", "CXXMethodDecl"}, {"name", "operator[]"}}} },
                                                IterJson, RefToLocal(Idx, "int32") })} };
    ++LoopDepth;
    if (Which != 'M')
    {
        if (Kind(*LoopDecl) != "VarDecl") { *Err = "TODO: a structured binding over a " + RangeTy; --LoopDepth; return false; }
        std::string VarTy = TypeOf(*LoopDecl);
        const bool bRef = !VarTy.empty() && VarTy.back() == '&';
        if (bRef && Which == 'A') RefAlias[LoopDecl->value("id", std::string())] = Elem;
        else
        {
            while (!VarTy.empty() && (VarTy.back() == '&' || VarTy.back() == ' ')) VarTy.pop_back();
            Json Decl = *LoopDecl;
            Decl["type"] = { {"qualType", StripTypeKeywords(VarTy)} };
            Decl["inner"] = Json::array({ Elem });
            Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({ Json{ {"kind", "DeclStmt"}, {"inner", Json::array({ Decl })} } })} };
            if (!LowerBody(Wrap, BP, *Loop.Body, Locals, Err)) { --LoopDepth; return false; }
        }
    }
    else
    {
        if (Kind(*LoopDecl) != "DecompositionDecl") { *Err = "a TMap range-for binds `auto [Key, Value]`"; --LoopDepth; return false; }
        std::vector<const Json*> Bindings;
        ForEach(*LoopDecl, [&](const Json& C) { if (Kind(C) == "BindingDecl") Bindings.push_back(&C); });
        if (Bindings.size() != 2) { *Err = "a TMap range-for binds exactly `auto [Key, Value]`"; --LoopDepth; return false; }
        const std::string Key = "__RangeKey" + N + "__", Val = "__RangeVal" + N + "__";
        if (!AddLocal(Key, Args[0]) || !AddLocal(Val, Args[1])) { --LoopDepth; return false; }
        FArgIR KeyAt;
        if (!LowerArg(Elem, BP, KeyAt, Err)) { --LoopDepth; return false; }
        Loop.Body->push_back(AssignStmt(Key, Args[0], std::move(KeyAt)));
        Loop.Body->push_back(CallStmt("BlueprintMapLibrary", "Map_Find", { Range, LocalArg(Key), LocalArg(Val) }));
        RefAlias[Bindings[0]->value("id", std::string())] = RefToLocal(Key, Args[0]);
        RefAlias[Bindings[1]->value("id", std::string())] = RefToLocal(Val, Args[1]);
        if (StripTypeKeywords(TypeOf(*LoopDecl)) == TypeOf(*LoopDecl))       // no leading const
        {
            FStmtIR Back = CallStmt("BlueprintMapLibrary", "Map_Add", { Range, LocalArg(Key), LocalArg(Val) });
            Loop.Inc->push_back(Back);
            Loop.Trailer = std::make_shared<std::vector<FStmtIR>>(1, Back);
        }
    }
    Json BodyWrap = Kind(*Body) == "CompoundStmt" ? *Body : Json{ {"kind", "CompoundStmt"}, {"inner", Json::array({ *Body })} };
    if (Loop.Trailer) ++WriteBackDepth;
    const bool bBodyOk = LowerBody(BodyWrap, BP, *Loop.Body, Locals, Err);
    if (Loop.Trailer) --WriteBackDepth;
    if (!bBodyOk) { --LoopDepth; return false; }
    --LoopDepth;
    Loop.Inc->push_back(AssignStmt(Idx, "int32", Math("Add_IntInt", LocalArg(Idx), One)));
    Out.push_back(std::move(Loop));
    return true;
}

bool FCompiler::HoistReadCall(FArgIR& A, const FReadViewSpec& V, FBlueprintClass& BP,
                              std::vector<FPropertyDef>& Locals,
                              std::vector<FStmtIR>& OutPre, std::string* Err)
{
    if (!A.Sub || A.Sub->Args.size() != 1)
    {
        *Err = std::string(V.Intrinsic) + " takes exactly one argument (the address)";
        return false;
    }
    if (!HasDerefStruct(Err)) return false;
    FArgIR AddrExpr = std::move(A.Sub->Args[0]);

    /* First hoist in this function adds the FDeref __DerefScratch__ local; the prologue seeds
       Num=1 once, later hoists only overwrite Data. */
    const FIndex DerefStruct = BP.ScriptStruct(ModPackage + "/FDeref", "FDeref");
    if (!ReadScratchAdded)
    {
        ReadScratchAdded = true;
        FPropertyDef Scratch = StructParam("__DerefScratch__", DerefStruct, "FDeref", 16, 0);
        Scratch.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
        Locals.push_back(Scratch);
    }

    const int32 N = ReadTmpCounter++;
    const std::string TmpName = "__DerefTmp" + std::to_string(N) + "__";

    FPropertyDef TmpPD;
    std::string PErr;
    if (!TypeToProperty(V.ResultType, TmpName, 0, TmpName, BP, &TmpPD, &PErr))
    {
        *Err = "hoisting " + std::string(V.Intrinsic) + ": " + PErr;
        return false;
    }
    TmpPD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
    Locals.push_back(TmpPD);

    /* Statement 1: __DerefScratch__.Data = <Addr>. */
    FStmtIR WriteData;
    WriteData.K = FStmtIR::Assign;
    WriteData.Var.K = FArgIR::Member;
    WriteData.Var.S = "Data";
    WriteData.Var.Owner = DerefStruct;
    WriteData.Var.LetOp = EX_Let;
    WriteData.Var.Base = std::make_shared<FArgIR>();
    WriteData.Var.Base->K = FArgIR::Local;
    WriteData.Var.Base->S = "__DerefScratch__";
    WriteData.Value = std::move(AddrExpr);
    OutPre.push_back(WriteData);

    /* Statement 2: __DerefTmpN__ = __DerefRead*__() -- emitted as ArrayGetByRef against the view. */
    FStmtIR ReadTmp;
    ReadTmp.K = FStmtIR::Assign;
    ReadTmp.Var.K = FArgIR::Local;
    ReadTmp.Var.S = TmpName;
    ReadTmp.Var.LetOp = LetOpFor(V.ResultType);
    ReadTmp.bAssignLocal = true;
    ReadTmp.Value.K = FArgIR::Call;
    ReadTmp.Value.Sub = std::make_shared<FCallIR>();
    ReadTmp.Value.Sub->Intrinsic = V.DerefIntrinsic;
    ReadTmp.Value.Sub->Extra = BP.ScriptStruct(V.ViewStructPkg, V.ViewStructName);
    /* ponytail: a mod-owned view is { TArray<T> Data; }, 16 bytes. */
    if (std::strncmp(V.ViewStructPkg, "/Game/", 6) == 0) KeepStructLoaded(ReadTmp.Value.Sub->Extra, V.ViewStructName, 16);
    OutPre.push_back(ReadTmp);

    /* Replace the original read expression with LocalVariable(__DerefTmpN__). */
    A = FArgIR{};
    A.K = FArgIR::Local;
    A.S = TmpName;
    A.LetOp = LetOpFor(V.ResultType);
    return true;
}

bool FCompiler::HoistReadsInArg(FArgIR& A, FBlueprintClass& BP,
                                std::vector<FPropertyDef>& Locals,
                                std::vector<FStmtIR>& OutPre, std::string* Err)
{
    if ((A.K == FArgIR::Member || A.K == FArgIR::Field || A.K == FArgIR::InterfaceCtx) && A.Base)
        return HoistReadsInArg(*A.Base, BP, Locals, OutPre, Err);
    if (A.K == FArgIR::Index && A.Base && A.Sub && A.Sub->Args.size() == 1)
        return HoistReadsInArg(*A.Base, BP, Locals, OutPre, Err) && HoistReadsInArg(A.Sub->Args[0], BP, Locals, OutPre, Err);
    if (A.K == FArgIR::DynCast && A.Sub)
        return HoistReadsInArg(A.Sub->Args[0], BP, Locals, OutPre, Err);
    if (A.K != FArgIR::Call || !A.Sub) return true;
    if (IsBranch(A.Sub->Intrinsic)) return HoistBranch(A, BP, Locals, OutPre, Err);
    if (A.Sub->Intrinsic == "__Inline__")
    {
        /* The body runs as statements before the one that uses its value, which is then just the result local. */
        if (A.Sub->InlineResult.empty()) { *Err = "a void inline function used as a value"; return false; }
        for (FStmtIR& B : *A.Sub->Inline)
        {
            if (B.Body && !HoistReadsInList(*B.Body, BP, Locals, Err)) return false;
            OutPre.push_back(std::move(B));
        }
        const std::string Result = A.Sub->InlineResult, Type = A.Sub->InlineType;
        A = FArgIR();
        A.K = FArgIR::Local;
        A.S = Result;
        A.LetOp = LetOpFor(Type);
        A.InnerType = Type;
        return true;
    }
    if (A.Sub->Target && !HoistReadsInArg(*A.Sub->Target, BP, Locals, OutPre, Err)) return false;

    /* Post-order: inner reads hoist before the outer. That way the outer's Addr can reference
       an already-materialised inner temp. */
    for (FArgIR& CA : A.Sub->Args)
        if (!HoistReadsInArg(CA, BP, Locals, OutPre, Err)) return false;

    if (const FReadViewSpec* V = FindReadView(A.Sub->Intrinsic))
        return HoistReadCall(A, *V, BP, Locals, OutPre, Err);
    if (A.Sub->Intrinsic == "__RefAt__")
        return HoistRefAt(A, BP, Locals, OutPre, Err);
    if (IsReinterpret(A.Sub->Intrinsic) && A.Sub->Args.size() == 1)
        return HoistOperand(A.Sub->Args[0], BP, Locals, OutPre, Err);
    return true;
}

FArgIR NotOf(FArgIR V, FBlueprintClass& BP)
{
    WrapInCall(V, BP.EngineFunction("/Script/Engine", "KismetMathLibrary", "Not_PreBool"));
    V.InnerType = "bool";
    return V;
}

/* `L && R`, `L || R`, `C ? A : B` as statements before the one that uses them, into a temp:
       T = L;  if (T)  { <R's own hoists>; T = R; }        (|| tests !T)
       if (C) { <A's hoists>; T = A; } else { <B's hoists>; T = B; }
   The arms' hoisted reads land inside their branch, so they run only when the branch does. */
bool FCompiler::HoistBranch(FArgIR& A, FBlueprintClass& BP, std::vector<FPropertyDef>& Locals,
                            std::vector<FStmtIR>& OutPre, std::string* Err)
{
    const std::string Which = A.Sub->Intrinsic;
    std::string Type = Which == "__Select__" ? A.InnerType : "bool";
    while (!Type.empty() && (Type.back() == '&' || Type.back() == ' ')) Type.pop_back();
    Type = StripTypeKeywords(Type);

    const std::string Tmp = "__Branch" + std::to_string(ReadTmpCounter++) + "__";
    FPropertyDef PD;
    std::string PErr;
    if (!TypeToProperty(Type, Tmp, 0, Tmp, BP, &PD, &PErr)) { *Err = "a branch's value: " + PErr; return false; }
    PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
    Locals.push_back(PD);

    FArgIR TmpRef;
    TmpRef.K = FArgIR::Local;
    TmpRef.S = Tmp;
    TmpRef.LetOp = LetOpFor(Type);
    TmpRef.InnerType = Type;
    auto Store = [&](FArgIR V) {
        FStmtIR St;
        St.K = FStmtIR::Assign;
        St.Var = TmpRef;
        St.bAssignLocal = true;
        St.Value = std::move(V);
        return St;
    };
    auto Arm = [&](FArgIR V, std::shared_ptr<std::vector<FStmtIR>>& Into) {
        Into = std::make_shared<std::vector<FStmtIR>>();
        if (!HoistReadsInArg(V, BP, Locals, *Into, Err)) return false;
        Into->push_back(Store(std::move(V)));
        return true;
    };

    FArgIR Cond = std::move(A.Sub->Args[0]);
    if (!HoistReadsInArg(Cond, BP, Locals, OutPre, Err)) return false;
    FStmtIR If;
    If.K = FStmtIR::If;
    if (Which == "__Select__")
    {
        If.Cond = std::move(Cond);
        if (!Arm(std::move(A.Sub->Args[1]), If.Then) || !Arm(std::move(A.Sub->Args[2]), If.Else)) return false;
    }
    else
    {
        OutPre.push_back(Store(std::move(Cond)));
        If.Cond = Which == "__AndAlso__" ? TmpRef : NotOf(TmpRef, BP);
        if (!Arm(std::move(A.Sub->Args[1]), If.Then)) return false;
    }
    OutPre.push_back(std::move(If));
    A = TmpRef;
    return true;
}

/* __RefAt__(Addr) stays an inline ArrayGetByRef, so a CustomThunk stepping it with a null
   result sees MostRecentPropertyAddress == Addr. A temp would hand it the temp's address
   instead. Each use gets its own FDeref scratch, so later reads in the statement can't
   overwrite its Data before the call runs. */
bool FCompiler::HoistRefAt(FArgIR& A, FBlueprintClass& BP, std::vector<FPropertyDef>& Locals,
                           std::vector<FStmtIR>& OutPre, std::string* Err)
{
    if (A.Sub->Args.size() != 1)
    {
        *Err = "__RefAt__ takes exactly one argument (the address)";
        return false;
    }
    if (!HasDerefStruct(Err)) return false;
    const FIndex DerefStruct = BP.ScriptStruct(ModPackage + "/FDeref", "FDeref");
    const std::string Scratch = "__RefScratch" + std::to_string(ReadTmpCounter++) + "__";
    FPropertyDef PD = StructParam(Scratch, DerefStruct, "FDeref", 16, 0);
    PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
    Locals.push_back(PD);

    auto Assign = [&](const char* Field, FArgIR Value)
    {
        FStmtIR St;
        St.K = FStmtIR::Assign;
        St.Var.K = FArgIR::Member;
        St.Var.S = Field;
        St.Var.Owner = DerefStruct;
        St.Var.LetOp = EX_Let;
        St.Var.Base = std::make_shared<FArgIR>();
        St.Var.Base->K = FArgIR::Local;
        St.Var.Base->S = Scratch;
        St.Value = std::move(Value);
        OutPre.push_back(std::move(St));
    };
    FArgIR One;
    One.K = FArgIR::Int;
    One.I = 1;
    Assign("Num", One);
    Assign("Data", std::move(A.Sub->Args[0]));

    /* A callee that steps the argument into a buffer copies the view's element type, so the view
       matches what the address holds; the user's own __RefAt__(Addr) is typed int64. */
    const FReadViewSpec* V = ViewFor(A.InnerType);
    if (!V) V = FindReadView("__Read64__");
    const std::string InnerType = A.InnerType;
    A = FArgIR{};
    A.K = FArgIR::Call;
    A.S = Scratch;
    A.InnerType = InnerType;
    A.Sub = std::make_shared<FCallIR>();
    A.Sub->Intrinsic = "__RefAtInline__";
    A.Sub->Extra = BP.ScriptStruct(V->ViewStructPkg, V->ViewStructName);
    A.Sub->View = ViewFieldOf(V->DerefIntrinsic);
    if (std::strncmp(V->ViewStructPkg, "/Game/", 6) == 0) KeepStructLoaded(A.Sub->Extra, V->ViewStructName, 16);
    return true;
}

bool ContainsRead(const FArgIR& A)
{
    if ((A.K == FArgIR::Member || A.K == FArgIR::Field || A.K == FArgIR::InterfaceCtx) && A.Base) return ContainsRead(*A.Base);
    if (A.K == FArgIR::Index && A.Base && A.Sub && A.Sub->Args.size() == 1)
        return ContainsRead(*A.Base) || ContainsRead(A.Sub->Args[0]);
    if (A.K == FArgIR::DynCast && A.Sub) return ContainsRead(A.Sub->Args[0]);
    if (A.K != FArgIR::Call || !A.Sub) return false;
    if (A.Sub->Target && ContainsRead(*A.Sub->Target)) return true;
    if (FindReadView(A.Sub->Intrinsic) || A.Sub->Intrinsic == "__RefAt__" || IsBranch(A.Sub->Intrinsic)
        || A.Sub->Intrinsic == "__Inline__") return true;
    if (IsReinterpret(A.Sub->Intrinsic) && A.Sub->Args.size() == 1 && !IsStored(A.Sub->Args[0])) return true;
    for (const FArgIR& CA : A.Sub->Args) if (ContainsRead(CA)) return true;
    return false;
}

bool FCompiler::HoistReadsInStmt(FStmtIR& St, FBlueprintClass& BP,
                                 std::vector<FPropertyDef>& Locals,
                                 std::vector<FStmtIR>& OutPre, std::string* Err)
{
    /* A while condition runs every iteration, but hoisted statements land before the loop. So a condition that
       needs them becomes `while (true) { if (!Cond) break; ... }`, and they land inside. */
    if (St.K == FStmtIR::While && ContainsRead(St.Cond))
    {
        FStmtIR Exit;
        Exit.K = FStmtIR::If;
        Exit.Cond = NotOf(std::move(St.Cond), BP);
        Exit.Then = std::make_shared<std::vector<FStmtIR>>();
        Exit.Then->emplace_back().K = FStmtIR::Break;
        if (!St.Body) St.Body = std::make_shared<std::vector<FStmtIR>>();
        St.Body->insert(St.Body->begin(), std::move(Exit));
        St.Cond = FArgIR();
        St.Cond.K = FArgIR::Bool;
        St.Cond.B = true;
    }

    if (St.Then) if (!HoistReadsInList(*St.Then, BP, Locals, Err)) return false;
    if (St.Else) if (!HoistReadsInList(*St.Else, BP, Locals, Err)) return false;
    if (St.Body) if (!HoistReadsInList(*St.Body, BP, Locals, Err)) return false;
    if (St.Inc) if (!HoistReadsInList(*St.Inc, BP, Locals, Err)) return false;
    if (St.Trailer) if (!HoistReadsInList(*St.Trailer, BP, Locals, Err)) return false;

    if (!HoistReadsInArg(St.Var,   BP, Locals, OutPre, Err)) return false;
    if (!HoistReadsInArg(St.Value, BP, Locals, OutPre, Err)) return false;
    if (!HoistReadsInArg(St.Cond,  BP, Locals, OutPre, Err)) return false;
    if (St.Call.Target && !HoistReadsInArg(*St.Call.Target, BP, Locals, OutPre, Err)) return false;
    for (FArgIR& CA : St.Call.Args)
        if (!HoistReadsInArg(CA, BP, Locals, OutPre, Err)) return false;
    for (FArgIR& CA : St.Target.Args)
        if (!HoistReadsInArg(CA, BP, Locals, OutPre, Err)) return false;
    return true;
}

bool FCompiler::HoistReadsInList(std::vector<FStmtIR>& Stmts, FBlueprintClass& BP,
                                 std::vector<FPropertyDef>& Locals, std::string* Err)
{
    std::vector<FStmtIR> Out;
    Out.reserve(Stmts.size());
    for (FStmtIR& St : Stmts)
    {
        std::vector<FStmtIR> Pre;
        if (!HoistReadsInStmt(St, BP, Locals, Pre, Err)) return false;
        for (FStmtIR& P : Pre) Out.push_back(std::move(P));
        Out.push_back(std::move(St));
    }
    Stmts = std::move(Out);
    return true;
}

std::string StripTypeKeywords(std::string T)
{
    for (const char* Prefix : { "const ", "struct ", "class " })
        if (T.compare(0, strlen(Prefix), Prefix) == 0) T = T.substr(strlen(Prefix));
    while (!T.empty() && (T.back() == ' ' || T.back() == '\t')) T.pop_back();
    return T;
}

/* The number a constant expression comes to: literals, enum constants, ConstVars, casts, and arithmetic over them,
   each step rounded to the type clang gave it so `2 * 50.f` and `1 << 31` come out as C++ has them. False for
   anything else (a call, sizeof, ?:) and for a division by zero.
   ponytail: no comparisons, `&&`, `?:` or sizeof; add them when a mod writes one. */
bool FCompiler::FoldConst(const Json& E, FConstVal& Out) const
{
    const std::string K = Kind(E);
    const auto Fit = [&](FConstVal& V) {
        std::string T = StripTypeKeywords(TypeOf(E));
        if (auto En = Enums.find(T); En != Enums.end()) T = En->second.Underlying;
        const bool bWantFloat = T == "float" || T == "double";
        const bool bInt = T == "bool" || T == "int" || T == "int32" || T == "unsigned int" || T == "uint32" || T == "uint8"
                       || T == "unsigned char" || T == "int8" || T == "signed char" || T == "short" || T == "unsigned short"
                       || T == "int16" || T == "uint16" || IsInt64Type(T);
        if (!bWantFloat && !bInt) return false;
        if (bWantFloat) { V.F = T == "float" ? double(float(V.Num())) : V.Num(); V.bFloat = true; return true; }
        V.I = V.bFloat ? int64(V.F) : V.I;
        V.bFloat = false;
        if (T == "bool") V.I = V.Num() != 0;
        else if (T == "int" || T == "int32") V.I = int32(V.I);
        else if (T == "unsigned int" || T == "uint32") V.I = uint32(V.I);
        else if (T == "uint8" || T == "unsigned char") V.I = uint8(V.I);
        else if (T == "int8" || T == "signed char") V.I = int8(V.I);
        else if (T == "short" || T == "int16") V.I = int16(V.I);
        else if (T == "unsigned short" || T == "uint16") V.I = uint16(V.I);
        return true;
    };
    if (K == "IntegerLiteral") { Out = {}; Out.I = int64(std::strtoull(E.value("value", std::string("0")).c_str(), nullptr, 10)); return true; }
    if (K == "FloatingLiteral") { Out = {}; Out.bFloat = true; Out.F = std::strtod(E.value("value", std::string("0")).c_str(), nullptr); return true; }
    if (K == "CXXBoolLiteralExpr") { Out = {}; Out.I = E.value("value", false); return true; }
    if (K == "ConstantExpr" && E.contains("value") && E["value"].is_string())
    {
        /* clang ran it: a consteval call (an immediate invocation is always wrapped so), whatever its body does. */
        const std::string V = E["value"].get<std::string>();
        Out = {};
        if (V == "true" || V == "false") Out.I = V == "true";
        else if (V.find_first_of(".eEn") != std::string::npos) { Out.bFloat = true; Out.F = std::strtod(V.c_str(), nullptr); }
        else Out.I = std::strtoll(V.c_str(), nullptr, 10);
        return Fit(Out);
    }
    if (K == "ParenExpr" || K == "ConstantExpr" || K == "ExprWithCleanups")
        return First(E) && FoldConst(*First(E), Out);
    if (K == "ImplicitCastExpr" || K == "CStyleCastExpr" || K == "CXXStaticCastExpr" || K == "CXXFunctionalCastExpr")
        return First(E) && FoldConst(*First(E), Out) && Fit(Out);
    if (K == "DeclRefExpr" && E.contains("referencedDecl"))
    {
        const std::string Id = E["referencedDecl"].value("id", std::string());
        if (auto V = EnumValues.find(Id); V != EnumValues.end()) { Out = {}; Out.I = V->second; return true; }
        auto C = ConstVars.find(Id);
        return C != ConstVars.end() && First(*C->second) && FoldConst(*First(*C->second), Out) && Fit(Out);
    }
    if (K == "UnaryOperator")
    {
        const std::string Op = E.value("opcode", std::string());
        if (!First(E) || !FoldConst(*First(E), Out)) return false;
        if (Op == "-") { Out.F = -Out.F; Out.I = -Out.I; }
        else if (Op == "~" && !Out.bFloat) Out.I = ~Out.I;
        else if (Op == "!") { Out.I = Out.Num() == 0; Out.bFloat = false; }
        else if (Op != "+") return false;
        return Fit(Out);
    }
    if (K != "BinaryOperator") return false;
    const std::string Op = E.value("opcode", std::string());
    FConstVal L, R;
    if (!Nth(E, 0) || !Nth(E, 1) || !FoldConst(*Nth(E, 0), L) || !FoldConst(*Nth(E, 1), R)) return false;
    Out = {};
    if (L.bFloat || R.bFloat)
    {
        Out.bFloat = true;
        if (Op == "+") Out.F = L.Num() + R.Num();
        else if (Op == "-") Out.F = L.Num() - R.Num();
        else if (Op == "*") Out.F = L.Num() * R.Num();
        else if (Op == "/" && R.Num() != 0) Out.F = L.Num() / R.Num();
        else return false;
        return Fit(Out);
    }
    if (Op == "+") Out.I = L.I + R.I;
    else if (Op == "-") Out.I = L.I - R.I;
    else if (Op == "*") Out.I = L.I * R.I;
    else if (Op == "/" && R.I != 0) Out.I = L.I / R.I;
    else if (Op == "%" && R.I != 0) Out.I = L.I % R.I;
    else if (Op == "&") Out.I = L.I & R.I;
    else if (Op == "|") Out.I = L.I | R.I;
    else if (Op == "^") Out.I = L.I ^ R.I;
    else if (Op == "<<" && R.I >= 0 && R.I < 64) Out.I = int64(uint64(L.I) << R.I);
    else if (Op == ">>" && R.I >= 0 && R.I < 64) Out.I = L.I >> R.I;
    else return false;
    return Fit(Out);
}

bool FCompiler::ConstToArg(const FConstVal& V, const std::string& Type, FArgIR& Out) const
{
    const EStrKind VK = StrKindOf(Canon(Type));
    if (VK != SK_Float && VK != SK_Bool && VK != SK_Byte && VK != SK_Int64 && VK != SK_Int) return false;
    Out = FArgIR();
    Out.K = VK == SK_Float ? FArgIR::Float : VK == SK_Bool ? FArgIR::Bool : VK == SK_Byte ? FArgIR::Byte
          : VK == SK_Int64 ? FArgIR::Int64 : FArgIR::Int;
    Out.F = float(V.Num());
    Out.B = V.Num() != 0;
    Out.I = int32(V.I);
    Out.I64 = V.I;
    return true;
}

bool FCompiler::AssetRef(const Json& N, FBlueprintClass& BP, FIndex* Out)
{
    if (Kind(N) != "UnaryOperator" || N.value("opcode", std::string()) != "&") return false;
    const Json* Ref = Strip(First(N));
    if (!Ref || Kind(*Ref) != "DeclRefExpr" || !Ref->contains("referencedDecl")) return false;
    const Json& D = (*Ref)["referencedDecl"];
    const FRecord* R = D.contains("type") ? Find(StripTypeKeywords(D["type"].value("qualType", std::string()))) : nullptr;
    if (Kind(D) != "VarDecl" || !R || R->bIsStruct) return false;

    std::string Package;
    const std::string Var = Name(D);
    if (auto At = AssetPaths.find(Var); At != AssetPaths.end()) Package = At->second;
    else if (std::any_of(AssetDecls.begin(), AssetDecls.end(), [&](const Json* A) { return Name(*A) == Var; }))
        Package = ModPackage + "/" + Var;
    else return false;

    const bool bNative = R->IsNative();
    *Out = BP.Asset(bNative ? R->UePackage : ModPackage + "/" + R->CppName, bNative ? R->UeName : R->CppName + "_C",
                    Package, Package.substr(Package.rfind('/') + 1));
    return true;
}

bool FCompiler::LowerDefault(const Json& F, FPropertyDef& PD, FBlueprintClass& BP, std::string* Err, const Json* Init,
                             bool bKeepZero)
{
    const Json* const Whole = Init ? Init : First(F);
    Init = Strip(Whole);
    if (!Init) return true;
    std::string K = Kind(*Init);

    /* `TArray<uint8> Blob = __EmbedFile__("rel/path")`: the file's bytes, read here at build time and
       written into the CDO one element per byte. The path is relative to the mod source being compiled. */
    if (std::string Rel; PD.Type == "ArrayProperty" && CallsFunction(*Init, "__EmbedFile__"))
    {
        if (!PD.Inner || PD.Inner->Type != "ByteProperty" || !PD.Inner->StructName.empty())
        { *Err = "__EmbedFile__ initialises a TArray<uint8>: " + Name(F); return false; }
        if (!FindLiteral(*Init, Rel))
        { *Err = "__EmbedFile__: the path must be a string literal: " + Name(F); return false; }
        const std::string Path = (std::filesystem::path(SourceDir) / Rel).string();
        const std::string Bytes = ReadText(Path);
        if (Bytes.empty()) { *Err = "__EmbedFile__: cannot read (or empty): " + Path; return false; }
        PD.Default.Items.reserve(Bytes.size());
        for (char C : Bytes) { FDefaultValue B; B.K = FDefaultValue::Int; B.I = uint8(C); PD.Default.Items.push_back(B); }
        PD.Default.K = FDefaultValue::Array;
        printf("  %-14s -> %s  (%d byte%s)\n", "embed", Name(F).c_str(), int32(Bytes.size()), Bytes.size() == 1 ? "" : "s");
        return true;
    }

    const bool bMap = PD.Type == "MapProperty";
    /* `= UE_ENUM_MAP(EMood)`: one pair per enumerator, whichever side of the map the enum is on. C++ has already
       checked that the map is over that enum (the conversion operators in UeMeta.h), so the property says which. */
    if (bMap && PD.Inner && PD.Value && CallsFunction(*Init, "__EnumMap__"))
    {
        const bool bEnumKey = !PD.Inner->StructName.empty();
        const FPropertyDef& EnumSide = bEnumKey ? *PD.Inner : *PD.Value;
        /* StructName is the engine's name (`TextureGroup`); the declaration is found by the C++ one (`ETextureGroup`). */
        std::string CppEnum = EnumSide.StructName;
        for (const auto& E : Enums) if (E.second.UeName == EnumSide.StructName) CppEnum = E.first;
        const std::vector<std::pair<std::string, int64>>* Names = nullptr;
        for (const auto& E : EnumDecls)
            if (E.first == CppEnum || (E.first.size() > CppEnum.size() + 2
                && E.first.compare(E.first.size() - CppEnum.size() - 2, std::string::npos, "::" + CppEnum) == 0))
                Names = &E.second;
        if (!Names || EnumSide.StructName.empty()) { *Err = "UE_ENUM_MAP: " + Name(F) + " is not a map over an enum"; return false; }
        for (const auto& Entry : *Names)
        {
            /* UHT's and the cooker's own closing enumerator is not one of the enum's values. */
            /* `PF_MAX_0` is Dumper-7's spelling of a PF_MAX that collided. */
            std::string Closer = Entry.first;
            while (!Closer.empty() && std::isdigit(uint8(Closer.back()))) Closer.pop_back();
            if (Closer.size() > 5 && Closer.back() == '_' && Closer.compare(Closer.size() - 5, 5, "_MAX_") == 0) continue;
            if (Entry.first.size() > 4 && Entry.first.compare(Entry.first.size() - 4, 4, "_MAX") == 0) continue;
            FDefaultValue Enum, Text;
            Enum.K = Text.K = FDefaultValue::Str;
            Enum.S = EnumSide.StructName + "::" + Entry.first;
            Text.S = Entry.first;
            PD.Default.Items.push_back(bEnumKey ? Enum : Text);
            PD.Default.Items.push_back(bEnumKey ? Text : Enum);
        }
        PD.Default.K = FDefaultValue::Array;
        return true;
    }
    if ((PD.Type == "ArrayProperty" || PD.Type == "SetProperty" || bMap) && PD.Inner && First(*Init))
    {
        /* The container's initializer_list constructor: the braces are the InitListExpr under it. A map's
           elements are TPair lists; Items then alternates key, value. */
        const Json* List = Init;
        while (List && Kind(*List) != "InitListExpr") List = First(*List);
        if (!List || (bMap && !PD.Value))
        { *Err = Name(F) + ": a container default is a braced list, `= { 1, 2 }`, of values a lone member could take"; return false; }
        bool bOk = true;
        auto Add = [&](const FPropertyDef& Of, const Json* E) {
            FPropertyDef Element = Of;
            bOk = bOk && E && LowerDefault(F, Element, BP, Err, E);
            PD.Default.Items.push_back(Element.Default);
            PD.Default.Items.back().Members = Element.Members;      // a struct element's members travel with it
        };
        ForEach(*List, [&](const Json& E) {
            if (!bMap) { Add(*PD.Inner, &E); return; }
            const Json* Pair = &E;
            while (Pair && Kind(*Pair) != "InitListExpr") Pair = First(*Pair);
            Add(*PD.Inner, Pair ? Nth(*Pair, 0) : nullptr);
            Add(*PD.Value, Pair ? Nth(*Pair, 1) : nullptr);
        });
        if (!bOk && Err->empty()) *Err = "a TMap default is a list of { key, value } pairs: " + Name(F);
        PD.Default.K = FDefaultValue::Array;
        return bOk;
    }
    if (FIndex Asset; PD.Type == "ObjectProperty" && AssetRef(*Init, BP, &Asset))
    {
        PD.Default.K = FDefaultValue::Obj;
        PD.Default.Object = Asset;
        return true;
    }
    const bool bNeg = K == "UnaryOperator" && Init->value("opcode", std::string()) == "-";
    if (bNeg)
    {
        Init = Strip(First(*Init));
        K = Init ? Kind(*Init) : std::string();
    }
    if (!bNeg && (K == "CXXNullPtrLiteralExpr" || (K == "CXXConstructExpr" && !First(*Init)))) return true;

    /* A struct value: `FFloatInterval(1, 5)` or `{1, 5}`, one argument per member in declaration
       order. The members go on the property, each with its own default, and the writer turns them
       into nested tags - or into raw bytes when the struct has a native Serialize. */
    if (!bNeg && PD.Type == "StructProperty"
        && (K == "CXXConstructExpr" || K == "CXXTemporaryObjectExpr" || K == "InitListExpr"))
    {
        const FRecord* SR = Find(StripTypeKeywords(TypeOf(*Init)));
        if (!SR) { *Err = "unknown struct type in an initializer: " + TypeOf(*Init); return false; }
        std::vector<const Json*> Args;
        ForEach(*Init, [&](const Json& A) { if (Kind(A) != "CXXDefaultArgExpr") Args.push_back(&A); });
        if (Args.size() != SR->Fields.size())
        {
            *Err = SR->CppName + " takes one value per member (" + std::to_string(SR->Fields.size())
                 + "), in declaration order: " + Name(F);
            return false;
        }
        auto Members = std::make_shared<std::vector<FPropertyDef>>();
        for (size_t I = 0; I < Args.size(); ++I)
        {
            FPropertyDef MD;
            const std::string MName = UeNameOf(SR, Name(*SR->Fields[I]));
            if (!TypeToProperty(TypeOf(*SR->Fields[I]), MName, 0, "member " + MName + " of " + SR->CppName,
                                BP, &MD, Err))
                return false;
            /* Every member is written, so a zero is a value here and not "leave it out". */
            if (!LowerDefault(*SR->Fields[I], MD, BP, Err, Args[I], /*bKeepZero=*/true)) return false;
            Members->push_back(MD);
        }
        PD.Members = Members;
        PD.Default.K = FDefaultValue::Struct;
        return true;
    }

    FDefaultValue& D = PD.Default;
    const std::string& T = PD.Type;
    /* A number is whatever the initializer comes to: `40`, `-1.5f`, `kMax * 2 + 1`, `1 << 3 | 2`. An enum-typed
       byte keeps its named-constant path below. */
    if (FConstVal V; (T == "IntProperty" || T == "Int64Property" || (T == "ByteProperty" && PD.StructName.empty())
                      || T == "FloatProperty" || T == "BoolProperty") && FoldConst(*Whole, V))
    {
        const double Num = V.Num();
        const int64 Int = V.bFloat ? int64(V.F) : V.I;
        if (T == "FloatProperty") { D.K = bKeepZero || Num != 0.0 ? FDefaultValue::Float : FDefaultValue::None; D.F = Num; }
        else if (T == "BoolProperty") { D.K = bKeepZero || Num != 0.0 ? FDefaultValue::Bool : FDefaultValue::None; D.I = Num != 0.0; }
        else { D.K = bKeepZero || Int != 0 ? FDefaultValue::Int : FDefaultValue::None; D.I = Int; }
        return true;
    }
    if (!bNeg && K == "DeclRefExpr" && (T == "ByteProperty" || T == "EnumProperty") && !PD.StructName.empty()
        && (*Init)["referencedDecl"].value("kind", std::string()) == "EnumConstantDecl")
    {
        D.S = PD.StructName + "::" + Name((*Init)["referencedDecl"]);
        D.K = !bKeepZero && D.S == PD.EnumZero ? FDefaultValue::None : FDefaultValue::Str;
        return true;
    }
    if (!bNeg && K == "StringLiteral" && (T == "StrProperty" || T == "NameProperty" || T == "TextProperty"))
    {
        D.S = Unquote(Init->value("value", std::string()));
        D.K = !bKeepZero && D.S.empty() ? FDefaultValue::None : FDefaultValue::Str;
        return true;
    }
    *Err = Name(F) + ": a default is a value known when the mod is built - a literal, a constant expression over "
           "literals, enum constants and constexpr variables, a braced struct, or an &Asset. A function call counts only "
           "through `constexpr T k = F();` with F consteval, which clang runs itself. Anything computed when the game "
           "runs belongs in ReceiveBeginPlay or UserConstructionScript";
    return false;
}

FIndex FCompiler::ClassImportOf(const FRecord& R, FBlueprintClass& BP) const
{
    if (R.IsNative()) return BP.EngineClass(R.UePackage, R.UeName);
    if (&R == Cur) return BP.ClassIndex();
    return BP.EngineClass(ModPackage + "/" + R.CppName, R.CppName + "_C");
}

/*
execMap_Find and execArray_Get write their out value in place only when the variable's property class is the value
property's; otherwise into a scratch that is thrown away. A nested container's value property is its wrapper struct,
so Call's argument OutArg becomes a wrapper temp, and Copy (whose Var is the real destination) stores the temp's Value
member after the call. Call becomes an __Inline__ block; its result, when ResultType is not empty, is the call's value.
*/
bool FCompiler::NestedWrapperOut(const std::string& ContainerType, size_t OutArg, const std::string& ResultType, FStmtIR Copy,
                                 FBlueprintClass& BP, FArgIR& Call, std::string* Err)
{
    int32 Size = 16, Align = 8;
    if (!LayoutOf(ContainerType, &Size, &Align, Err)) return false;
    const FIndex Wrapper = NestedWrapperImport(ContainerType, BP);
    const std::string Tmp = "__Wrap" + std::to_string(ReadTmpCounter++) + "__";
    FPropertyDef W = StructParam(Tmp, Wrapper, NestedWrapper(ContainerType), Size, 0);
    W.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
    CurLocals->push_back(W);

    FArgIR TmpArg;
    TmpArg.K = FArgIR::Local;
    TmpArg.S = Tmp;
    FCallIR Inner = *Call.Sub;
    Inner.Args[OutArg] = TmpArg;

    auto Body = std::make_shared<std::vector<FStmtIR>>();
    std::string Result;
    if (ResultType.empty())
    {
        Body->emplace_back();
        Body->back().K = FStmtIR::StaticCall;
        Body->back().Call = Inner;
    }
    else
    {
        Result = "__Found" + std::to_string(ReadTmpCounter++) + "__";
        FPropertyDef R;
        if (!TypeToProperty(ResultType, Result, 0, "a lookup result", BP, &R, Err)) return false;
        R.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
        CurLocals->push_back(R);
        Body->emplace_back();
        Body->back().K = FStmtIR::Assign;
        Body->back().Var.K = FArgIR::Local;
        Body->back().Var.S = Result;
        Body->back().Var.LetOp = LetOpFor(ResultType);
        Body->back().bAssignLocal = true;
        Body->back().Value.K = FArgIR::Call;
        Body->back().Value.InnerType = ResultType;
        Body->back().Value.Sub = std::make_shared<FCallIR>(Inner);
    }
    Copy.Value.K = FArgIR::Member;
    Copy.Value.S = "Value";
    Copy.Value.Owner = Wrapper;
    Copy.Value.Base = std::make_shared<FArgIR>(TmpArg);
    Copy.Value.LetOp = Copy.Var.LetOp;
    Copy.Value.InnerType = ContainerType;
    Body->push_back(std::move(Copy));

    auto Block = std::make_shared<std::vector<FStmtIR>>(1);
    (*Block)[0].K = FStmtIR::Block;
    (*Block)[0].Body = Body;
    Call = FArgIR();
    Call.K = FArgIR::Call;
    Call.InnerType = ResultType;
    Call.Sub = std::make_shared<FCallIR>();
    Call.Sub->Intrinsic = "__Inline__";
    Call.Sub->Inline = Block;
    Call.Sub->InlineResult = Result;
    Call.Sub->InlineType = ResultType;
    return true;
}

/* A class the headers only forward-declare has no record, so nothing says what package to import it from. A
   Blueprint class's header is named after it. */
static std::string UnknownClass(const std::string& Where, const std::string& QualType, std::string Class)
{
    Class = StripTypeKeywords(Class);
    const size_t Leaf = Class.rfind("::");
    if (Class.size() > 2 && Class.compare(Class.size() - 2, 2, "_C") == 0)
        return Where + ": " + Class + " is only forward-declared here - #include \"UeApi/Game/"
             + Class.substr(Leaf == std::string::npos ? 0 : Leaf + 2) + ".h\"";
    return "TODO: unimplemented " + Where + ": " + QualType;
}

bool FCompiler::TypeToProperty(const std::string& QualType, const std::string& PName, uint64 ExtraFlags,
                               const std::string& Where, FBlueprintClass& BP, FPropertyDef* Out, std::string* Err)
{
    const std::string Type = StripTypeKeywords(QualType);
    std::string Inner;
    for (const char* Tpl : { "TArray", "TSet", "TMap" })
    {
        if (!TemplateArg(Type, Tpl, &Inner)) continue;
        const std::vector<std::string> Args = SplitTemplateArgs(Inner);
        std::vector<FPropertyDef> Parts;
        for (const std::string& A : Args)
        {
            FPropertyDef E;
            if (IsContainerType(A))
            {
                int32 Size = 16, Align = 8;
                if (!LayoutOf(A, &Size, &Align, Err)) return false;
                E = StructParam(PName, NestedWrapperImport(A, BP), NestedWrapper(A), Size, 0);
            }
            else if (!TypeToProperty(A, PName, 0, Where + " element", BP, &E, Err)) return false;
            Parts.push_back(E);
        }
        if (Tpl[1] == 'A')      *Out = ArrayParam(PName, Parts[0], ExtraFlags);
        else if (Tpl[1] == 'S') *Out = SetParam(PName, Parts[0], ExtraFlags);
        else if (Parts.size() == 2) *Out = MapParam(PName, Parts[0], Parts[1], ExtraFlags);
        else { *Err = "TODO: unimplemented " + Where + ": " + QualType; return false; }
        return true;
    }
    if (TemplateArg(Type, "TScriptInterface", &Inner))
    {
        const FRecord* IR = Find(Inner);
        if (!IR || IR->bIsStruct) { *Err = UnknownClass(Where, QualType, Inner); return false; }
        *Out = InterfaceParam(PName, ClassImportOf(*IR, BP), ExtraFlags);
        return true;
    }
    for (const char* Tpl : { "TSubclassOf", "TSoftObjectPtr", "TSoftClassPtr" })
    {
        if (!TemplateArg(Type, Tpl, &Inner)) continue;
        const FRecord* IR = Find(Inner);
        if (!IR || IR->bIsStruct) { *Err = "TODO: unimplemented " + Where + ": " + QualType; return false; }
        const FIndex Meta = ClassImportOf(*IR, BP);
        const FIndex ClassClass = BP.EngineClass("/Script/CoreUObject", "Class");
        if (Tpl[1] == 'S' && Tpl[2] == 'u') *Out = ClassParam(PName, ClassClass, Meta, ExtraFlags);
        else if (Tpl[5] == 'O')             *Out = SoftObjectParam(PName, Meta, ExtraFlags);
        else                                *Out = SoftClassParam(PName, ClassClass, Meta, ExtraFlags);
        return true;
    }
    if (Type == "UClass *" || Type == "UClass*")
    {
        *Out = ClassParam(PName, BP.EngineClass("/Script/CoreUObject", "Class"),
                          BP.EngineClass("/Script/CoreUObject", "Object"), ExtraFlags);
        return true;
    }
    if (Type == "float") { *Out = FloatParam(PName, ExtraFlags); return true; }
    if (Type == "int" || Type == "int32") { *Out = IntParam(PName, ExtraFlags); return true; }
    if (Type == "int64" || Type == "long long") { *Out = Int64Param(PName, ExtraFlags); return true; }
    if (Type == "bool") { *Out = BoolParam(PName, ExtraFlags); return true; }
    if (Type == "uint8" || Type == "unsigned char") { *Out = ByteParam(PName, ExtraFlags); return true; }
    if (Type == "char *" || Type == "char*" || Type == "FString")
    { *Out = StringParam(PName, ExtraFlags); return true; }
    if (Type == "FName") { *Out = NameParam(PName, ExtraFlags); return true; }
    if (Type == "FText") { *Out = TextParam(PName, ExtraFlags); return true; }
    if (auto E = Enums.find(Type); E != Enums.end())
    {
        if (E->second.Underlying == "int32" || E->second.Underlying == "int64")
        {
            /* An EnumProperty over an Int/Int64Property, the way UHT reflects `enum class : int32`. No editor-made
               Blueprint has one, but FEnumProperty takes any numeric underlying property and UEnum values are int64. */
            const bool b64 = E->second.Underlying == "int64";
            *Out = b64 ? Int64Param(PName, ExtraFlags) : IntParam(PName, ExtraFlags);
            Out->Type = "EnumProperty";
            Out->Extra = BP.Enum(E->second.Package, E->second.UeName);
            Out->StructName = E->second.UeName;
            Out->EnumZero = E->second.UeName + "::" + E->second.First;
            Out->Inner = std::make_shared<FPropertyDef>(b64 ? Int64Param("UnderlyingType") : IntParam("UnderlyingType"));
            Out->Inner->PropertyFlags = 0;
            return true;
        }
        if (E->second.Underlying != "uint8")
        { *Err = "TODO: unimplemented " + Where + ": " + QualType + " (a uint8, int32 or int64 enum only)"; return false; }
        *Out = ByteParam(PName, ExtraFlags);
        Out->Extra = BP.Enum(E->second.Package, E->second.UeName);
        Out->StructName = E->second.UeName;
        Out->EnumZero = E->second.UeName + "::" + E->second.First;
        return true;
    }
    if (auto S = Structs.find(Type); S != Structs.end())
    {
        *Out = StructParam(PName, BP.ScriptStruct(S->second.Package, S->second.UeName),
                           S->second.UeName, S->second.Size, ExtraFlags);
        return true;
    }

    if (const FRecord* SR = Find(Type); SR && SR->IsModStruct())
    {
        int32 Size = 0, Align = 0;
        if (!StructLayout(*SR, &Size, &Align, Err)) return false;
        *Out = StructParam(PName, BP.ScriptStruct(SR->IsNative() ? SR->UePackage : ModPackage + "/" + SR->CppName, SR->CppName),
                           SR->CppName, Size, ExtraFlags);
        return true;
    }

    /* Object pointer, spelled `[const] class X *`. UClass* is a reflected UClass reference,
       so it emits an FClassProperty (PropertyClass = UClass, MetaClass = UObject "any class")
       rather than an FObjectProperty; that also makes __ClassOf__(Out) resolve to
       "ClassProperty", matching real ClassProperty fields on target objects. */
    const size_t Star = Type.find('*');
    const std::string ClassName = Star == std::string::npos ? std::string()
                                                            : StripTypeKeywords(Type.substr(0, Star));
    if (ClassName == "UClass")
    {
        const FIndex UClassImp = BP.EngineClass("/Script/CoreUObject", "Class");
        const FIndex UObjectImp = BP.EngineClass("/Script/CoreUObject", "Object");
        *Out = ClassParam(PName, UClassImp, UObjectImp, ExtraFlags);
        return true;
    }
    const FRecord* PR = ClassName.empty() ? nullptr : Find(ClassName);
    if (!PR || PR->bIsStruct) { *Err = UnknownClass(Where, QualType, ClassName); return false; }
    *Out = ObjectParam(PName, PR->IsNative() ? BP.EngineClass(PR->UePackage, PR->UeName)
                                             : BP.EngineClass(ModPackage + "/" + PR->CppName, PR->CppName + "_C"),
                       ExtraFlags);
    return true;
}

bool FCompiler::LayoutOf(const std::string& QualType, int32* Size, int32* Align, std::string* Err)
{
    const std::string T = StripTypeKeywords(QualType);
    if (T == "float" || T == "int" || T == "int32" || T == "uint32" || T == "unsigned int") { *Size = 4; *Align = 4; return true; }
    if (T == "int64" || T == "long long" || T == "uint64" || T == "unsigned long long" || T == "double")
    { *Size = 8; *Align = 8; return true; }
    if (T == "int16" || T == "uint16" || T == "short" || T == "unsigned short") { *Size = 2; *Align = 2; return true; }
    if (T == "bool" || T == "uint8" || T == "unsigned char" || T == "int8" || T == "signed char" || T == "char")
    { *Size = 1; *Align = 1; return true; }
    if (T == "FName") { *Size = 8; *Align = 4; return true; }
    if (T == "FString" || T == "char *" || T == "char*") { *Size = 16; *Align = 8; return true; }
    if (T == "FText") { *Size = 24; *Align = 8; return true; }
    if (!T.empty() && T.back() == '*') { *Size = 8; *Align = 8; return true; }
    if (auto E = Enums.find(T); E != Enums.end() && (E->second.Underlying == "int32" || E->second.Underlying == "int64"))
    { *Size = *Align = E->second.Underlying == "int32" ? 4 : 8; return true; }
    if (Enums.count(T)) { *Size = 1; *Align = 1; return true; }
    std::string Inner;
    if (TemplateArg(T, "TSubclassOf", &Inner)) { *Size = 8; *Align = 8; return true; }
    if (TemplateArg(T, "TArray", &Inner) || TemplateArg(T, "TScriptInterface", &Inner)) { *Size = 16; *Align = 8; return true; }
    if (TemplateArg(T, "TSet", &Inner) || TemplateArg(T, "TMap", &Inner)) { *Size = 80; *Align = 8; return true; }
    if (TemplateArg(T, "TSoftObjectPtr", &Inner) || TemplateArg(T, "TSoftClassPtr", &Inner)) { *Size = 40; *Align = 8; return true; }
    if (auto S = Structs.find(T); S != Structs.end()) { *Size = S->second.Size; *Align = S->second.Align; return true; }
    if (const FRecord* SR = Find(T); SR && SR->bIsStruct) return StructLayout(*SR, Size, Align, Err);
    *Err = "TODO: unimplemented struct member type: " + QualType;
    return false;
}

/* UStruct::Link: members at their natural alignment, size rounded up to the largest one. */
bool FCompiler::StructLayout(const FRecord& R, int32* Size, int32* Align, std::string* Err)
{
    int32 Offset = 0, MaxAlign = 1;
    for (const Json* F : R.Fields)
    {
        int32 S = 0, A = 1;
        if (!LayoutOf(TypeOf(*F), &S, &A, Err)) return false;
        Offset = (Offset + A - 1) / A * A + S;
        MaxAlign = std::max(MaxAlign, A);
    }
    *Align = MaxAlign;
    *Size = std::max(1, (Offset + MaxAlign - 1) / MaxAlign * MaxAlign);
    return true;
}

bool FCompiler::LowerParams(const Json& M, const std::string& Fn, FBlueprintClass& BP,
                            std::vector<FPropertyDef>& Params, std::string* Err)
{
    CurrentOutParms.clear();
    bool bOk = true;
    ForEach(M, [&](const Json& C) {
        if (Kind(C) != "ParmVarDecl" || !bOk) return;
        std::string Type = TypeOf(C);
        const std::string PName = Name(C);
        bool bOutParm = false;
        while (!Type.empty() && (Type.back() == '&' || Type.back() == ' ' || Type.back() == '\t'))
        {
            if (Type.back() == '&') bOutParm = true;
            Type.pop_back();
        }
        FPropertyDef PD;
        bOk = TypeToProperty(Type, PName, bOutParm ? uint64(CPF_OutParm | CPF_ReferenceParm) : 0,
                             "parameter " + PName + " on " + Fn, BP, &PD, Err);
        if (!bOk) return;
        Params.push_back(PD);
        if (bOutParm) CurrentOutParms.insert(PName);
    });
    return bOk;
}

/*
A UE_INTERFACE: a BlueprintGeneratedClass whose super is UInterface, holding one empty UFunction per
declared method. Measured on BPI_InputKeyHandler: ClassFlags CLASS_Parsed | CLASS_Interface |
CLASS_CompiledFromBlueprint, no SCS, the CDO archetyped on Default__Interface. An interface declares
no state, so there are no variables to carry.
*/
bool FCompiler::GenerateInterface(const FRecord& R, const std::string& OutDir, std::string* Err)
{
    Cur = &R;
    const std::string PackageName = ModPackage + "/" + R.CppName;
    FPackage P(PackageName);
    StampIdentity(P, PackageName);

    /* `class IChild : public IParent`: the editor never offers it for a Blueprint Interface, but a UClass has a
       super like any other, and the runtime asks for nothing more. UClass::ImplementsInterface (Class.cpp:4649)
       tests each Interfaces entry with IsChildOf - "SomeInterface might be a base interface of our implemented
       interface" - so an implementing class lists the child alone, which is also all the Kismet compiler writes
       (KismetCompiler.cpp:2420). One super, so one parent. */
    if (!R.Interfaces.empty())
    { *Err = R.CppName + ": an interface extends one interface at most, as a UClass has one super"; return false; }
    const FRecord* Parent = R.Base.empty() ? nullptr : Find(R.Base);
    if (!R.Base.empty() && (!Parent || Parent->bIsStruct || (Parent->IsNative() ? Parent->CppName[0] != 'I' : !Parent->bIsInterface)))
    { *Err = R.CppName + " extends " + R.Base + ", which is not an interface"; return false; }
    const bool bParentIsLocal = Parent && !Parent->IsNative();
    const std::string ParentPkg = !Parent ? "/Script/CoreUObject" : bParentIsLocal ? ModPackage + "/" + Parent->CppName : Parent->UePackage;
    FBlueprintClass BP(P, R.CppName + "_C", ParentPkg,
                       !Parent ? "Interface" : bParentIsLocal ? Parent->CppName + "_C" : Parent->UeName,
                       ParentPkg.compare(0, 6, "/Game/") == 0);
    BP.SetIsActor(false);
    BP.SetClassFlags(CLASS_Parsed | CLASS_Interface | CLASS_CompiledFromBlueprint);

    /* A variable on an interface is this compiler's own idea, not the engine's: it becomes a property of every
       class that implements the interface (Generate), so none is written here. What cannot move that way is
       refused. */
    for (const Json* F : R.Fields)
    {
        if (R.Components.count(Name(*F)))
        { *Err = R.CppName + "::" + Name(*F) + ": an interface cannot declare a UE_COMPONENT"; return false; }
        if (StripTypeKeywords(TypeOf(*F)).compare(0, 25, "TMulticastInlineDelegate<") == 0)
        { *Err = R.CppName + "::" + Name(*F) + ": an interface cannot declare a UE_DISPATCHER"; return false; }
    }

    for (const auto& Entry : R.Methods)
    {
        /* A RepNotify of one of its variables is the implementing class's function, never called through the
           interface. */
        if (std::any_of(R.Replicated.begin(), R.Replicated.end(), [&](const auto& Rep) {
                return Rep.second.substr(0, Rep.second.find(':')) == Entry.first; })) continue;
        std::vector<FPropertyDef> Params;
        if (!LowerParams(*Entry.second, Entry.first, BP, Params, Err)) return false;

        const std::string FnQual = (*Entry.second)["type"].value("qualType", std::string());
        const size_t LParen = FnQual.find('(');
        std::string RetType = LParen == std::string::npos ? FnQual : FnQual.substr(0, LParen);
        while (!RetType.empty() && (RetType.back() == ' ' || RetType.back() == '\t')) RetType.pop_back();
        if (!RetType.empty() && RetType != "void")
        {
            FPropertyDef PD;
            if (!TypeToProperty(RetType, "ReturnValue", CPF_ReturnParm | CPF_OutParm,
                                "return type on " + Entry.first, BP, &PD, Err)) return false;
            PD.PropertyFlags &= ~uint64(CPF_BlueprintVisible | CPF_BlueprintReadOnly);
            Params.push_back(PD);
        }

        /* `[I]` the flag set, not measured: the uncooked asset's field stream is not what dumpstruct
           reads. An implementing class re-declares the function with its own flags anyway, and those
           are the ones a caller dispatches through. */
        const bool bOut = std::any_of(Params.begin(), Params.end(),
                                      [](const FPropertyDef& P) { return (P.PropertyFlags & CPF_OutParm) != 0; });
        BP.AddFunction(Entry.first, Null(), Params,
                       [](FScript& S, FIndex) { S.Return(); S.EndOfScript(); },
                       FUNC_Public | FUNC_BlueprintCallable | FUNC_BlueprintEvent
                           | (bOut ? FUNC_HasOutParms : 0));
    }

    BP.Finish();
    if (!P.Save(OutDir + "/" + R.CppName, Err)) return false;
    RegistryRows.push_back({ PackageName, R.CppName, "BlueprintGeneratedClass" });
    printf("  %-14s -> %s.uasset  (interface, %d functions)\n", R.CppName.c_str(), R.CppName.c_str(),
           int32(R.Methods.size()));
    return true;
}

bool FCompiler::GenerateStruct(const FRecord& R, const std::string& OutDir, std::string* Err)
{
    Cur = &R;
    const std::string PackageName = ModPackage + "/" + R.CppName;
    FPackage P(PackageName);
    StampIdentity(P, PackageName);
    FBlueprintClass BP(P, R.CppName, "", "", false);

    for (const Json* F : R.Fields)
    {
        FPropertyDef PD;
        const std::string Field = IsInternalViewStruct(R.CppName) ? Name(*F)
                                                                  : ModFieldName(PackageName, Name(*F));
        if (!TypeToProperty(TypeOf(*F), Field, 0, "member " + Name(*F), BP, &PD, Err)) return false;
        if (!LowerDefault(*F, PD, BP, Err)) return false;
        PD.PropertyFlags = CPF_Edit | CPF_BlueprintVisible;
        BP.AddVariable(PD);
    }

    const uint32 H = StrCrc32(PackageName);
    const uint32 Guid[4] = { ~H, H * 2654435761u, H ^ 0x9E3779B9u, H };
    /* The editor stub reads the members before FinishStruct consumes them; both share the same Guid,
       so the cooked and uncooked assets carry identical member names and struct identity. */
    if (!ApiDir.empty() && !IsInternalViewStruct(R.CppName) && !BP.WriteApiStruct(ApiDir, Guid, Err))
        return false;
    BP.FinishStruct(Guid);
    if (!P.Save(OutDir + "/" + R.CppName, Err)) return false;
    RegistryRows.push_back({ PackageName, R.CppName, "UserDefinedStruct" });
    printf("  %-14s -> %s.uasset  (struct, %d members)\n", R.CppName.c_str(), R.CppName.c_str(),
           int32(R.Fields.size()));
    return true;
}

/* Each wrapper a container needed, into NestedPackage beside the mod's own folder. Two mods that need the same one
   cook identical packages at the same path. A wrapper's own member may need a deeper one. */
bool FCompiler::GenerateEnum(const std::string& Name, const std::string& OutDir, std::string* Err)
{
    const std::string PackageName = ModPackage + "/" + Name;
    FPackage P(PackageName);
    StampIdentity(P, PackageName);
    FBlueprintClass BP(P, Name, "", "", false);
    if (!ApiDir.empty() && !BP.WriteApiEnum(ApiDir, ModEnums[Name], Err)) return false;
    BP.FinishEnum(ModEnums[Name]);
    if (!P.Save(OutDir + "/" + Name, Err)) return false;
    RegistryRows.push_back({ PackageName, Name, "UserDefinedEnum" });
    printf("  %-14s -> %s.uasset  (enum, %d enumerators)\n", Name.c_str(), Name.c_str(), int32(ModEnums[Name].size()));
    return true;
}

/* Measured on ED_Spider_Grunt. The initializer's semantic form lists the bases first, then every field in order,
   so a designator is found by position. Only a field the braces name is written: the rest stay the CDO's. */
bool FCompiler::GenerateAsset(const Json& Var, const std::string& OutDir, std::string* Err)
{
    const FRecord* R = Find(StripTypeKeywords(TypeOf(Var)));
    if (!R || R->bIsStruct || (R->UeName.empty() && !R->IsGenerated())) return true;    // a plain C++ aggregate
    Cur = nullptr;

    const std::string AssetName = Name(Var);
    const std::string PackageName = ModPackage + "/" + AssetName;
    FPackage P(PackageName);
    StampIdentity(P, PackageName);
    FBlueprintClass BP(P, AssetName, "", "", false);

    const auto Unset = IsUnsetInit;
    std::function<bool(const Json&, const FRecord&)> Fill = [&](const Json& List, const FRecord& Rec) {
        size_t I = 0;
        if (!Rec.Base.empty())
        {
            const FRecord* B = Find(Rec.Base);
            const Json* Sub = Nth(List, I++);
            if (B && Sub && Kind(*Sub) == "InitListExpr" && !Fill(*Sub, *B)) return false;
        }
        I += Rec.Interfaces.size();
        for (const Json* F : Rec.Fields)
        {
            const Json* Init = Strip(Nth(List, I++));
            if (!Init || Unset(*Init)) continue;
            FPropertyDef PD;
            if (!TypeToProperty(TypeOf(*F), UeNameOf(&Rec, Name(*F)), 0, AssetName + "." + Name(*F), BP, &PD, Err)) return false;
            if (!LowerDefault(*F, PD, BP, Err, Init)) return false;
            BP.AddVariable(PD);
        }
        return true;
    };
    if (!Fill(*BracedInit(Var), *R)) return false;

    const std::string ClassPkg = R->IsNative() ? R->UePackage : ModPackage + "/" + R->CppName;
    const std::string ClassName = R->IsNative() ? R->UeName : R->CppName + "_C";
    BP.FinishAsset(BP.EngineClass(ClassPkg, ClassName), BP.ClassDefaultObject(ClassPkg, ClassName));
    if (!P.Save(OutDir + "/" + AssetName, Err)) return false;
    RegistryRows.push_back({ PackageName, AssetName, ClassName });
    printf("  %-14s -> %s.uasset  (asset, a %s)\n", AssetName.c_str(), AssetName.c_str(), R->CppName.c_str());
    return true;
}

bool FCompiler::GenerateNestedWrappers(const std::string& OutDir, std::string* Err)
{
    /* OutDir is Content/<ModPackage without /Game>; the wrappers go to Content/<NestedPackage without /Game>. */
    std::filesystem::path Content(OutDir);
    for (size_t At = ModPackage.find('/', 1); At != std::string::npos; At = ModPackage.find('/', At + 1))
        Content = Content.parent_path();
    const std::filesystem::path Dir = Content / std::string(NestedPackage).substr(6);
    std::set<std::string> Done;
    for (bool bMore = true; bMore;)
    {
        bMore = false;
        const auto Pending = NestedWrappers;
        for (const auto& [Name, Type] : Pending)
        {
            if (!Done.insert(Name).second) continue;
            bMore = true;
            const std::string PackageName = std::string(NestedPackage) + "/" + Name;
            FPackage P(PackageName);
            StampIdentity(P, PackageName);
            FBlueprintClass BP(P, Name, "", "", false);
            FPropertyDef PD;
            if (!TypeToProperty(Type, "Value", 0, "nested container " + Type, BP, &PD, Err)) return false;
            PD.PropertyFlags = CPF_Edit | CPF_BlueprintVisible;
            BP.AddVariable(PD);
            const uint32 H = StrCrc32(PackageName);
            const uint32 Guid[4] = { ~H, H * 2654435761u, H ^ 0x9E3779B9u, H };
            BP.FinishStruct(Guid);
            std::error_code Ec;
            std::filesystem::create_directories(Dir, Ec);
            if (!P.Save((Dir / Name).string(), Err)) return false;
            RegistryRows.push_back({ PackageName, Name, "UserDefinedStruct" });
            printf("  %-14s -> %s/%s.uasset  (wraps %s)\n", "nested", std::string(NestedPackage).c_str(), Name.c_str(), Type.c_str());
        }
    }
    return true;
}

bool FCompiler::Generate(const FRecord& R, const std::string& OutDir, std::string* Err)
{
    const FRecord* B = Find(R.Base);
    if (!B) { *Err = R.CppName + " derives from an undeclared class: " + R.Base; return false; }
    Cur = &R;

    const std::string PackageName = ModPackage + "/" + R.CppName;
    FPackage P(PackageName);
    StampIdentity(P, PackageName);

    /* Parent-is-local decides the spelling; parent-is-Blueprint (/Game) decides the CDO's
       create-before-serialize edge onto it (measured on BP_ThornsComponent). */
    const bool bParentIsLocal = !B->IsNative();
    const std::string ParentPkg = bParentIsLocal ? ModPackage + "/" + B->CppName : B->UePackage;
    FBlueprintClass BP(P, R.CppName + "_C", ParentPkg,
                       bParentIsLocal ? B->CppName + "_C" : B->UeName,
                       ParentPkg.compare(0, 6, "/Game/") == 0);

    std::vector<std::string> Ancestry;
    for (const FRecord* A = &R; A; A = A->Base.empty() ? nullptr : Find(A->Base))
        if (!A->UeName.empty()) Ancestry.push_back(A->UeName);

    const bool bIsActor = std::find(Ancestry.begin(), Ancestry.end(), "Actor") != Ancestry.end();
    BP.SetIsActor(bIsActor);
    BP.SetClassFlags(ClassFlagsFor(Ancestry));

    for (const std::string& I : R.Interfaces)
    {
        const FRecord* IR = Find(I);
        if (!IR || IR->bIsStruct) { *Err = R.CppName + " implements an undeclared interface: " + I; return false; }
        if (!IR->IsNative() && !IR->bIsInterface)
        { *Err = R.CppName + " implements " + I + ", which is not an interface"; return false; }
        /* clang already rejects one listed twice; an ancestor's copy it only warns about. A native
           ancestor's interfaces are not in the dump, so only the mod's classes are checked. */
        /* Through an interface that extends it as well: this class's empty stubs would otherwise override the
           functions the ancestor implemented. */
        for (const FRecord* A = Find(R.Base); A; A = A->Base.empty() ? nullptr : Find(A->Base))
            for (const std::string& Other : A->Interfaces)
                for (const FRecord* Mine : InterfaceChain(IR))
                    for (const FRecord* Theirs : InterfaceChain(Find(Other)))
                        if (Mine == Theirs)
                        { *Err = R.CppName + " implements " + I + ", which its parent " + A->CppName + " already implements"
                                 + (Find(Other) == IR ? "" : " (through " + Other + ")"); return false; }
        /* Two of its own that meet in one interface would declare that one's variables twice. */
        for (const std::string& Other : R.Interfaces)
            if (&Other < &I)
                for (const FRecord* Mine : InterfaceChain(IR))
                    for (const FRecord* Theirs : InterfaceChain(Find(Other)))
                        if (Mine == Theirs)
                        { *Err = R.CppName + " implements " + Other + " and " + I + ", which both extend " + Mine->CppName; return false; }
        BP.AddInterface(ClassImportOf(*IR, BP));
    }

    auto LowerParams = [&](const Json& M, const std::string& Fn, std::vector<FPropertyDef>& Params) {
        return this->LowerParams(M, Fn, BP, Params, Err);
    };
    auto HasOutParm = [](const std::vector<FPropertyDef>& Params) {
        return std::any_of(Params.begin(), Params.end(),
                           [](const FPropertyDef& P) { return (P.PropertyFlags & CPF_OutParm) != 0; });
    };

    /* A UE_DISPATCHER's signature function goes first, so the dispatcher property can name its export.
       Measured on MOD_Proxy_SpawnEnemy: flags BlueprintEvent | BlueprintCallable | Delegate | Public, an
       empty body, listed in Children and FuncMap like any function. */
    CurSignatures.clear();
    bool bReplicatesAnything = false;
    for (const Json* F : R.Fields)
    {
        if (StripTypeKeywords(TypeOf(*F)).compare(0, 25, "TMulticastInlineDelegate<") != 0) continue;
        const std::string SigName = Name(*F) + "__DelegateSignature";
        auto Sig = R.Methods.find(SigName);
        if (Sig == R.Methods.end())
        { *Err = R.CppName + "::" + Name(*F) + ": a dispatcher is declared with UE_DISPATCHER"; return false; }
        std::vector<FPropertyDef> Params;
        if (!LowerParams(*Sig->second, SigName, Params)) return false;
        const uint32 Flags = FUNC_BlueprintEvent | FUNC_BlueprintCallable | FUNC_Delegate | FUNC_Public
                           | (HasOutParm(Params) ? FUNC_HasOutParms : 0);
        CurSignatures[Name(*F)] = BP.AddFunction(SigName, Null(), Params,
                                                 [](FScript& S, FIndex) { S.Return(); S.EndOfScript(); }, Flags);
    }

    /*
    UE_DEFAULTS: `Comp->Field = value;` per statement. The block is never lowered - each assignment
    becomes one tagged property on that component's archetype, which is where the editor keeps a
    per-component default. Anything else in there is refused rather than silently dropped, since a
    statement that looks like it runs and does not is the worst possible failure here.
    */
    std::map<std::string, std::vector<FPropertyDef>> ComponentDefaults;
    struct FOverride { const FRecord* Owner = nullptr; std::vector<FPropertyDef> Defaults; };
    std::map<std::string, FOverride> ComponentOverrides;
    std::map<std::string, FOverride> SubobjectDefaults;
    std::vector<FPropertyDef> InheritedDefaults;
    std::map<std::string, FPropertyDef> InterfaceVarDefaults;     // a default for a variable an implemented interface declares
    if (R.Defaults)
    {
        const std::string Where = R.CppName + "::UE_DEFAULTS";
        auto OwnerOf = [&](const Json& M) {
            auto It = FieldOwner.find(M.value("referencedMemberDecl", std::string()));
            return It == FieldOwner.end() ? std::string() : It->second;
        };
        const Json* Body = nullptr;
        ForEach(*R.Defaults, [&](const Json& C) { if (Kind(C) == "CompoundStmt") Body = &C; });
        bool bOk = true;
        if (Body)
            ForEach(*Body, [&](const Json& S) {
                if (!bOk) return;
                /* Assigning a struct is an operator call, not a BinaryOperator: its inner is the
                   callee then the two operands, so both shapes are read the same way one index on. */
                const Json* Assign = Strip(&S);
                const std::string AK = Assign ? Kind(*Assign) : std::string();
                const bool bOpCall = AK == "CXXOperatorCallExpr";
                const size_t Base = bOpCall ? 1 : 0;
                const Json* Lhs = Assign ? Strip(Nth(*Assign, Base)) : nullptr;
                const Json* Rhs = Assign ? Nth(*Assign, Base + 1) : nullptr;
                const bool bAssign = bOpCall ? IsAssignOperatorCall(*Assign)
                                             : AK == "BinaryOperator"
                                                   && Assign->value("opcode", std::string()) == "=";
                if (!Assign || !bAssign || !Lhs || Kind(*Lhs) != "MemberExpr" || !Rhs)
                { *Err = Where + ": every statement is `Field = value;` or `Component->Field = value;`"; bOk = false; return; }

                /* `Comp->Field` reaches through a component; a bare `Field` targets this class's
                   own CDO. Which of the three destinations a statement means is decided by who
                   DECLARES the member it names, so no Super:: spelling is needed. */
                const Json* Owner = Strip(First(*Lhs));
                const bool bThroughComponent = Owner && Kind(*Owner) == "MemberExpr";
                const std::string CompName = bThroughComponent ? Name(*Owner) : std::string();
                const std::string Declarer = OwnerOf(bThroughComponent ? *Owner : *Lhs);
                const FRecord* DR = Declarer.empty() ? nullptr : Find(Declarer);
                if (!DR)
                { *Err = Where + ": cannot tell which class declares " + Name(*Lhs); bOk = false; return; }
                if (bThroughComponent && !DR->Components.count(CompName) && !DR->IsNative())
                { *Err = Where + ": " + CompName + " is not a UE_COMPONENT"; bOk = false; return; }
                if (!bThroughComponent && DR == &R)
                { *Err = Where + ": " + Name(*Lhs) + " is declared here - give it an initializer instead"; bOk = false; return; }
                /* An interface's variable is this class's own property when this class is the one implementing
                   it; below a parent that does, it is one more inherited property. */
                const bool bInterfaceVar = !bThroughComponent && DR->bIsInterface && InterfaceVarHolder(*DR, &R) == &R;

                FPropertyDef PD;
                /* The tag is read back by the engine's name of the member, which its declarer's header knows. */
                const std::string LhsDeclarer = OwnerOf(*Lhs);
                const std::string TagName = UeNameOf(LhsDeclarer.empty() ? DR : Find(LhsDeclarer), Name(*Lhs));
                if (!TypeToProperty(TypeOf(*Lhs), TagName, 0, Where, BP, &PD, Err)) { bOk = false; return; }
                /* Zero is a real value here: an archetype deltas against the component CDO (where
                   bVisible is already true) and an inherited property against the parent's CDO,
                   not against the type's zero the way a fresh class variable does. */
                if (!LowerDefault(*Lhs, PD, BP, Err, Rhs, /*bKeepZero=*/true)) { bOk = false; return; }
                if (PD.Default.K == FDefaultValue::None)
                { *Err = Where + ": " + Name(*Lhs) + " needs a literal value"; bOk = false; return; }
                if (bInterfaceVar) { InterfaceVarDefaults[Name(*Lhs)] = PD; return; }

                if (!bThroughComponent) { InheritedDefaults.push_back(PD); return; }
                if (DR == &R) { ComponentDefaults[CompName].push_back(PD); return; }
                /* A native parent's component is a default subobject, not an SCS node: it is
                   overridden by an export under this class's CDO, with no handler involved. */
                /* A game Blueprint's component is an SCS node too, and overriding it takes the node's guid, which only
                   a dump can say (`<Comp>__UeScsNode`, from the Dumper-7 fork's `ScsNode=`). Without it the default
                   subobject path below would cook an export the engine never looks at - so that is refused. */
                const bool bBlueprintParent = DR->IsNative() && DR->UeName.size() > 2
                                              && DR->UeName.compare(DR->UeName.size() - 2, 2, "_C") == 0;
                if (bBlueprintParent && !DR->ScsNodes.count(CompName))
                {
                    *Err = Where + ": " + CompName + " is a component of the Blueprint " + DR->UeName + ", and its header does "
                           "not say which SCS node it is - re-dump the game with the Dumper-7 fork (ScsNode=) and regenerate UeApi";
                    bOk = false;
                    return;
                }
                if (DR->IsNative() && !bBlueprintParent)
                {
                    FOverride& Sub = SubobjectDefaults[CompName];
                    Sub.Owner = DR;
                    Sub.Defaults.push_back(PD);
                    return;
                }
                FOverride& O = ComponentOverrides[CompName];
                O.Owner = DR;
                O.Defaults.push_back(PD);
            });
        if (!bOk) return false;
    }

    /* A cooked property carries no offset: FProperty::SetupOffset lays ChildProperties out in order, so emitting them
       by alignment, largest first, is the packing. Only the order moves; the bytecode names a property by path. */
    std::vector<std::pair<int32, FPropertyDef>> ClassVars;
    auto AddVariable = [&](const std::string& Type, const FPropertyDef& PD) {
        int32 Size = 8, Align = 8;
        std::string Ignored;
        if (!LayoutOf(Type, &Size, &Align, &Ignored)) Align = 8;
        ClassVars.emplace_back(Align, PD);
    };
    /* A variable declared on a mod interface is a property of each class that implements it - here, once, and
       found by InterfaceVarHolder from this class's subclasses. Its markers are read off the interface. */
    struct FOwnedField { const Json* F; const FRecord* Decl; };
    std::vector<FOwnedField> OwnedFields;
    for (const Json* F : R.Fields) OwnedFields.push_back({ F, &R });
    for (const std::string& I : R.Interfaces)
        for (const FRecord* Link : InterfaceChain(Find(I)))
            if (Link->bIsInterface)
                for (const Json* F : Link->Fields)
                {
                    if (std::any_of(OwnedFields.begin(), OwnedFields.end(), [&](const FOwnedField& O) { return Name(*O.F) == Name(*F); }))
                    { *Err = R.CppName + ": the variable " + Name(*F) + " of the interface " + Link->CppName + " is declared twice"; return false; }
                    OwnedFields.push_back({ F, Link });
                }
    for (const FOwnedField& Owned : OwnedFields)
    {
        const Json* F = Owned.F;
        const FRecord& Decl = *Owned.Decl;
        const std::string FieldName = Name(*F);
        if (auto Sig = CurSignatures.find(FieldName); &Decl == &R && Sig != CurSignatures.end())
        {
            AddVariable("", DispatcherParam(FieldName, Sig->second));
            continue;
        }
        FPropertyDef PD;
        std::string PErr;
        if (!TypeToProperty(TypeOf(*F), FieldName, 0, "property " + FieldName, BP, &PD, &PErr))
        { *Err = PErr; return false; }
        if (!LowerDefault(*F, PD, BP, Err)) return false;
        /* UE_DEFAULTS naming an interface's variable is this class's own default for it. */
        if (auto Own = InterfaceVarDefaults.find(FieldName); Own != InterfaceVarDefaults.end())
        {
            PD.Default = Own->second.Default;
            PD.Members = Own->second.Members;
            InterfaceVarDefaults.erase(Own);
        }

        /* CPF_Parm would make it part of the call frame; CPF_BlueprintReadOnly would forbid assignment -
           except on a `const` field, where the source forbids it anyway, so the flag is the truth. */
        PD.PropertyFlags = (PD.PropertyFlags & ~uint64(CPF_Parm | CPF_BlueprintReadOnly))
                         | CPF_Edit | CPF_BlueprintVisible | CPF_DisableEditOnInstance;
        if (TypeOf(*F).compare(0, 6, "const ") == 0) PD.PropertyFlags |= CPF_BlueprintReadOnly;
        PD.bApiHidden = Decl.PrivateFields.count(Name(*F)) != 0;
        if (&Decl == &R && R.Components.count(FieldName))
        {
            if (!bIsActor) { *Err = R.CppName + "::" + FieldName + ": only an actor has a construction script"; return false; }
            const size_t Star = TypeOf(*F).find('*');
            const FRecord* CR = Star == std::string::npos ? nullptr
                                                          : Find(StripTypeKeywords(TypeOf(*F).substr(0, Star)));
            if (!CR || !CR->IsNative())
            { *Err = R.CppName + "::" + FieldName + ": a UE_COMPONENT names an engine component class"; return false; }
            bool bIsScene = false, bIsComponent = false;
            for (const FRecord* A = CR; A; A = A->Base.empty() ? nullptr : Find(A->Base))
            {
                bIsScene = bIsScene || A->UeName == "SceneComponent";
                bIsComponent = bIsComponent || A->UeName == "ActorComponent";
            }
            if (!bIsComponent) { *Err = R.CppName + "::" + FieldName + ": " + CR->CppName + " is not a UActorComponent"; return false; }
            /* The variable stays an ordinary ObjectProperty: ExecuteNodeOnActor finds it by name and
               assigns the instance it built from the archetype. */
            BP.AddComponent(FieldName, BP.EngineClass(CR->UePackage, CR->UeName),
                            BP.ClassDefaultObject(CR->UePackage, CR->UeName), bIsScene,
                            ComponentDefaults[FieldName]);
            ComponentDefaults.erase(FieldName);
        }
        if (auto Rep = Decl.Replicated.find(FieldName); Rep != Decl.Replicated.end())
        {
            /* Measured on BP_LiftPod.IsLaunchEnabled: the editor's flags plus CPF_Net, and CPF_RepNotify with the
               function's name when it has one. */
            const std::string Notify = Rep->second.substr(0, Rep->second.find(':'));
            const std::string Cond = Rep->second.substr(Rep->second.find(':') + 1);
            if (HoldsMapOrSet(PD)) { *Err = R.CppName + "::" + FieldName + ": a TMap or TSet does not replicate"; return false; }
            PD.PropertyFlags |= CPF_Net;
            if (!Notify.empty())
            {
                /* An interface variable's RepNotify is declared beside it; a class that leaves it out gets the
                   empty stub every interface function gets. */
                const Json* NotifyFn = nullptr;
                if (auto M = R.Methods.find(Notify); M != R.Methods.end()) NotifyFn = M->second;
                else if (auto D = Decl.Methods.find(Notify); D != Decl.Methods.end()) NotifyFn = D->second;
                if (!NotifyFn || !ParmNames(*NotifyFn).empty())
                { *Err = R.CppName + "::" + FieldName + ": its RepNotify " + Notify + " must be a method of the class taking no parameters"; return false; }
                PD.PropertyFlags |= CPF_RepNotify;
                PD.RepNotify = Notify;
            }
            static const char* const Conditions[] = { "None", "InitialOnly", "OwnerOnly", "SkipOwner", "SimulatedOnly",
                "AutonomousOnly", "SimulatedOrPhysics", "InitialOrOwner", "Custom", "ReplayOrOwner", "ReplayOnly",
                "SimulatedOnlyNoReplay", "SimulatedOrPhysicsNoReplay", "SkipReplay", "Never" };
            const std::string C = Cond.compare(0, 5, "COND_") == 0 ? Cond.substr(5) : Cond;
            const auto At = std::find_if(std::begin(Conditions), std::end(Conditions), [&](const char* N) { return C.empty() ? false : C == N; });
            if (!C.empty() && At == std::end(Conditions))
            { *Err = R.CppName + "::" + FieldName + ": unknown replication condition " + Cond; return false; }
            PD.RepCondition = C.empty() ? 0 : uint8(At - std::begin(Conditions));
            bReplicatesAnything = true;
        }
        AddVariable(TypeOf(*F), PD);
    }
    std::stable_sort(ClassVars.begin(), ClassVars.end(), [](const auto& A, const auto& B) { return A.first > B.first; });
    for (const auto& V : ClassVars) BP.AddVariable(V.second);

    if (!ComponentDefaults.empty())
    { *Err = R.CppName + "::UE_DEFAULTS: " + ComponentDefaults.begin()->first + " is not declared with UE_COMPONENT"; return false; }
    for (const FPropertyDef& V : InheritedDefaults) BP.AddCdoDefault(V);
    /* Resolving the component class from the declaring record's own field, the same walk the
       UE_COMPONENT registration does - a native subobject has no marker to read it off. */
    auto ComponentClassOf = [&](const FRecord& Owner, const std::string& Field) -> const FRecord* {
        const Json* F = nullptr;
        for (const Json* PF : Owner.Fields) if (Name(*PF) == Field) F = PF;
        const size_t Star = F ? TypeOf(*F).find('*') : std::string::npos;
        return Star == std::string::npos ? nullptr
                                         : Find(StripTypeKeywords(TypeOf(*F).substr(0, Star)));
    };
    for (const auto& Entry : SubobjectDefaults)
    {
        const FRecord* CR = ComponentClassOf(*Entry.second.Owner, Entry.first);
        if (!CR || !CR->IsNative())
        { *Err = R.CppName + "::UE_DEFAULTS: cannot resolve the class of " + Entry.first; return false; }
        BP.AddSubobjectOverride(Entry.first, BP.EngineClass(CR->UePackage, CR->UeName),
                                Entry.second.Defaults);
    }
    for (const auto& Entry : ComponentOverrides)
    {
        /* The parent's own archetype, imported as a subobject of its class, is the record's template:
           the tags this class writes on top of it are exactly the overridden values. */
        const FRecord& Owner = *Entry.second.Owner;
        const FRecord* CR = ComponentClassOf(Owner, Entry.first);
        if (!CR || !CR->IsNative())
        { *Err = R.CppName + "::UE_DEFAULTS: cannot resolve the class of " + Entry.first; return false; }

        const std::string OwnerPkg = Owner.IsNative() ? Owner.UePackage : ModPackage + "/" + Owner.CppName;
        const std::string OwnerCls = Owner.IsNative() ? Owner.UeName : Owner.CppName + "_C";
        uint32 NodeGuid[4] = {};
        const std::string VarName = UeNameOf(&Owner, Entry.first);      // the node's name: `Audio Flying`, spaces and all
        if (auto Node = Owner.ScsNodes.find(Entry.first); Node != Owner.ScsNodes.end())
        {
            /* The guid's 16 bytes as the engine holds them, which is four little-endian words on disk as well. */
            if (Node->second.size() != 32)
            { *Err = R.CppName + "::UE_DEFAULTS: " + Entry.first + "__UeScsNode is not 32 hex digits"; return false; }
            for (int B = 0; B < 16; ++B)
                NodeGuid[B / 4] |= uint32(std::stoul(Node->second.substr(size_t(B) * 2, 2), nullptr, 16)) << (8 * (B % 4));
        }
        else
            ScsNodeGuid(OwnerCls, Entry.first, NodeGuid);
        const FIndex OwnerClass = BP.EngineClass(OwnerPkg, OwnerCls);
        BP.AddComponentOverride(VarName, BP.EngineClass(CR->UePackage, CR->UeName),
                                BP.Subobject(CR->UePackage, CR->UeName, OwnerClass,
                                             VarName + "_GEN_VARIABLE"),
                                OwnerClass, NodeGuid, Entry.second.Defaults);
    }

    /* The OOL definition carries body/parms; only the in-class decl carries storageClass. */
    struct FMethod { std::string Name; const Json* Decl; const Json* Def; const Json* Body; };
    auto BodyOf = [](const FRecord& Owner, const std::string& Fn, const Json*& Def) {
        auto DefIt = Owner.MethodDefs.find(Fn);
        if (DefIt != Owner.MethodDefs.end()) Def = DefIt->second;
        const Json* Body = nullptr;
        ForEach(*Def, [&](const Json& C) { if (Kind(C) == "CompoundStmt") Body = &C; });
        return Body;
    };
    std::vector<FMethod> Methods;
    for (const auto& Entry : R.Methods)
    {
        FMethod Fn{ Entry.first, Entry.second, Entry.second, nullptr };
        if (IsInlineMethod(R, Fn.Name)) continue;
        if ((Fn.Body = BodyOf(R, Fn.Name, Fn.Def))) Methods.push_back(Fn);
    }
    /* The editor compiles every Blueprint-implementable function of an implemented interface, a stub
       where the Blueprint has none (KismetCompiler.cpp MergeUbergraphPagesIn, ConformImplementedInterfaces);
       without it a call finds the interface's own native UFunction. A native interface has no bodies,
       so its stubs are empty. BlueprintEvent is the flag, not the editor's event node: a function that
       returns a value has it too (Targetable::GetIsTargetable) and the editor implements it as a function
       graph. Only a native-only function lacks it, which UHT allows just under
       CannotImplementInterfaceInBlueprint (HeaderParser.cpp:7538). */
    for (const std::string& Listed : R.Interfaces)
    for (const FRecord* Link : InterfaceChain(Find(Listed)))        // the interfaces it extends are implemented too
    {
        const FRecord& IR = *Link;
        const std::string& I = IR.CppName;
        /* A mod's own interface needs none of the Events.json conformance check below: GenerateInterface
           emits every one of its functions as a BlueprintEvent, so all of them are implementable by
           construction. What it does share is the empty stub for one this class leaves out. */
        if (IR.bIsInterface)
        {
            for (const auto& M : IR.Methods)
            {
                if (std::any_of(Methods.begin(), Methods.end(),
                                [&](const FMethod& F) { return F.Name == M.first; })) continue;
                FMethod Fn{ M.first, M.second, M.second, nullptr };
                Fn.Body = BodyOf(IR, M.first, Fn.Def);
                Methods.push_back(Fn);
            }
            continue;
        }
        const std::string Prefix = IR.UePackage.substr(IR.UePackage.rfind('/') + 1) + "." + IR.UeName + ".";
        for (const auto& M : IR.Methods)
            if (M.first != "StaticClass" && !EventFlags.count(Prefix + UeNameOf(&IR, M.first)))     // StaticClass: UE_CLASS's
            { *Err = I + "::" + M.first + " is native only (not a BlueprintNativeEvent or BlueprintImplementableEvent), "
                     "so a Blueprint cannot implement " + I; return false; }
        for (auto It = EventFlags.lower_bound(Prefix); It != EventFlags.end() && It->first.compare(0, Prefix.size(), Prefix) == 0; ++It)
        {
            const std::string Name_ = CppNameOf(IR, It->first.substr(Prefix.size()));     // the table is in engine names
            if (std::any_of(Methods.begin(), Methods.end(), [&](const FMethod& F) { return F.Name == Name_; })) continue;
            auto Decl = IR.Methods.find(Name_);
            if (Decl == IR.Methods.end())
            { *Err = I + "::" + Name_ + " takes a type AssetGen cannot write yet, so " + R.CppName + " cannot implement " + I; return false; }
            FMethod Fn{ Name_, Decl->second, Decl->second, nullptr };
            Fn.Body = BodyOf(IR, Name_, Fn.Def);
            Methods.push_back(Fn);
        }
    }

    /* A method that makes a latent call becomes a segment of ExecuteUbergraph_<Class> and keeps its name as a stub
       event that jumps in, as the editor compiles an event graph. Any class can: every BPGC object gets a persistent
       frame (FObjectInitializer::PostConstructInit), and UWorld::Tick resumes every object's actions
       (ProcessLatentActions(nullptr), LevelTick.cpp:1539), not only an actor's. What the call needs is a world, and
       these find their own; any other object has one only through its Outer (UObject::GetWorld, Obj.cpp:846). The
       editor hides Delay there for that reason alone (EdGraphSchema_K2.cpp:846, ImplementsGetWorld). */
    const bool bHasOwnWorld = std::any_of(Ancestry.begin(), Ancestry.end(), [](const std::string& A) {
        return A == "Actor" || A == "ActorComponent" || A == "UserWidget" || A == "GameInstance" || A == "Subsystem"; });
    struct FSegment
    {
        std::string Name;
        FIndex Super;
        std::vector<FPropertyDef> Parms, Locals;
        std::vector<FStmtIR> Stmts;
        uint32 Flags = 0;
        std::map<std::string, std::string> Rename;
        std::vector<FCompletion> Completions;
    };
    std::vector<FSegment> Segments;
    GeneratedEvents.clear();

    for (const FMethod& Fn : Methods)
    {
        const Json& Decl = *Fn.Decl;
        const Json& M = *Fn.Def;
        std::vector<FPropertyDef> Params;
        if (!LowerParams(M, Fn.Name, Params)) return false;
        CurrentWco.clear();
        for (const std::string& PName : ParmNames(M))
            if (IsStaticDecl(Decl) && CurrentWco.empty() && IsWcoName(PName)) CurrentWco = PName;

        /* UE finds the return property by the exact name "ReturnValue". */
        const std::string FnQual = M["type"].value("qualType", std::string());
        std::string RetType;
        {
            const size_t LParen = FnQual.find('(');
            RetType = LParen == std::string::npos ? FnQual : FnQual.substr(0, LParen);
            while (!RetType.empty() && (RetType.back() == ' ' || RetType.back() == '\t'))
                RetType.pop_back();
        }
        if (!RetType.empty() && RetType.back() == '&')
        {
            *Err = R.CppName + "::" + Fn.Name + " returns " + RetType + ": a reference return is refused until what it "
                   "means to a C++ caller is specified; return a value or a pointer";
            return false;
        }
        if (!RetType.empty() && RetType != "void")
        {
            FPropertyDef PD;
            std::string PErr;
            if (!TypeToProperty(RetType, "ReturnValue", CPF_ReturnParm | CPF_OutParm,
                                "return type on " + Fn.Name, BP, &PD, &PErr))
            { *Err = PErr; return false; }
            /* Measured on BP_SentryGun_MoveMarker: a Blueprint ReturnValue is Parm | OutParm | ReturnParm only. */
            PD.PropertyFlags &= ~uint64(CPF_BlueprintVisible | CPF_BlueprintReadOnly);
            Params.push_back(PD);
        }

        std::vector<FStmtIR> Stmts;
        std::vector<FPropertyDef> Locals;
        ReadScratchAdded = false;
        ReadTmpCounter = 0;
        RefAddr.clear();
        RefAlias.clear();
        LocalRename.clear();
        ParmConst.clear();
        CurLocals = &Locals;
        LoopDepth = 0;
        SwitchDepth = 0;
        WriteBackDepth = 0;
        GotoLabels.clear();
        bBodyHasGoto = Fn.Body && HasGoto(*Fn.Body);
        KeepLoaded.clear();
        CurFnName = Fn.Name;
        bCurNet = (NetFlagsOf(Decl) | NetFlagsOf(M)) != 0;
        bCurNoOpt = IsNoOptDecl(Decl) || IsNoOptDecl(M);
        WarnedRefParms.clear();
        bMadeLatentCall = false;
        LatentCount = 0;
        Completions.clear();
        ActivatedActions.clear();
        LatentRefusal = IsStaticDecl(Decl) ? "a static function has no object whose ubergraph frame could keep its locals"
                      : (!RetType.empty() && RetType != "void") || HasOutParm(Params)
                          ? "a function that resumes later returns nothing and takes no reference parameters"
                      : "";
        if (Fn.Body && !LowerBody(*Fn.Body, BP, Stmts, Locals, Err))
        {
            *Err = R.CppName + "::" + Fn.Name + ": " + *Err;
            return false;
        }
        if (!HoistReadsInList(Stmts, BP, Locals, Err))
        {
            *Err = R.CppName + "::" + Fn.Name + ": " + *Err;
            return false;
        }
        if (!bCurNoOpt)
        {
            DropUnusedPure(Stmts);
            DropUnusedLocals(Stmts, Locals);
        }
        for (const auto& [Struct, Keep] : KeepLoaded)
        {
            auto Typed = [&](const FPropertyDef& P) { return P.Type == "StructProperty" && P.Extra.V == Struct; };
            if (std::none_of(Params.begin(), Params.end(), Typed) && std::none_of(Locals.begin(), Locals.end(), Typed))
                Locals.push_back(Keep);
        }
        /* Locals follow ReturnValue in ChildProperties; the engine tells them apart by CPF_Parm. */
        for (const FPropertyDef& L : Locals) Params.push_back(L);

        const bool bEndsWithReturn = !Stmts.empty() && Stmts.back().K == FStmtIR::Return;
        const bool bScratchNeeded = ReadScratchAdded;
        const FIndex DerefStruct = bScratchNeeded
            ? BP.ScriptStruct(ModPackage + "/FDeref", "FDeref")
            : FIndex{};

        /* An override or interface implementation takes these of its parent's flags (KismetCompiler.cpp
           PrecompileFunction and the event stubs); measured on BP_SentryGun_MoveMarker. A static left
           FUNC_Event would be treated by the loader as an overridable entry point. */
        uint32 Inherited = 0;
        const FIndex Super = FindEvent(BP, R.CppName, Fn.Name, &Inherited);
        uint32 Flags = Inherited ? Inherited & kOverrideInherits
                     : IsStaticDecl(Decl) ? uint32(FUNC_Static | FUNC_BlueprintCallable | FUNC_Public | FUNC_Final)
                     : FFunctionDef().FunctionFlags;
        /* All 7229 BlueprintPure functions in the DRG dump are BlueprintCallable too. */
        /* Its own access specifier, where no parent decides. The editor refuses a call node it forbids; the VM checks
           nothing, and clang has already refused what C++ forbids. */
        if (auto A = R.MethodAccess.find(Fn.Name); !Inherited && A != R.MethodAccess.end())
            Flags = (Flags & ~uint32(FUNC_Public | FUNC_Protected | FUNC_Private)) | A->second;
        if (IsPureDecl(M)) Flags |= FUNC_BlueprintPure | FUNC_BlueprintCallable;
        const std::string DeclType = TypeOf(Decl);
        if (DeclType.size() > 6 && DeclType.compare(DeclType.size() - 6, 6, " const") == 0) Flags |= FUNC_Const;
        /* ProcessEvent only lists a script function's out-parms for EX_LocalOutVariable when this is set
           (ScriptCore.cpp), and native code, delegates and interfaces all call through ProcessEvent. */
        if (HasOutParm(Params)) Flags |= FUNC_HasOutParms;
        if (const uint32 Net = NetFlagsOf(Decl) | NetFlagsOf(M))
        {
            /* Measured on BP_LiftPod: Server_ButtonPressedAnim is Net | NetServer, Multi_ButtonPressedAnim
               Net | NetMulticast; reliable adds NetReliable. A net function returns nothing (a reference parameter arrives as a copy),
               and an override keeps its parent's net flags (UClass::SetUpRuntimeReplicationData checks). */
            if ((Net & (FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast)) == 0)
            { *Err = R.CppName + "::" + Fn.Name + ": UE_RELIABLE needs UE_SERVER, UE_CLIENT or UE_MULTICAST"; return false; }
            if (Super.V != 0) { *Err = R.CppName + "::" + Fn.Name + ": an override takes its parent's replication; drop the RPC marker"; return false; }
            if (!RetType.empty() && RetType != "void") { *Err = R.CppName + "::" + Fn.Name + ": an RPC returns void"; return false; }
            if (std::any_of(Params.begin(), Params.end(), [](const FPropertyDef& P) { return HoldsMapOrSet(P); }))
            { *Err = R.CppName + "::" + Fn.Name + ": an RPC parameter cannot be a TMap or TSet, which do not replicate"; return false; }
            Flags |= Net;
            bReplicatesAnything = true;
        }
        if (const uint32 Access = AccessFlagsOf(Decl) | AccessFlagsOf(M))
        {
            /* Where the call is skipped rather than refused: UObject::CallFunction / ProcessEvent check these. */
            Flags |= Access;
        }

        if (bMadeLatentCall)
        {
            if (!bHasOwnWorld)
                printf("  warning: %s::%s waits, and an object of this class finds its world only through its Outer: make "
                       "it with an actor or component as Outer, or the call does nothing and the function never resumes\n",
                       R.CppName.c_str(), Fn.Name.c_str());
            if (bScratchNeeded)
            { *Err = R.CppName + "::" + Fn.Name + ": TODO: a pointer read in a function that makes a latent call"; return false; }
            FSegment Seg{ Fn.Name, Super, {}, Locals, Stmts, Flags, {}, Completions };
            Seg.Parms.assign(Params.begin(), Params.end() - Locals.size());
            Segments.push_back(std::move(Seg));
            continue;
        }

        BP.AddFunction(UeNameOf(&R, Fn.Name), Super, Params,
                       [Stmts, bEndsWithReturn, bScratchNeeded, DerefStruct](FScript& S, FIndex SelfExp) {
            if (bScratchNeeded)
            {
                /* Prime __DerefScratch__.Num = 1 so the ArrayGetByRef bounds check (0 <= idx < Num)
                   passes on the fake TArray header. Every hoisted read only overwrites .Data. */
                S.Let(EX_Let, "Num", DerefStruct,
                    [DerefStruct, SelfExp](FScript& V) {
                        V.StructMember("Num", DerefStruct,
                            [SelfExp](FScript& C) { C.LocalVariable("__DerefScratch__", SelfExp); });
                    },
                    [](FScript& V) { V.IntOne(); });
            }
            EmitStmts(Stmts, S, SelfExp);
            if (!bEndsWithReturn) S.Return();
            S.EndOfScript();
        }, Flags);
    }

    if (!Segments.empty())
    {
        const std::string UberName = "ExecuteUbergraph_" + R.CppName;
        for (const FRecord* A = &R; A; A = A->Base.empty() ? nullptr : Find(A->Base))
            if (A->Methods.count(UberName)) { *Err = A->CppName + " declares " + UberName + ", the ubergraph's own name"; return false; }

        /* Every segment's parms and locals share the ubergraph frame: <Fn>_<Name>, numbered past a taken name. The
           mod's own properties along the class chain are taken too, so a frame local never reads like one. */
        std::set<std::string> Taken = { "EntryPoint", "UberGraphFrame" };
        for (const FRecord* A = &R; A; A = A->Base.empty() ? nullptr : Find(A->Base))
            for (const Json* F : A->Fields) Taken.insert(Name(*F));
        auto Renamed = [](FPropertyDef P, const std::string& To) {
            /* A container's inner properties carry the container's name. */
            if (P.Inner) { P.Inner = std::make_shared<FPropertyDef>(*P.Inner); if (P.Inner->Name == P.Name) P.Inner->Name = To; }
            if (P.Value) { P.Value = std::make_shared<FPropertyDef>(*P.Value); if (P.Value->Name == P.Name) P.Value->Name = To; }
            P.Name = To;
            P.PropertyFlags &= ~uint64(CPF_Parm | CPF_OutParm | CPF_ReferenceParm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
            return P;
        };
        /* Measured on ExecuteUbergraph_BP_LiftPod: EntryPoint is Parm | BlueprintVisible | BlueprintReadOnly. */
        FPropertyDef Entry = IntParam("EntryPoint");
        Entry.PropertyFlags = CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly;
        std::vector<FPropertyDef> Frame{ Entry };
        for (FSegment& Seg : Segments)
        {
            for (std::vector<FPropertyDef>* List : { &Seg.Parms, &Seg.Locals })
                for (const FPropertyDef& P : *List)
                {
                    std::string To = Seg.Name + "_" + P.Name;
                    for (int32 N = 2; Taken.count(To); ++N) To = Seg.Name + "_" + P.Name + "_" + std::to_string(N);
                    Taken.insert(To);
                    Seg.Rename[P.Name] = To;
                    Frame.push_back(Renamed(P, To));
                }
            FLocalRenamer{ Seg.Rename }.List(Seg.Stmts);
        }

        /* Measured: a computed jump on EntryPoint, each event's code at the offset its stub passes. A segment ends in
           a return where the editor pops back to its one pushed return. */
        auto Entries = std::make_shared<std::vector<int32>>(Segments.size());
        const FIndex Uber = BP.AddFunction(UberName, Null(), Frame, [Segments, Entries](FScript& S, FIndex SelfExp) {
            S.ComputedJump([SelfExp](FScript& C) { C.LocalVariable("EntryPoint", SelfExp); });
            for (size_t I = 0; I < Segments.size(); ++I)
            {
                (*Entries)[I] = S.MemorySize();
                EmitStmts(Segments[I].Stmts, S, SelfExp);
                S.Return();
            }
            S.EndOfScript();
        }, FUNC_UbergraphFunction | FUNC_HasDefaults | FUNC_Final);

        /* The stubs follow the ubergraph, so its body (and Entries) is written first. Measured on BP_LiftPod's
           OnCompleted_*: each parm copied into the frame, then EX_LocalFinalFunction ExecuteUbergraph(offset). */
        for (size_t I = 0; I < Segments.size(); ++I)
        {
            std::vector<std::pair<std::string, std::string>> Copies;
            for (const FPropertyDef& P : Segments[I].Parms) Copies.emplace_back(P.Name, Segments[I].Rename.at(P.Name));
            BP.AddFunction(UeNameOf(&R, Segments[I].Name), Segments[I].Super, Segments[I].Parms,
                           [Copies, Uber, Entries, I](FScript& S, FIndex SelfExp) {
                for (const auto& [From, To] : Copies)
                    S.LetValueOnPersistentFrame(To, Uber, [&, From](FScript& V) { V.LocalVariable(From, SelfExp); });
                S.LocalFinalFunction(Uber);
                S.IntConst((*Entries)[I]);
                S.EndFunctionParms();
                S.Return();
                S.EndOfScript();
            }, Segments[I].Flags);
        }

        /* Measured on BP_LiftPod's OnCompleted_*: BlueprintCallable | BlueprintEvent, storing its parameter into the
           frame, then calling the ubergraph at the code after the await. A latent call's completion fires before
           the latent action resumes, so that one only stores. */
        for (const FSegment& Seg : Segments)
            for (const FCompletion& C : Seg.Completions)
            {
                const std::string To = C.Local.empty() ? std::string() : Seg.Rename.at(C.Local);
                const std::string From = C.Parms.empty() ? std::string() : C.Parms[0].Name;
                const std::shared_ptr<int32> Resume = C.Resume;
                BP.AddFunction(C.Event, Null(), C.Parms, [To, From, Resume, Uber](FScript& S, FIndex SelfExp) {
                    if (!To.empty())
                        S.LetValueOnPersistentFrame(To, Uber, [&](FScript& V) { V.LocalVariable(From, SelfExp); });
                    if (Resume)
                    {
                        S.LocalFinalFunction(Uber);
                        S.IntConst(*Resume);
                        S.EndFunctionParms();
                    }
                    S.Return();
                    S.EndOfScript();
                }, FUNC_BlueprintCallable | FUNC_BlueprintEvent);
            }

        /* Measured on BP_LiftPod: a transient FPointerToUberGraphFrame the class links by this name. */
        FPropertyDef FramePtr;
        FramePtr.Type = "StructProperty";
        FramePtr.Name = "UberGraphFrame";
        FramePtr.ElementSize = 16;
        FramePtr.PropertyFlags = CPF_Transient | CPF_DuplicateTransient;
        FramePtr.Extra = BP.ScriptStruct("/Script/Engine", "PointerToUberGraphFrame");
        FramePtr.StructName = "PointerToUberGraphFrame";
        BP.AddVariable(FramePtr);
        BP.SetUberGraphFunction(Uber);
    }

    if (bReplicatesAnything) BP.SetReplicates(true);
    BP.Finish();
    if (!P.Save(OutDir + "/" + R.CppName, Err)) return false;
    if (!ApiDir.empty() && !BP.WriteApi(ApiDir, Err))
    {
        /* A class with nothing callable is not a build failure; the mod simply has no API surface. */
        printf("  %-14s -> no API asset: %s\n", R.CppName.c_str(), Err->c_str());
        Err->clear();
    }
    RegistryRows.push_back({ PackageName, R.CppName + "_C", "BlueprintGeneratedClass" });
    printf("  %-14s -> %s.uasset  (%s %s)\n", R.CppName.c_str(), R.CppName.c_str(),
           B->IsNative() ? "extends" : "extends BP", R.Base.c_str());
    return true;
}

bool FCompiler::LoadTables(const std::string& IncludeDir, std::string* Err)
{
    auto Load = [&](const char* File, Json* Out) {
        *Out = Json::parse(ReadText(IncludeDir + "/" + File), nullptr, false);
        if (Out->is_discarded()) { *Err = std::string("missing or invalid ") + IncludeDir + "/" + File + " (run genueapi.py)"; return false; }
        return true;
    };
    Json ConvDoc, OpsDoc, TypesDoc, EventsDoc;
    if (!Load("Conv.json", &ConvDoc) || !Load("Ops.json", &OpsDoc) || !Load("Types.json", &TypesDoc)
        || !Load("Events.json", &EventsDoc)) return false;
    for (auto It = EventsDoc.begin(); It != EventsDoc.end(); ++It) EventFlags[It.key()] = It->get<uint32>();

    for (const Json& Row : ConvDoc)
        Convs.push_back({ Row.value("from", std::string()), Row.value("to", std::string()),
                          Row.value("package", std::string()), Row.value("class", std::string()),
                          Row.value("fn", std::string()), ExtraArgs(Row) });
    for (const Json& Row : OpsDoc)
        Ops.push_back({ Row.value("op", std::string()), Row.value("lhs", std::string()), Row.value("rhs", std::string()),
                        Row.value("ret", std::string()), Row.value("package", std::string()),
                        Row.value("class", std::string()), Row.value("fn", std::string()), ExtraArgs(Row) });
    for (auto It = TypesDoc["enums"].begin(); It != TypesDoc["enums"].end(); ++It)
        Enums[It.key()] = { It->value("package", std::string()), It->value("name", std::string()),
                            It->value("underlying", std::string()), It->value("first", std::string()) };
    for (auto It = TypesDoc["structs"].begin(); It != TypesDoc["structs"].end(); ++It)
    {
        FStructInfo S;
        S.Package = It->value("package", std::string());
        S.UeName = It->value("name", std::string());
        S.Size = It->value("size", 0);
        S.Align = It->value("align", 1);
        S.bComplete = It->value("complete", false);
        for (const Json& F : (*It)["fields"]) S.Fields.emplace_back(F[0].get<std::string>(), F[1].get<std::string>());
        Structs[It.key()] = S;
    }
    return true;
}

bool FCompiler::Run(const std::string& SourcePath, const std::string& IncludeDir,
                    const std::string& OutDir, const std::string& InApiDir, std::string* Err)
{
    ApiDir = InApiDir;
    SourceDir = std::filesystem::path(SourcePath).parent_path().string();
    const std::string AstPath = OutDir + "/ast.json";
    /* Both the UeApi dir and its parent are include paths, so "FSD.h" and "UeApi/FSD.h" both resolve. */
    const std::string Parent = std::filesystem::path(IncludeDir).parent_path().string();
    const std::string Cmd = "clang++ -std=c++20 -fsyntax-only -Xclang -ast-dump=json"
                            " \"" + SourcePath + "\" -I\"" + IncludeDir + "\" -I\"" + Parent
                          + "\" > \"" + AstPath + "\"";
    if (system(("\"" + Cmd + "\"").c_str()) != 0)
    {
        *Err = "clang rejected " + SourcePath + " (diagnostics above)";
        return false;
    }

    const std::string Text = ReadText(AstPath);
    if (Text.empty()) { *Err = "clang produced no AST at " + AstPath; return false; }
    Doc = Json::parse(Text, nullptr, false);
    if (Doc.is_discarded()) { *Err = "could not parse clang's AST dump"; return false; }

    if (!LoadTables(IncludeDir, Err)) return false;
    if (!Collect(Err)) return false;
    /* Inline free functions anywhere in the translation unit, a template's instantiations included: a call names
       the instantiation's own FunctionDecl. Class bodies are skipped; their methods are the records'. */
    std::function<void(const Json&)> IndexConsts = [&](const Json& N) {
        const std::string K = Kind(N);
        if (K == "VarDecl" && N.contains("init") && N.contains("id")
            && (N.value("constexpr", false) || TypeOf(N).compare(0, 6, "const ") == 0))
            ConstVars[N.value("id", std::string())] = &N;
        if (K == "TranslationUnitDecl" || K == "NamespaceDecl" || K == "CXXRecordDecl" || K == "LinkageSpecDecl")
            ForEach(N, IndexConsts);
    };
    IndexConsts(Doc);
    std::function<void(Json&)> IndexInlines = [&](Json& N) {
        if (!N.is_object()) return;
        const std::string K = Kind(N);
        if (K == "CXXRecordDecl") return;
        if (K == "FunctionDecl" && N.value("inline", false))
        {
            bool bBody = false;
            ForEach(N, [&](const Json& C) { if (Kind(C) == "CompoundStmt") bBody = true; });
            if (bBody)
            {
                NormalizePointers(N);
                FreeInlines[N.value("id", std::string())] = &N;
            }
            return;
        }
        auto It = N.find("inner");
        if (It != N.end()) for (Json& C : *It) IndexInlines(C);
    };
    IndexInlines(Doc);
    /* Before anything reads a type: a raw pointer is an int64 from here on (see IsRawPointer). */
    for (const auto& Entry : Records)
    {
        if (!Entry.second.IsGenerated() && !Entry.second.IsModStruct()) continue;
        for (const Json* F : Entry.second.Fields) NormalizePointers(const_cast<Json&>(*F));
        for (const auto& M : Entry.second.Methods) NormalizePointers(const_cast<Json&>(*M.second));
        for (const auto& M : Entry.second.MethodDefs) NormalizePointers(const_cast<Json&>(*M.second));
    }

    int32 Generated = 0;
    for (const auto& Entry : Records)
    {
        const FRecord& R = Entry.second;
        if (!R.IsGenerated()) continue;
        if (!(R.bIsStruct       ? GenerateStruct(R, OutDir, Err)
              : R.bIsInterface  ? GenerateInterface(R, OutDir, Err)
                                : Generate(R, OutDir, Err)))
        {
            /* Remove every generated asset; keep the AST for inspection. */
            for (const auto& Other : Records)
                if (Other.second.IsGenerated())
                    for (const char* Ext : { ".uasset", ".uexp" })
                        remove((OutDir + "/" + Other.first + Ext).c_str());
            return false;
        }
        ++Generated;
    }
    for (const auto& Entry : ModEnums)
    {
        if (!GenerateEnum(Entry.first, OutDir, Err)) return false;
        ++Generated;
    }
    for (const Json* Var : AssetDecls)
        if (!GenerateAsset(*Var, OutDir, Err)) return false;
    if (Generated == 0 && RegistryRows.empty()) { *Err = "the source declares no UE_STRUCT, UE_ENUM or class deriving from a UE class"; return false; }
    if (!GenerateNestedWrappers(OutDir, Err)) return false;

    /* A cooked package carries no registry data; without the bake the classes are invisible to it. */
    if (!SaveAssetRegistry(RegistryRows, OutDir + "/AssetRegistry.bin", Err)) return false;
    printf("  %-14s -> AssetRegistry.bin  (%d asset%s)\n", "registry",
           int32(RegistryRows.size()), RegistryRows.size() == 1 ? "" : "s");

    remove(AstPath.c_str());        // kept only on failure
    return true;
}
}   // namespace

bool CompileToAssets(const std::string& SourcePath, const std::string& IncludeDir,
                     const std::string& OutDir, const std::string& ApiDir, std::string* Err)
{
    FCompiler C;
    return C.Run(SourcePath, IncludeDir, OutDir, ApiDir, Err);
}

}   // namespace Uasset
