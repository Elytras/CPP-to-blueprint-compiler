#include "Cpp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <map>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
static int ProcessId() { return _getpid(); }
#else
#include <unistd.h>
static int ProcessId() { return getpid(); }
#endif

#include <nlohmann/json.hpp>

#include "Blueprint.h"
#include "Cooked.h"
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

/* FDeref and FDerefTextView are the compiler's own read-hoist plumbing, and FMapSlot_* / FMapSlots_* its view of a
   TMap walked in place: their fields are referenced by fixed names from hardcoded emit sites (ViewFieldOf, the
   __DerefScratch__ prologue, LowerMapWalk), never through user field access, so they must keep their plain C++ names
   on both the cooked layout and every reference. */
bool IsInternalViewStruct(const std::string& CppName)
{
    return CppName == "FDeref" || CppName == "FDerefTextView" || CppName.compare(0, 9, "FMapSlot_") == 0
        || CppName.compare(0, 10, "FMapSlots_") == 0;
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
Json RefToLocal(const std::string& Name, const std::string& Type);

std::string TypeOf(const Json& N)
{
    auto It = N.find("type");
    return It == N.end() ? std::string() : It->value("qualType", std::string());
}

/* A namespace is a folder: `A::B::X` is <Package>/A/B/X. One that starts at Game is a /Game path of its own, `Game::A::X`
   being /Game/A/X, which is where genueapi puts a game Blueprint's class too. */
std::string PathIn(const std::string& Package, const std::string& Qualified)
{
    std::string P = Qualified.compare(0, 6, "Game::") == 0 ? "/" + Qualified : Package + "/" + Qualified;
    for (size_t At; (At = P.find("::")) != std::string::npos;) P.replace(At, 2, "/");
    return P;
}

/* `A::B::X` is X. */
std::string LeafOf(const std::string& Qualified) { return Qualified.substr(Qualified.rfind(':') + 1); }

/* A CharacterLiteral's value is its code unit; a plain char is signed (clang for MSVC), so '\xff' is -1. */
int64 CharValue(const Json& N)
{
    const int64 V = N.value("value", int64(0));
    const std::string T = StripTypeKeywords(TypeOf(N));
    return T == "char" || T == "signed char" ? int64(int8(V)) : V;
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
/* A UE_ASSET_AT path: "/Game/Dir/Pkg.Object" names an object other than the package's namesake, "/Game/Dir/Pkg" means
   Pkg.Pkg. Path becomes the package; the object's name is returned. */
std::string SplitAssetPath(std::string& Path)
{
    std::string Object = Path.substr(Path.rfind('/') + 1);
    if (const size_t Dot = Object.find('.'); Dot != std::string::npos)
    {
        Path.resize(Path.size() - (Object.size() - Dot));
        Object = Object.substr(Dot + 1);
    }
    return Object;
}

bool IsAssignOperatorCall(const Json& N)
{
    const Json* Callee = Strip(First(N));
    if (!Callee || Kind(*Callee) != "DeclRefExpr") return false;
    auto Ref = Callee->find("referencedDecl");
    return Ref != Callee->end() && Ref->value("name", std::string()) == "operator=";
}

bool IsIndexOperatorCall(const Json& N)
{
    const Json* Callee = Strip(First(N));
    if (!Callee || Kind(*Callee) != "DeclRefExpr") return false;
    auto Ref = Callee->find("referencedDecl");
    return Ref != Callee->end() && Ref->value("name", std::string()) == "operator[]";
}

/* An assignment statement's two sides. Assigning a struct is an operator call, not a BinaryOperator: its inner is the
   callee then the two operands, so both shapes are read the same way one index on. */
bool AssignmentSides(const Json& S, const Json*& Lhs, const Json*& Rhs)
{
    const Json* Assign = Strip(&S);
    const std::string AK = Assign ? Kind(*Assign) : std::string();
    const bool bOpCall = AK == "CXXOperatorCallExpr";
    const size_t Base = bOpCall ? 1 : 0;
    Lhs = Assign ? Strip(Nth(*Assign, Base)) : nullptr;
    Rhs = Assign ? Nth(*Assign, Base + 1) : nullptr;
    const bool bAssign = bOpCall ? IsAssignOperatorCall(*Assign)
                                 : AK == "BinaryOperator" && Assign->value("opcode", std::string()) == "=";
    return Assign && bAssign && Lhs && Rhs;
}

/* One UE_DEFAULTS statement, `Field = value;` or `Component->Field = value;`: the field's MemberExpr, the value, and
   the component's MemberExpr it reaches through (null for none). False for any other statement. */
bool DefaultAssignment(const Json& S, const Json*& Lhs, const Json*& Rhs, const Json*& Through)
{
    if (!AssignmentSides(S, Lhs, Rhs) || Kind(*Lhs) != "MemberExpr") return false;
    const Json* Owner = Strip(First(*Lhs));
    Through = Owner && Kind(*Owner) == "MemberExpr" ? Owner : nullptr;
    return true;
}

/* A patch's UE_DEFAULTS statement, which may also assign part of a member's value: DefaultAssignment's `Field` or
   `Component->Field`, then any `.Member` and `[i]` into it (`PrimaryActorTick.bCanEverTick = v`, `Spans[1].Max = v`).
   Root is the field's MemberExpr, Steps each `.Member`'s MemberExpr or `[i]`'s operator call, from the root out. */
bool DefaultPath(const Json& S, const Json*& Root, const Json*& Rhs, const Json*& Through, std::vector<const Json*>& Steps)
{
    const Json* E = nullptr;
    if (!AssignmentSides(S, E, Rhs)) return false;
    Steps.clear();
    for (;;)
    {
        const std::string K = Kind(*E);
        const Json* Next = K == "MemberExpr" && !E->value("isArrow", false) ? Strip(First(*E))
                         : K == "CXXOperatorCallExpr" && IsIndexOperatorCall(*E) ? Strip(Nth(*E, 1)) : nullptr;
        if (!Next) break;
        Steps.insert(Steps.begin(), E);
        E = Next;
    }
    if (Kind(*E) != "MemberExpr") return false;
    Root = E;
    const Json* Owner = Strip(First(*E));
    Through = Owner && Kind(*Owner) == "MemberExpr" ? Owner : nullptr;
    return true;
}

/* A UE_ASSET_EDITS statement: `Asset.Member = value`, then any `.Member` and `[i]` into the member's value. Asset is
   the DeclRefExpr naming the asset's variable, Root the member's MemberExpr, Steps as DefaultPath's. */
bool AssetPath(const Json& S, const Json*& Asset, const Json*& Root, const Json*& Rhs, std::vector<const Json*>& Steps)
{
    const Json* E = nullptr;
    if (!AssignmentSides(S, E, Rhs)) return false;
    Steps.clear();
    for (;;)
    {
        const std::string K = Kind(*E);
        const Json* Next = K == "MemberExpr" ? Strip(First(*E))
                         : K == "CXXOperatorCallExpr" && IsIndexOperatorCall(*E) ? Strip(Nth(*E, 1)) : nullptr;
        if (!Next) return false;
        if (K == "MemberExpr" && Kind(*Next) == "DeclRefExpr")
        {
            Asset = Next;
            Root = E;
            return true;
        }
        Steps.insert(Steps.begin(), E);
        E = Next;
    }
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
/* A method that overrides nothing. Measured on the DRG dump: a Blueprint-authored function is
   (Public, BlueprintCallable, BlueprintEvent) - 4624 of them, CD2_Module_C's among them. Without BlueprintCallable
   no Blueprint can place a call node, and the API stub, which lists what a Blueprint can call, left every plain
   method out. FUNC_Event stays: it is what every mod was cooked and run with so far. */
static const uint32 kPlainMethodFlags = FFunctionDef().FunctionFlags | FUNC_BlueprintCallable;

uint32 AccessFlagsOf(const Json& Decl)
{
    uint32 Flags = 0;
    ForEach(Decl, [&](const Json& C) {
        const std::string K = Kind(C);
        if (K == "NoStackProtectorAttr") Flags |= FUNC_BlueprintAuthorityOnly;
        else if (K == "NoInstrumentFunctionAttr") Flags |= FUNC_BlueprintCosmetic;
    });
    return Flags;
}

/* An attribute clang put on the declaration: `final` (FinalAttr), `noinline` in any spelling (NoInlineAttr). */
bool HasAttr(const Json& Decl, const char* AttrKind)
{
    bool bHas = false;
    ForEach(Decl, [&](const Json& C) { bHas = bHas || Kind(C) == AttrKind; });
    return bHas;
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
    std::map<std::string, const Json*> Inlines;     // decl id (in-class or out-of-line) -> an inline method's
                                                    // definition: by id, as Methods keeps one overload per name
    std::vector<const Json*> AllMethods;            // every in-class method decl, overloads included, in order
    std::vector<const Json*> Fields;
    std::vector<const Json*> Ctors;                 // CXXConstructorDecls: their parameter names place a value's arguments
    std::vector<std::string> Interfaces;            // every base after the first
    std::map<std::string, std::string> Replicated;  // UE_REPLICATED*: variable -> "Notify:Condition"
    std::set<std::string> Components;               // UE_COMPONENT: variables that are also SCS nodes
    /* genueapi's `<X>__UeName`: the engine's name of a member or function Dumper-7 had to respell (`Name_0` is
       `Name`, `Audio_Flying` is `Audio Flying`). C++ keeps its spelling; everything cooked goes through UeNameOf. */
    std::map<std::string, std::string> UeNames;
    std::map<std::string, std::string> Forwards;    // method -> "ULibrary::Static" that does it, the object first (UObject::GetOuter)
    /* The C++ access specifier, which is the editor's own: private is the class alone, protected its subclasses too.
       A function carries it as a flag. A variable has private only, and only as editor metadata, so a private
       field is simply left out of the API stub; a protected one stays visible, a subclass having a right to it. */
    std::map<std::string, uint32> MethodAccess;     // method -> FUNC_Public / FUNC_Protected / FUNC_Private
    std::map<std::string, std::string> Categories;  // method or field -> the UE_CATEGORY section it is declared in
    std::set<std::string> PrivateFields;
    std::map<std::string, std::string> ScsNodes;    // `<X>__UeScsNode`: a game Blueprint's component -> its node's guid, 32 hex
    std::map<std::string, std::string> Subobjects;  // `<X>__UeSubobject`: a native component -> "<name> <class path>" on this CDO
    std::map<std::string, std::string> TypeAliases; // `using Leaf = Game::...::Leaf;` in the class body
    std::set<std::string> FinalMethods;             // `virtual T F() final`: no subclass has an F of its own
    const Json* Defaults = nullptr;                 // UE_DEFAULTS: the static-init block, never lowered
    bool bFinal = false;        // `class X final`: X has no subclass
    bool bIsLocal = false;      // UePackage == ModPackage/CppName: cooked here, published at its /Game path
    bool bIsStruct = false;     // UE_STRUCT: cooked as a UserDefinedStruct asset
    bool bIsInterface = false;  // UE_INTERFACE: cooked as a BPGC whose super is UInterface
    bool bIsPatch = false;      // UE_PATCH: not a class of its own; its UE_DEFAULTS edit its parent's, in the parent's package

    bool IsNative() const { return !UePackage.empty() && !bIsLocal; }
    bool IsGenerated() const { return !IsNative() && !bIsPatch && (bIsStruct || bIsInterface || !Base.empty()); }
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
    std::vector<size_t> Refs;       // the const reference parameters, which the native reads by address: see HoistCallArgs
};

/* One row of UeApi/Ops.json: the Kismet function behind `Lhs <op> Rhs`. */
struct FOpInfo
{
    std::string Op, Lhs, Rhs, Ret;
    std::string Package, Class, Fn;
    std::vector<Json> Extra;
    std::vector<size_t> Refs;
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

std::vector<size_t> RefArgs(const Json& Row)
{
    std::vector<size_t> Out;
    auto It = Row.find("refs");
    if (It != Row.end()) for (const Json& E : *It) Out.push_back(E.get<size_t>());
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
    bool bReceiverIsArg = false;        // a forwarded UObject helper (Obj->GetOuter()): Obj is already the first argument, no EX_Context
    bool bOnArg0 = false;               // Args[0] is the container or dispatcher worked on: the object holding it goes first
    FIndex Context;                     // CDO a static call runs against; null = self
    bool bPure = false;                 // a function of its arguments (UE_PURE, a Kismet operator or conversion): see DropUnusedPure
    uint64 WrittenArgs = ~uint64(0);    // bit I: argument I must stay its own variable, which the callee may write: see ContainerWrites
    std::vector<std::string> RefParms;  // per argument, the type of the reference parameter it binds, else "": see HoistCallArgs
    bool bRefsTakeConst = false;        // a native's P_GET_PROPERTY_REF: a constant there goes through the thunk's own buffer
    std::string VirtualName;            // a generated class's own instance method: EX_VirtualFunction resolves it by name at run time
    bool bLocal = false;                // EX_LocalVirtualFunction / EX_LocalFinalFunction: a script function that is no RPC
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
    if (!Call.VirtualName.empty() && Call.bLocal) S.LocalVirtualFunction(Call.VirtualName);
    else if (!Call.VirtualName.empty()) S.VirtualFunction(Call.VirtualName);
    else if (Call.bLocal) S.LocalFinalFunction(Call.Fn);
    else if (Call.bScript || Call.bInstance || Call.Context.V) S.FinalFunction(Call.Fn);    // a native static on its CDO
    else S.CallMath(Call.Fn);
}

/* RValue: the variable an assignment stores the call's value in, which a context call names so the VM clears it when
   the object is None (ProcessContextOpcode, ScriptCore.cpp 2950-2953). */
bool EmitCall(FScript& S, const FCallIR& Call, FIndex SelfExp, std::string* Err,
              const std::optional<FFieldRef>& RValue = std::nullopt);
bool EmitArg(FScript& S, const FArgIR& A, FIndex SelfExp, std::string* Err);

/* A value stored into Dest: a call on another object names Dest as its context's r-value, as the editor's does
   (KismetCompilerVMBackend.cpp 1241-1244), so a None object leaves Dest cleared, not holding its old value. */
void EmitValueInto(FScript& S, const FArgIR& Value, FIndex SelfExp, const std::optional<FFieldRef>& Dest)
{
    if (Value.K == FArgIR::Call && Value.Sub && Value.Sub->Intrinsic.empty() && (Value.Sub->Target || Value.Sub->Context.V))
        EmitCall(S, *Value.Sub, SelfExp, nullptr, Dest);
    else
        EmitArg(S, Value, SelfExp, nullptr);
}

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
    bool bConstCond = false;                        // While: Cond is a constant (PruneConstBranches), so no test is emitted
    bool bJumpOut = false;                          // If: Then is one break / continue, taken when Cond is FALSE: a
                                                    // single JumpIfNot straight to where it goes
    std::vector<FArgIR> CaseTests;                  // Switch: per case, true when the value does NOT match
    int32 LabelId = -1;                             // Label: the case it marks, -1 if no value can; Switch: the default's label, or -1;
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
namespace { bool CallsImpure(const FArgIR& A); }

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
                  [&](FScript& C) { C.InstanceVariable(A.S, A.Owner); }, FFieldRef{ A.S, A.Owner });
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
        if (A.Sub->Intrinsic == "__SwitchValue__")
        {
            /* EX_SwitchValue (`[S]` ScriptCore.cpp:2519): uint16 case count, uint32 MEMORY offset past it all, the index,
               then per case its value, the offset past its result and the result, then the default. Args are the
               index, the (value, result) pairs, the default. */
            const std::vector<FArgIR>& C = A.Sub->Args;
            const uint16 NumCases = uint16((C.size() - 2) / 2);
            S.Op(EX_SwitchValue);
            S.Raw(&NumCases, sizeof NumCases, sizeof NumCases);
            const int32 ToEnd = S.StorageSize();
            S.RawInt32(0);
            if (!EmitArg(S, C[0], SelfExp, Err)) return false;
            for (size_t I = 1; I + 1 < C.size(); I += 2)
            {
                if (!EmitArg(S, C[I], SelfExp, Err)) return false;
                const int32 ToNext = S.StorageSize();
                S.RawInt32(0);
                if (!EmitArg(S, C[I + 1], SelfExp, Err)) return false;
                S.PatchJumpTarget(ToNext, S.MemorySize());
            }
            if (!EmitArg(S, C.back(), SelfExp, Err)) return false;
            S.PatchJumpTarget(ToEnd, S.MemorySize());
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

bool EmitCall(FScript& S, const FCallIR& Call, FIndex SelfExp, std::string* Err, const std::optional<FFieldRef>& RValue)
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
    /* Make Array / Set / Map (LowerContainerLiteral): Args[0] is the variable filled, then the elements, a map's
       alternating key and value; a set or map says how many first. */
    if (Call.Intrinsic == "__SetArray__" || Call.Intrinsic == "__SetSet__" || Call.Intrinsic == "__SetMap__")
    {
        const bool bArray = Call.Intrinsic == "__SetArray__", bMap = Call.Intrinsic == "__SetMap__";
        S.Op(bArray ? EX_SetArray : bMap ? EX_SetMap : EX_SetSet);
        if (!EmitArg(S, Call.Args[0], SelfExp, Err)) return false;
        if (!bArray) S.RawInt32(int32(Call.Args.size() - 1) / (bMap ? 2 : 1));
        if (!EmitArgs(S, std::vector<FArgIR>(Call.Args.begin() + 1, Call.Args.end()), SelfExp, Err)) return false;
        S.Op(bArray ? EX_EndArray : bMap ? EX_EndMap : EX_EndSet);
        return true;
    }

    /* Any other intrinsic has a value and no UFunction (`__AddrOf__`, `__NameIndex__`, a pointer read). As a statement,
       an unused local's store or a discarded call, the value goes nowhere, and an EX_CallMath would call null. A call
       among its arguments still runs. */
    if (!Call.Intrinsic.empty() && !Call.Fn.V)
    {
        for (const FArgIR& A : Call.Args)
            if (A.K == FArgIR::Call && A.Sub && A.Sub->Intrinsic.empty() && !EmitCall(S, *A.Sub, SelfExp, Err)) return false;
        return true;
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
            }, RValue);
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
    FILE* F = fopen(Path.c_str(), "rb");
    if (!F) return Out;
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
             const std::string& OutDir, const std::optional<std::string>& InApiDir, std::string* Err);
    std::string GameDir;        // `--game`: the folder /Game is in, in the extracted game pak - what an edit reads

private:
    bool Collect(std::string* Err);
    bool Generate(const FRecord& R, const std::string& OutDir, std::string* Err);
    /* S38: UE_ASSET_EDIT and UE_PATCH, the game's own packages with only the named tags changed. */
    bool GenerateEdit(const std::string& Key, const Json& Var, std::string* Err);
    bool GeneratePatch(const FRecord& R, std::string* Err);
    bool PatchDefaults(const FRecord& Bp, const Json& Body, const std::string& Where, std::string* Err);
    /* One assignment of an edit: to a member (Path empty, Chain the member alone), or to part of its value - Chain
       then holds the member and the def each step of Path reaches, the last the value written. The last carries the
       value either way. */
    struct FEditDef { std::vector<FPropertyDef> Chain; std::vector<FValueStep> Path; };
    bool EditStep(const Json& Step, const FPropertyDef& Parent, const std::string& Where, FBlueprintClass& BP,
                  FValueStep& Out, FPropertyDef& Reached, std::string* Err);
    bool EditAssignment(const Json& Root, const FRecord& Declarer, const std::vector<const Json*>& Steps, const Json* Rhs,
                        const std::string& Where, FBlueprintClass& BP, FEditDef& Out, std::string* Err);
    bool GenerateAssetEdits(const Json& Block, std::string* Err);
    std::vector<const Json*> AssetEditBlocks;               // UE_ASSET_EDITS: each block's function
    bool ApplyEdit(const std::string& Package, const std::string& Object, std::vector<FEditDef> Edits,
                   const FPackage& From, const std::string& Where, std::string* Err);
    bool SaveEdits(const std::string& OutDir, std::string* Err);
    bool BracedMembers(const Json& List, const FRecord& Rec, const std::string& Where, FBlueprintClass& BP, bool bKeepZero,
                       std::vector<FPropertyDef>& Out, std::string* Err);
    struct FEdited { std::string Package, Ext; FCookedPackage P; std::vector<std::string> Objects; };
    std::map<std::string, FEdited> Edited;                  // lowercased package name -> the game's package, edited
    FEdited* LoadEdited(const std::string& Package, const std::string& Where, std::string* Err);
    bool TransplantFunctions(const FRecord& R, const FRecord& B, const FPackage& Scratch, FIndex ScratchClass,
                             std::string* Err);
    std::map<std::string, FIndex> PatchSupers;              // "Patch::Method" -> what it would override, in its scratch
    std::map<std::string, const Json*> EditTargets;         // UE_ASSET_EDIT: Ns + its number -> `&Asset`'s variable
    std::vector<std::pair<std::string, const Json*>> Edits; // ... -> the braced variable holding the edit
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
    bool NeedsResultLocal(const std::string& Type, int32 Depth = 0);
    bool StructLayout(const FRecord& R, int32* Size, int32* Align, std::string* Err);
    bool LowerBody(const Json& Body, FBlueprintClass& BP, std::vector<FStmtIR>& Out,
                   std::vector<FPropertyDef>& Locals, std::string* Err);

    /* A local this body declares, holds them all the uses of, and only ever reads: its constant initialiser can
       stand in for every read (ParmConst), so neither the property nor its store is compiled. */
    bool ReadOnlyLocal(const std::string& DeclId) const;
    bool LowerCall(const Json& CallExprNode, FBlueprintClass& BP, FCallIR& Out, std::string* Err);
    bool LowerRangeFor(const Json& ForNode, FBlueprintClass& BP, std::vector<FStmtIR>& Out,
                       std::vector<FPropertyDef>& Locals, std::string* Err);
    bool ChangesMapCount(const Json& Body, const std::string& MapType) const;
    bool LowerWithoutPrefix(const Json& Stmt, FBlueprintClass& BP, std::vector<FStmtIR>& Out,
                            std::vector<FPropertyDef>& Locals, std::string* Err);

    /* `inline` functions are the editor's macros: never a UFunction, their body is copied into each caller.
       `inline` may sit on the declaration or on an out-of-line definition. */
    bool IsInlineMethod(const FRecord& R, const std::string& Method) const;
    bool CheckMemberNames(const FRecord& R, std::string* Err) const;
    bool ExpandInline(const Json& CallNode, const Json& Def, const std::string& Method, bool bMethod, FBlueprintClass& BP,
                      FCallIR& Out, std::string* Err, const Json* Receiver = nullptr, bool bStaticCall = false);
    /* `final`: the class holding the version of Method a call by name reaches on every object of class Of or below,
       the nearest declaration from Of up that is a Blueprint function, when no subclass can bring its own: Of is
       final, or that declaration is. Null when one could. */
    const FRecord* FinalOwner(const FRecord* Of, const std::string& Method) const;
    /* The definition a call (Call, to the declaration clang picked, Picked) to In's Method may be expanded from in
       place of the call, or null. See LowerCall. */
    const Json* Expandable(const FRecord& In, const std::string& Method, const Json& Call, const Json* Picked) const;
    bool ResumesLater(const Json& N, std::set<const Json*>& Seen) const;
    /* A constant outside any function body, `constexpr int32 kMax = 40;` at namespace scope or static in a class, and an
       inline class variable, `static inline const TArray<FName> Tags = {...};`: decl id -> its VarDecl. It has no
       storage in a Blueprint, so nothing is cooked for it and a use is its value (LowerInlineVar). */
    std::map<std::string, const Json*> ConstVars;
    /* A class's static variable that is not const: a Blueprint class has no static storage, and one that is only its
       initializer must be const for nothing to write it. Refused where it is used (StaticRefusal). */
    std::map<std::string, const Json*> MutableStatics;
    bool LowerInlineVar(const Json& Var, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    bool LowerContainerLiteral(const Json& List, const std::string& Type, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    /* The braced list of the inline variable E names (`kPrimes`, `this->kPrimes`), else null; and its elements as VM
       constants of type Elem, false when one is not. A range-for and Contains over one make no array. */
    const Json* InlineListOf(const Json* E) const;
    bool InlineConsts(const Json& Items, const std::string& Elem, FBlueprintClass& BP, std::vector<FArgIR>* Out);
    /* A == B for a type whose Kismet == is its property's Identical, what Array_Contains compares with; false for text
       and structs, whose == differs (a vector's takes a tolerance). */
    bool ExactEqual(const std::string& Type, FArgIR A, FArgIR B, FBlueprintClass& BP, FArgIR& Out) const;
    bool LowerInlineContains(const Json& Item, const std::string& Elem, const std::vector<FArgIR>& Consts, FBlueprintClass& BP,
                             FArgIR& Out, std::string* Err);
    /* Why a use of the static variable Id cannot compile (a write, or a static that is not const), or empty. */
    std::string StaticRefusal(const std::string& Id, bool bWrite) const
    {
        if (const auto M = MutableStatics.find(Id); M != MutableStatics.end())
            return "static " + Name(*M->second) + ": a Blueprint class has no static storage, so a static variable is inline, "
                   "each use its initializer; declare it `static inline const` (or `static constexpr`), or make it a "
                   "plain member to keep a value";
        if (const auto C = ConstVars.find(Id); bWrite && C != ConstVars.end())
            return Name(*C->second) + " is inline: each use is its initializer, and there is no variable to write";
        return std::string();
    }
    struct FConstVal { bool bFloat = false; double F = 0; int64 I = 0; double Num() const { return bFloat ? F : double(I); } };
    bool FoldConst(const Json& E, FConstVal& Out) const;
    const Json* ValueCastBelow(const Json& N) const;
    bool FreeNegation(const Json& C, Json& Out) const;
    bool ConstToArg(const FConstVal& V, const std::string& Type, FArgIR& Out) const;
    std::map<std::string, const Json*> FreeInlines;                     // decl id -> an inline free function's definition
                                                                        // (a template's: each instantiation)
    std::map<std::string, const Json*> MemberTemplates;                 // decl id -> a member template's instantiation,
                                                                        // expanded inline like an inline method
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
    std::vector<const Json*> InlineStack;                               // the inline functions being expanded (their
                                                                        // definitions: overloads share a name)
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
                      std::vector<FStmtIR>& OutPre, std::string* Err, bool bAlways = false);
    bool PinObject(FArgIR& Obj, const FArgIR* After, size_t NumAfter, FBlueprintClass& BP,
                   std::vector<FPropertyDef>& Locals, std::vector<FStmtIR>& OutPre, std::string* Err);
    bool PinHolder(FArgIR& Place, const FArgIR* After, size_t NumAfter, FBlueprintClass& BP,
                   std::vector<FPropertyDef>& Locals, std::vector<FStmtIR>& OutPre, std::string* Err);
    bool HoistCallArgs(FCallIR& C, FBlueprintClass& BP, std::vector<FPropertyDef>& Locals,
                       std::vector<FStmtIR>& OutPre, std::string* Err);
    bool HasDerefStruct(std::string* Err) const;
    bool LowerPtrCastSource(const Json& Call, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    std::map<std::string, std::string> RefAddr;     // VarDecl id -> pointee: `T& R = *P` keeps the address in an int64 local R
    std::map<std::string, Json> RefAlias;           // VarDecl id -> the variable `T& R = V` is another name for; a copy,
                                                    // since a for-init is lowered out of a temporary Json
    std::map<std::string, FArgIR> RefPlace;         // BindingDecl id -> the place it names: V of a TMap walked in place
    /* N with the variable at its root (under parentheses and `.` members) replaced by what RefAlias names, so a place
       reached through a reference (`auto& [K, V]`'s V is `Map[K]`) is judged as what it is. */
    Json Unalias(const Json& N) const
    {
        const std::string K = Kind(N);
        if ((K == "ParenExpr" || K == "ExprWithCleanups" || (K == "ImplicitCastExpr" && N.value("castKind", std::string()) == "NoOp")
             || (K == "MemberExpr" && !N.value("isArrow", false))) && First(N))
        {
            Json Out = N;
            Out["inner"][0] = Unalias(N["inner"][0]);
            return Out;
        }
        if (K == "DeclRefExpr")
            if (auto A = RefAlias.find(N["referencedDecl"].value("id", std::string())); A != RefAlias.end()) return Unalias(A->second);
        return N;
    }
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
    void PruneConstBranches(std::vector<FStmtIR>& Stmts);
    void ForwardSingleUse(std::vector<FStmtIR>& Stmts, const std::vector<FStmtIR>& All);
    void FlattenBlocks(std::vector<FStmtIR>& Stmts);
    void DropOverwritten(std::vector<FStmtIR>& Stmts);
    void DropUnusedLocals(std::vector<FStmtIR>& Stmts, std::vector<FPropertyDef>& Locals);
    void CoalesceTemps(std::vector<FStmtIR>& Stmts, std::vector<FPropertyDef>& Locals, FBlueprintClass& BP);
    void ThreadBranches(std::vector<FStmtIR>& Stmts, const std::vector<FStmtIR>& All, std::vector<FPropertyDef>& Locals);
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
                     std::vector<FStmtIR>& OutPre, std::string* Err, const FArgIR* Dest = nullptr,
                     const FPropertyDef* DestProp = nullptr);

    /* `X op= Y`, `++X`, `X--`: DesugarUpdate rewrites one into a CompoundStmt of plain `=`, evaluating X's
       side effects once (StabilizeLvalue / HoistExpr park them in synthetic locals first). With Result, the
       block also leaves the expression's value in a local named there. */
    bool DesugarUpdate(const Json& S, Json& Wrap, std::string* Result, std::string* Err);
    Json StabilizeLvalue(const Json& N, Json& Pre, bool bPin = false);
    Json HoistExpr(const Json& N, Json& Pre, bool bPin = false);
    Json SynthLocal(const std::string& Type, const Json& Init, Json& Pre);
    bool LowerUpdateValue(const Json& N, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    /* A written `T&` bound to what the call cannot reach by reference (Refs: each such argument in Call, and its
       parameter's name) gets a hidden local instead, stored back after the call. */
    bool LowerCopyBack(const Json& Call, const std::vector<std::pair<const Json*, std::string>>& Refs,
                       const std::string& Method, FBlueprintClass& BP, FCallIR& Out, std::string* Err);
    std::set<std::string> WarnedCopies;
    std::set<std::string> WarnedNoWorld;        // Class::Function that passed self as a world context without a world
    std::set<std::string> UcsReached;           // the class's functions UserConstructionScript runs, itself included
    std::set<std::string> WarnedUcsSpawn;

    /* `X::StaticClass()`: the record X names, read back from the mod's sources (clang's JSON keeps no qualifier). */
    const FRecord* NamedQualifier(const Json& Ref) const;
    bool IsSubclassOf(const FRecord& Child, const FRecord& Parent) const;
    std::vector<uint8> NativeTail(const FRecord* Component) const;
    std::vector<const Json*> StructArgs(const Json& Value, const FRecord* R, const std::vector<std::string>& Fields) const;
    mutable std::vector<std::string> SourceTexts;                       // the mod directory's .h/.cpp, read on demand

    /* The native UFunction Method overrides, or null; InheritedFlags gets the flags it passes on, also
       for a method implementing an interface's function, which has no Super. */
    uint32 ModMethodFlags(const FRecord& Owner, const std::string& Method, FBlueprintClass& BP);
    FIndex FindEvent(FBlueprintClass& BP, const std::string& FromRecord, const std::string& Method,
                     uint32* InheritedFlags, bool bFlagsOnly = false);
    /* The declaration of the function Method replaces, as ParentClass->FindFunctionByName finds it (each class's own
       functions, then its interfaces, then its super; Self's own are no parent), and the record holding it. */
    std::pair<const FRecord*, const Json*> ReplacedDecl(const FRecord& Self, const std::string& Method) const;
    std::string Unreplicable(const Json& Typed, int32 Depth = 0) const;

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
        if (auto S = SlotStructs.find(CppName); S != SlotStructs.end()) return &S->second;
        auto A = Aliases.find(CppName);
        return A == Aliases.end() || A->second == CppName ? nullptr : Find(A->second);
    }
    /* A record's package and UClass / UScriptStruct name. A mod's own is cooked where its namespace says (PathIn),
       named after its leaf, a class with _C; the engine's and the game's carry theirs in UE_CLASS. */
    std::string PackageOf(const FRecord& R) const { return R.UePackage.empty() ? PathIn(ModPackage, R.CppName) : R.UePackage; }
    std::string ClassOf(const FRecord& R) const
    {
        return !R.UePackage.empty() ? R.UeName : LeafOf(R.CppName) + (R.bIsStruct ? "" : "_C");
    }
    /* OutDir holds UE_MOD_PACKAGE, and is <Content>/<that path without /Game>, as bpbuild stages a mod. A package under
       the mod's is saved under OutDir, any other /Game one under Content. The log names the first relative to OutDir. */
    std::filesystem::path ContentDir(const std::string& OutDir) const
    {
        std::filesystem::path Content(OutDir);
        for (size_t At = ModPackage.find('/', 1); At != std::string::npos; At = ModPackage.find('/', At + 1))
            Content = Content.parent_path();
        return Content;
    }
    std::string Shown(const std::string& Package) const
    {
        return Package.compare(0, ModPackage.size() + 1, ModPackage + "/") == 0 ? Package.substr(ModPackage.size() + 1) : Package;
    }
    std::filesystem::path FileOf(const std::string& OutDir, const std::string& Package) const
    {
        const std::string In = Shown(Package);
        return In != Package ? std::filesystem::path(OutDir) / In : ContentDir(OutDir) / Package.substr(6);
    }
    bool SavePackage(FPackage& P, const std::string& OutDir, const std::string& Package, std::string* Err) const
    {
        const std::filesystem::path File = FileOf(OutDir, Package);
        std::error_code Ec;
        std::filesystem::create_directories(File.parent_path(), Ec);
        return P.Save(File.string(), Err);
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
    std::optional<std::string> ApiDir;      // `--api`: where the uncooked editor-side stubs go
    std::string SourceDir;      // the compiled .cpp's folder: what __EmbedFile__ resolves a relative path against
    /* A container inside a container: UE has no such property, so the inner one is the single member (Value) of a
       wrapper struct, <wrapper name> -> the container type. The wrapper has the container's layout. */
    std::map<std::string, std::string> NestedWrappers;
    static constexpr const char* NestedPackage = "/Game/_ElytrasMods/_NestedContainerStructs";
    bool NestedWrapperOut(const std::string& ContainerType, size_t OutArg, const std::string& ResultType, FStmtIR Copy,
                          FBlueprintClass& BP, FArgIR& Call, std::string* Err);
    /* A type as part of a name: `TMap<int32, TArray<int32>>` is TMap_int_TArray_int. clang spells one type int or
       int32 depending on where it comes from: one name for both. */
    std::string TypeTag(const std::string& Type) const
    {
        std::string Name, Word;
        auto Flush = [&]() { Name += Word == "int32" ? "int" : Word == "long" ? "int64" : Word; Word.clear(); };
        for (char C : StripTypeKeywords(Type) + " ")
            if (std::isalnum(uint8(C)) || C == '_') Word += C;
            else
            {
                if (Word == "long" && Name.size() >= 5 && Name.compare(Name.size() - 5, 5, "int64") == 0) Word.clear();
                Flush();
                if (C == '<' || C == ',') Name += '_';
                else if (C == '*') Name += "Ptr";
            }
        return Name;
    }
    std::string NestedWrapper(const std::string& ContainerType)
    {
        const std::string Name = "FNC_" + TypeTag(ContainerType);
        NestedWrappers.emplace(Name, StripTypeKeywords(ContainerType));
        return Name;
    }
    /* A TMap walked in place (LowerMapWalk): per map type, its element as a struct, FMapSlot_<map>, whose Key, Value
       and the set's two hash links give it the sparse array's stride, and FMapSlots_<map>, whose one member __Slots__
       reads the map's storage as a TArray of those. Like FDeref, made on first use and cooked after lowering. */
    std::map<std::string, Json> SlotAst;              // struct name -> the FieldDecl nodes its record's Fields point into
    std::map<std::string, FRecord> SlotStructs;
    bool MapSlotView(const std::string& MapType, const std::string& Key, const std::string& Value, FBlueprintClass& BP,
                     FIndex* View, FIndex* Slot, std::string* Err);
    FIndex NestedWrapperImport(const std::string& ContainerType, FBlueprintClass& BP)
    {
        const std::string Name = NestedWrapper(ContainerType);
        return BP.ScriptStruct(std::string(NestedPackage) + "/" + Name, Name);
    }
    bool GenerateNestedWrappers(const std::string& OutDir, std::string* Err);
    /* The read-hoist scratch, synthesized on first deref rather than copied into every mod source.
       It is kept out of Records so the cook loop cannot reach it: whether a deref happened is only
       known once lowering has run, so GenerateDerefStruct cooks it afterwards. */
    mutable Json    DerefAst;        // owns the three FieldDecl nodes SynthDeref.Fields point into
    mutable FRecord SynthDeref;
    mutable bool    bSynthDeref = false;
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
    std::map<std::string, std::string> AssetPaths;  // UE_ASSET_AT: Ns::variable -> the path of an asset cooked elsewhere
    std::map<std::string, std::string> VarScope;    // a namespace-scope variable's decl id -> its namespace, "A::B::"
    std::map<std::string, const Json*> NsVars;      // a namespace-scope variable's decl id -> the decl
    std::map<std::string, const Json*> NsVarNamed;  // Ns::variable -> the decl
    std::set<std::string> AssetAlls;                // UE_ASSET_ALL: Ns::All
    std::map<std::string, FRecord> Globals;         // Ns::variable a function uses -> the class holding it, see LowerGlobal
    std::map<std::string, Json> GlobalAst;          // an All's member: its default assembled from the UE_ASSET_ATs
    bool LowerGlobal(const Json& Decl, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
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
    const Json* CurFnDef = nullptr;                  // ... its definition, which a call from it never expands
    std::string LatentRefusal;                        // why that method cannot make a latent call, or empty
    bool bMadeLatentCall = false;                     // LowerCall: it made one, so it moves into the ubergraph
    std::string StaticLocal;                          // LowerBody: a static it keeps in the ubergraph's frame, or empty
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
    bool PlaceActivations(std::vector<FStmtIR>& Stmts, std::string* Err);
    bool PeelFirstRound(std::vector<FStmtIR>& Stmts, const int32* Await);
    std::vector<FCompletion> Completions;             // the method being lowered's
    std::set<std::string> GeneratedEvents;            // the class's, so two never share a name
    int32 SwitchDepth = 0;                            // LowerBody: the switches around it
    std::map<std::string, int32> GotoLabels;          // the body being lowered's: clang LabelDecl id -> FStmtIR::LabelId
    int32 NextGotoLabel = 0;                          // never reset: latent functions share one ubergraph script
    bool bBodyHasGoto = false;                        // a goto can re-reach any declaration, as a loop does
    bool bFnHasGoto = false;                          // the method or anything inlined into it has one: never restored
    int32 ReEntered = 0;                              // inline expansions under a caller's loop or goto: their bodies run again
    std::vector<FStmtIR> WriteBacks;                  // LowerBody: the Map_Add of each TMap range-for around it, innermost last
    int32 GotoLabelOf(const std::string& DeclId)
    {
        const auto It = GotoLabels.find(DeclId);
        return It != GotoLabels.end() ? It->second : (GotoLabels[DeclId] = NextGotoLabel++);
    }
};

/* Whether running Stmts never falls off their end: the last one returns or jumps away (Return only, with
   bReturnOnly), itself or in both branches of an `if`. */
bool NeverFallsThrough(const std::vector<FStmtIR>& Stmts, bool bReturnOnly = false)
{
    if (Stmts.empty()) return false;
    const FStmtIR& St = Stmts.back();
    if (St.K == FStmtIR::Return) return true;
    if (St.K == FStmtIR::If && !St.bJumpOut)
        return St.Then && St.Else && NeverFallsThrough(*St.Then, bReturnOnly) && NeverFallsThrough(*St.Else, bReturnOnly);
    return !bReturnOnly && (St.K == FStmtIR::Break || St.K == FStmtIR::Continue || St.K == FStmtIR::Goto
                            || St.K == FStmtIR::InlineReturn);
}

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
            const std::optional<FFieldRef> RValue = St.Var.K == FArgIR::Index ? std::nullopt
                                                  : std::optional<FFieldRef>(FFieldRef{ Top.S, VarOwner });
            S.LetPath(St.Var.LetOp, Path, VarOwner,
                      [St, SelfExp](FScript& V) { EmitArg(V, St.Var, SelfExp, nullptr); },
                      [St, SelfExp, RValue](FScript& V) { EmitValueInto(V, St.Value, SelfExp, RValue); });
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
                      [St, SelfExp](FScript& V) { EmitValueInto(V, St.Value, SelfExp, FFieldRef{ St.Var.S, SelfExp }); });
            break;

        case FStmtIR::Return:
            if (St.bHasValue)       // the value goes to ReturnValue, the editor's Let destination for a return
                S.Return([St, SelfExp](FScript& V) { EmitValueInto(V, St.Value, SelfExp, FFieldRef{ "ReturnValue", SelfExp }); });
            else
                S.Return();
            break;

        case FStmtIR::If:
        {
            if (St.bJumpOut)
            {
                const int32 Out = S.JumpIfNot(0, [St, SelfExp](FScript& C) { EmitArg(C, St.Cond, SelfExp, nullptr); });
                if (Loop) ((*St.Then)[0].K == FStmtIR::Break ? Loop->Breaks : Loop->Continues).push_back(Out);
                else S.PatchJumpTarget(Out, S.MemorySize());
                break;
            }
            const int32 NotPatch = S.JumpIfNot(0,
                [St, SelfExp](FScript& C) { EmitArg(C, St.Cond, SelfExp, nullptr); });
            if (St.Then) EmitStmts(*St.Then, S, SelfExp, Loop, Returns);
            if (St.Else && !St.Else->empty())
            {
                /* A branch that returns or jumps away needs no jump over the else. */
                const int32 EndPatch = St.Then && NeverFallsThrough(*St.Then) ? -1 : S.Jump(0);
                S.PatchJumpTarget(NotPatch, S.MemorySize());
                EmitStmts(*St.Else, S, SelfExp, Loop, Returns);
                if (EndPatch >= 0) S.PatchJumpTarget(EndPatch, S.MemorySize());
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
            if (!St.bPostTest && !St.bConstCond) Test();
            if (St.Body) EmitStmts(*St.Body, S, SelfExp, &Inner, Returns);
            for (int32 P : Inner.Continues) S.PatchJumpTarget(P, S.MemorySize());
            if (St.Inc) EmitStmts(*St.Inc, S, SelfExp, Loop, Returns);
            if (St.bPostTest && !St.bConstCond) Test();     // do/while: `continue` lands on the test, as in C++
            if (!St.bConstCond || St.Cond.B) S.Jump(Head);  // `do {} while (false)` runs once
            if (St.Trailer && !Inner.Breaks.empty())
            {
                /* A `break` runs the trailer and falls into the exit; a failed condition skips it. */
                for (int32 P : Inner.Breaks) S.PatchJumpTarget(P, S.MemorySize());
                Inner.Breaks.clear();
                EmitStmts(*St.Trailer, S, SelfExp, Loop, Returns);
            }
            if (ExitPatch >= 0) S.PatchJumpTarget(ExitPatch, S.MemorySize());
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
                if (B.K == FStmtIR::Label) { if (B.LabelId >= 0) LabelAt[B.LabelId] = S.MemorySize(); continue; }
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
        /* UE_ASSET_EDIT: a pointer naming the target, then the braced variable holding the edit, one number between them. */
        if (Kind(N) == "VarDecl" && Name(N).compare(0, 10, "UeEditOf__") == 0)
        {
            EditTargets[Ns + Name(N).substr(10)] = &N;
            return;
        }
        if (Kind(N) == "VarDecl" && Name(N).compare(0, 8, "UeEdit__") == 0)
        {
            Edits.emplace_back(Ns + Name(N).substr(8), &N);
            return;
        }
        if (Kind(N) == "FunctionDecl" && Name(N).compare(0, 14, "UeAssetEdits__") == 0)
        {
            AssetEditBlocks.push_back(&N);
            return;
        }
        if (Kind(N) == "VarDecl" && BracedInit(N)) AssetDecls.push_back(&N);
        if (Kind(N) == "VarDecl" && !Ns.empty()) VarScope[N.value("id", std::string())] = Ns;
        if (Kind(N) == "VarDecl") NsVars[N.value("id", std::string())] = NsVarNamed[Ns + Name(N)] = &N;
        if (Kind(N) == "VarDecl" && Name(N).size() > 12 && Name(N).compare(Name(N).size() - 12, 12, "__UeAssetAll") == 0)
        {
            AssetAlls.insert(Ns + Name(N).substr(0, Name(N).size() - 12));
            return;
        }
        if (Kind(N) == "VarDecl" && Name(N).size() > 9 && Name(N).compare(Name(N).size() - 9, 9, "__UeAsset") == 0)
        {
            FindLiteral(N, AssetPaths[Ns + Name(N).substr(0, Name(N).size() - 9)]);
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
                /* Values past int's range are converted to the enum's wider type around the ConstantExpr. */
                while (V && !V->contains("value") && Kind(*V) == "ImplicitCastExpr") V = First(*V);
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
            /* No fixed type (a mod's own `enum { kMax = 1000 };`): C++ makes it int, or wider if a value needs it. */
            bool bWide = false;
            for (const auto& En : Mine) bWide |= En.second < INT32_MIN || En.second > INT32_MAX;
            const int32 Width = Under.empty() ? (bWide ? 8 : 4) : Canon == "uint8" ? 1 : Canon == "int64" ? 8 : 4;
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
            auto& Inlines = RecIt->second.Inlines;
            if (N.value("inline", false) || Inlines.count(PrevId)) Inlines[PrevId] = Inlines[N.value("id", std::string())] = &N;
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
        std::string Category;       // UE_CATEGORY is positional, like an access specifier
        ForEach(N, [&](const Json& C) {
            if (Kind(C) == "VarDecl" && Name(C).compare(0, 12, "UeCategory__") == 0)
            {
                Category.clear();
                FindLiteral(C, Category);
            }
            if (!Category.empty() && C.contains("name") && (Kind(C) == "CXXMethodDecl" || Kind(C) == "FieldDecl"))
                R.Categories[Name(C)] = Category;
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
            else if (Kind(C) == "VarDecl" && Name(C) == "UePatchMeta")
            {
                R.bIsPatch = true;
            }
            else if (Kind(C) == "VarDecl" && Name(C) == "UeStructMeta")
            {
                R.bIsStruct = true;
                std::string Owner;
                if (FindLiteral(C, Owner))
                {
                    R.UePackage = PathIn(Owner, R.CppName);
                    R.UeName = LeafOf(R.CppName);
                }
            }
            else if (Kind(C) == "VarDecl" && Name(C).size() > 11 && Name(C).compare(Name(C).size() - 11, 11, "__UeForward") == 0)
            {
                std::string Target;
                if (FindLiteral(C, Target)) R.Forwards[Name(C).substr(0, Name(C).size() - 11)] = Target;
            }
            else if (Kind(C) == "VarDecl" && Name(C).size() > 8 && Name(C).compare(Name(C).size() - 8, 8, "__UeName") == 0)
            {
                std::string Real;
                if (FindLiteral(C, Real)) R.UeNames[Name(C).substr(0, Name(C).size() - 8)] = Real;
            }
            else if (Kind(C) == "VarDecl" && Name(C).size() > 13 && Name(C).compare(Name(C).size() - 13, 13, "__UeSubobject") == 0)
            {
                std::string Sub;
                if (FindLiteral(C, Sub)) R.Subobjects[Name(C).substr(0, Name(C).size() - 13)] = Sub;
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
            else if (Kind(C) == "CXXMethodDecl" && C.contains("name") && !C.value("isImplicit", false))
            {
                /* genueapi's overload without the world context shares the name; the longer one is the UFunction. */
                const Json*& Slot = R.Methods[Name(C)];
                if (!Slot || ParmNames(C).size() > ParmNames(*Slot).size()) Slot = &C;
                R.AllMethods.push_back(&C);
                MethodOwner[C.value("id", std::string())] = R.CppName;
                R.MethodAccess[Name(C)] = Access;
                if (C.value("inline", false)) R.Inlines[C.value("id", std::string())] = &C;
                if (HasAttr(C, "FinalAttr")) R.FinalMethods.insert(Name(C));
            }
            else if (Kind(C) == "CXXConstructorDecl")
                R.Ctors.push_back(&C);
            else if (Kind(C) == "FieldDecl" && C.contains("name"))
            {
                R.Fields.push_back(&C);
                FieldOwner[C.value("id", std::string())] = R.CppName;
                if (Access == FUNC_Private) R.PrivateFields.insert(Name(C));
            }
            else if ((Kind(C) == "TypeAliasDecl" || Kind(C) == "TypedefDecl") && C.contains("name"))
                R.TypeAliases[Name(C)] = StripTypeKeywords(TypeOf(C));
            else if (Kind(C) == "FinalAttr")
                R.bFinal = true;
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

    /* UE_ENUM / UE_ENUM_IN: a UserDefinedEnum at <owner>/<Name>, typed like UeApi's native enums. clang spells a use
       inside the enum's namespace as written, `EKind`, so a leaf no other one shares names it too, as Bare does a class. */
    std::map<std::string, int32> Leaves;
    for (const auto& Mark : EnumMarks) ++Leaves[LeafOf(Mark.first)];
    for (const auto& [Enum, Owner] : EnumMarks)
    {
        auto D = EnumDecls.find(Enum);
        if (D == EnumDecls.end() || D->second.empty()) { *Err = "UE_ENUM(" + Enum + ") names no enum with enumerators"; return false; }
        const std::string U = EnumUnderlying[Enum];
        if (U != "uint8" && U != "int32" && U != "int64")
        { *Err = "UE_ENUM(" + Enum + "): declare it `: uint8` (a Blueprint enum), `: int32` or `: int64`"; return false; }
        /* The UE C++ idiom `..., EGear_MAX }` declares the sentinel the writer adds anyway: one past the largest value,
           under the same FName. Kept, it would be a second entry of that name; so it is the sentinel, or refused. */
        const std::string MaxName = LeafOf(Enum) + "_MAX";
        if (auto M = std::find_if(D->second.begin(), D->second.end(), [&](const auto& En) { return En.first == MaxName; });
            M != D->second.end())
        {
            int64 Largest = INT64_MIN;
            for (const auto& En : D->second) if (En.first != MaxName) Largest = std::max(Largest, En.second);
            if (D->second.size() == 1 || M->second != Largest + 1)
            { *Err = "UE_ENUM(" + Enum + "): " + MaxName + " is the sentinel the engine adds, one past the largest value; "
                     "leave it out or give it that value"; return false; }
            D->second.erase(M);
        }
        const int64 Top = U == "uint8" ? 254 : U == "int32" ? int64(INT32_MAX) - 1 : INT64_MAX - 1;
        const int64 Bottom = U == "uint8" ? 0 : U == "int32" ? int64(INT32_MIN) : INT64_MIN;
        for (const auto& En : D->second)
            if (En.second < Bottom || En.second > Top)
            { *Err = "UE_ENUM(" + Enum + "): " + En.first + " is out of range (the largest value is _MAX's)"; return false; }
        const std::string Leaf = LeafOf(Enum);
        /* An enumerator's FName is <Enum>::<Name>, kept in one global map where the first enum loaded wins
           (UEnum::AddNamesToMasterList, Enum.cpp 83-94): the game's enum of that name would answer every lookup. */
        if (auto N = Enums.find(Leaf); (Owner.empty() || Owner == ModPackage) && N != Enums.end()
            && N->second.Package.compare(0, ModPackage.size(), ModPackage) != 0)
        { *Err = "UE_ENUM(" + Enum + "): " + N->second.Package + " already has an enum " + Leaf + ", whose enumerator names ("
                 + Leaf + "::...) the engine keeps in one global table; rename it"; return false; }
        std::string First = D->second.front().first;
        for (const auto& En : D->second) if (En.second == 0) { First = En.first; break; }
        Enums[Enum] = { PathIn(Owner.empty() ? ModPackage : Owner, Enum), Leaf, U, First };
        if (Leaf != Enum && Leaves[Leaf] == 1) Enums.emplace(Leaf, Enums[Enum]);
        if (Owner.empty() || Owner == ModPackage) ModEnums[Enum] = D->second;
    }

    for (auto& It : Records)
    {
        FRecord& R = It.second;
        /* Only the replicating class's own CPF_Net properties enter its replication list; a struct variable is sent
           whole, so a marker on one of its members has no effect. */
        if (R.bIsStruct && !R.Replicated.empty() && !R.IsNative())
        {
            *Err = R.CppName + "::" + R.Replicated.begin()->first + ": UE_REPLICATED on a struct member has no effect: a "
                   "struct replicates whole, through the class variable that holds it; drop the marker";
            return false;
        }
        if (R.UePackage.empty()) continue;
        if (R.UePackage != PathIn(ModPackage, R.CppName)) continue;
        if (!R.bIsStruct && R.UeName != LeafOf(R.CppName) + "_C")
        {
            *Err = "UE_CLASS on " + R.CppName + " says \"" + R.UeName
                 + "\", but cooking it here requires \"" + LeafOf(R.CppName) + "_C\"";
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
                return bFlagsOnly ? Null() : BP.EngineFunction(PackageOf(*R), ClassOf(*R), UeMethod);
            }
        if (R->IsNative() && R->Methods.count(Method) && !R->Forwards.count(Method))   // a forwarder is no UFunction to override
        {
            *InheritedFlags = FlagsOf(*R);
            return bFlagsOnly ? Null() : BP.EngineFunction(R->UePackage, R->UeName, UeMethod);
        }
        /* Measured on BP_SentryGun_MoveMarker: an implementation of an interface of the class's own list has no Super.
           One an ancestor implements is found through it, as ParentClass->FindFunctionByName looks in each class's
           interfaces before its super (Class.cpp:5281-5323): a mod ancestor's own stub (Generate compiles one for each
           function it leaves out), a native one's interface function. */
        for (const std::string& I : R->Interfaces)
            if (const FRecord* IR = Find(I); IR && IR->IsNative() && IR->Methods.count(Method))
            {
                *InheritedFlags = FlagsOf(*IR);
                if (R == Self || bFlagsOnly) return Null();
                return R->IsNative() ? BP.EngineFunction(IR->UePackage, IR->UeName, UeMethod)
                                     : BP.EngineFunction(PackageOf(*R), ClassOf(*R), UeMethod);
            }
    }
    return Null();      // not an override
}

/* A node's type with every typedef and alias resolved, as clang's canonical type spells it. */
std::string DesugaredTypeOf(const Json& N)
{
    auto It = N.find("type");
    return It == N.end() ? std::string() : It->value("desugaredQualType", It->value("qualType", std::string()));
}

std::pair<const FRecord*, const Json*> FCompiler::ReplacedDecl(const FRecord& Self, const std::string& Method) const
{
    for (const FRecord* R = &Self; R; R = R->Base.empty() ? nullptr : Find(R->Base))
    {
        if (R != &Self && !R->bIsInterface)
            if (auto M = R->Methods.find(Method); M != R->Methods.end()
                && (R->IsNative() ? !R->Forwards.count(Method) : !IsStaticDecl(*M->second) && !IsInlineMethod(*R, Method)))
                return { R, M->second };
        for (const std::string& I : R->Interfaces)
            for (const FRecord* IR : InterfaceChain(Find(I)))
                if (auto M = IR->Methods.find(Method); M != IR->Methods.end()) return { IR, M->second };
    }
    return {};
}

/* What of a C++ type never reaches the other machine, or empty: a TMap or TSet (their NetSerializeItem only logs) or an
   interface (FInterfaceProperty's writes nothing), found through containers and a mod struct's members, as RepLayout
   sends a struct member by member (InitFromProperty_r). A native struct may serialize itself, so it is not looked in. */
std::string FCompiler::Unreplicable(const Json& Typed, int32 Depth) const
{
    const std::string Type = DesugaredTypeOf(Typed);
    if (Type.find("TMap<") != std::string::npos || Type.find("TSet<") != std::string::npos) return "a TMap or TSet";
    if (Type.find("TScriptInterface<") != std::string::npos) return "an interface";
    if (Depth > 8) return {};
    for (size_t At = 0; At < Type.size();)
    {
        const size_t End = std::find_if(Type.begin() + At, Type.end(), [](char C) { return !isalnum(uint8(C)) && C != '_' && C != ':'; }) - Type.begin();
        if (End > At)
            if (const FRecord* S = Find(Type.substr(At, End - At)); S && S->bIsStruct && !S->IsNative())
                for (const Json* F : S->Fields)
                    if (std::string Why = Unreplicable(*F, Depth + 1); !Why.empty()) return Why + " in " + S->CppName;
        At = End + 1;
    }
    return {};
}

/* A function's signature as the engine tells two apart (IsSignatureCompatibleWith, Class.cpp:5882): the return type and
   each parameter's, typedefs resolved, `const`, `class`, `struct` and `enum` dropped, and a const reference read as the
   value it passes. Names do not count. */
std::vector<std::string> SignatureOf(const Json& Fn)
{
    auto Norm = [](std::string T, bool bParm) {
        const bool bConstRef = bParm && T.compare(0, 6, "const ") == 0 && !T.empty() && T.back() == '&';
        for (const char* Kw : { "const ", "struct ", "class ", "enum " })
            for (size_t At; (At = T.find(Kw)) != std::string::npos;) T.erase(At, strlen(Kw));
        T.erase(std::remove(T.begin(), T.end(), ' '), T.end());
        if (bConstRef) T.pop_back();
        return T;
    };
    const std::string FnType = DesugaredTypeOf(Fn);
    std::vector<std::string> Out{ Norm(FnType.substr(0, FnType.find('(')), false) };
    ForEach(Fn, [&](const Json& C) { if (Kind(C) == "ParmVarDecl") Out.push_back(Norm(DesugaredTypeOf(C), true)); });
    return Out;
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
    uint32 Flags = Inherited ? Inherited & kOverrideInherits : kPlainMethodFlags;
    if (auto A = Owner.MethodAccess.find(Method); !Inherited && A != Owner.MethodAccess.end())
        Flags = (Flags & ~uint32(FUNC_Public | FUNC_Protected | FUNC_Private)) | A->second;
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

/* Conv.json and Ops.json name a soft pointer by its template over UObject, the type Kismet's functions take: any soft
   pointer passes for it, whatever class it is of (genueapi's conv_kind). */
std::string SoftKey(const std::string& T)
{
    std::string Of;
    for (const char* Tpl : { "TSoftObjectPtr", "TSoftClassPtr" })
        if (TemplateArg(T, Tpl, &Of)) return std::string(Tpl) + "<UObject>";
    return T;
}

const FConv* FCompiler::FindConv(const std::string& From, const std::string& To) const
{
    const std::string F = SoftKey(From), T = SoftKey(To);
    for (const FConv& C : Convs)
        if (C.From == F && C.To == T) return &C;
    return nullptr;
}

const FOpInfo* FCompiler::FindOp(const std::string& Op, const std::string& Lhs, const std::string& Rhs) const
{
    const std::string L = SoftKey(Lhs), R = SoftKey(Rhs);
    for (const FOpInfo& O : Ops)
        if (O.Op == Op && O.Lhs == L && O.Rhs == R) return &O;
    return nullptr;
}

void FCompiler::ApplyConv(const FConv& C, FBlueprintClass& BP, FArgIR& Arg)
{
    WrapInCall(Arg, BP.EngineFunction(C.Package, C.Class, C.Fn));
    for (const Json& E : C.Extra) Arg.Sub->Args.push_back(ConstArg(E));
    for (const size_t At : C.Refs)
        if (At < Arg.Sub->Args.size())
        {
            Arg.Sub->RefParms.resize(Arg.Sub->Args.size());
            Arg.Sub->RefParms[At] = At == 0 ? C.From : Arg.Sub->Args[At].InnerType;
        }
    Arg.Sub->bRefsTakeConst = true;
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
    /* clang spells a class as written, `FAmmo` inside its namespace and `Weapons::FAmmo` outside. */
    if (const FRecord* R = Find(T)) return R->CppName;
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
    /* No Conv_ row turns an int64, a float or a byte (an enum's too) into a bool: C++ tests it against zero, and so
       does this. A NaN is true, as NotEqual_FloatFloat answers. */
    const EStrKind FromNum = StrKindOf(From);
    if (ToKind == SK_Bool && (FromNum == SK_Int64 || (FromNum == SK_Float && Arg.K != FArgIR::Float) || FromNum == SK_Byte))
    {
        WrapInCall(Arg, BP.EngineFunction("/Script/Engine", "KismetMathLibrary", FromNum == SK_Int64 ? "NotEqual_Int64Int64"
                                          : FromNum == SK_Float ? "NotEqual_FloatFloat" : "NotEqual_ByteByte"));
        FArgIR Zero;
        Zero.K = FromNum == SK_Int64 ? FArgIR::Int64 : FromNum == SK_Float ? FArgIR::Float : FArgIR::Byte;
        Arg.Sub->Args.push_back(Zero);
        Arg.InnerType = "bool";
        return true;
    }

    /* `(AItem *)Soft`: the object or class a soft pointer names, null unless it is loaded (Conv_SoftObjectReferenceToObject,
       Conv_SoftClassReferenceToClass). An object or class to a soft pointer is a Conv_ row too, found below. */
    if (ToKind == SK_Object && From.compare(0, 5, "TSoft") == 0)
        if (const FConv* C = FindConv(From, To)) { ApplyConv(*C, BP, Arg); return true; }

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
    if (Arg.K == FArgIR::Float && ToKind == SK_Bool) { Arg.K = FArgIR::Bool; Arg.B = Arg.F != 0; return true; }
    if (Arg.K == FArgIR::Int && ToKind == SK_Byte)  { Arg.K = FArgIR::Byte; return true; }
    if (To.compare(0, 5, "TSoft") == 0)
    {
        if (Arg.K == FArgIR::Str) { Arg.K = FArgIR::SoftPath; Arg.InnerType = To; return true; }
        if (From.compare(0, 5, "TSoft") == 0) return true;
    }

    /* C++ truncates a float toward zero: FTrunc/FTrunc64 (FMath::TruncToInt, the Blueprint autocast). Conv.json
       has no such row, and the FString round trip prints 6 decimals first, making 0.99999994f 1. */
    if (FromKind == SK_Float && (ToKind == SK_Int || ToKind == SK_Int64))
    {
        WrapInCall(Arg, BP.EngineFunction("/Script/Engine", "KismetMathLibrary", ToKind == SK_Int ? "FTrunc" : "FTrunc64"));
        Arg.InnerType = To;
        return true;
    }

    if (const FConv* Direct = FindConv(From, To)) { ApplyConv(*Direct, BP, Arg); return true; }
    /* A uint8 or a bool has no Conv_ row to int64, but int32 holds every value it can be. */
    if (const FConv *In = FindConv(From, "int"), *Out = FindConv("int", To);
        In && Out && (FromNum == SK_Byte || FromNum == SK_Bool) && ToKind == SK_Int64)
    { ApplyConv(*In, BP, Arg); ApplyConv(*Out, BP, Arg); return true; }
    /* UE 4.27 Blueprint has no int64 -> float at all (UE5 has Conv_Int64ToDouble, its reals being double), so the value
       passes through int32: only the low 32 bits survive, where C++ would round the whole value. */
    if (const FConv *In = FindConv(From, "int"), *Out = FindConv("int", To); In && Out && FromNum == SK_Int64 && ToKind == SK_Float)
    {
        printf("  warning: %s::%s: int64 -> float goes through int32 (UE 4.27 has no int64 -> float), so a value outside "
               "int32's range wraps\n", Cur ? Cur->CppName.c_str() : "", CurFnName.c_str());
        ApplyConv(*In, BP, Arg);
        ApplyConv(*Out, BP, Arg);
        return true;
    }
    const auto IsNumber = [](EStrKind K) { return K == SK_Int || K == SK_Int64 || K == SK_Float || K == SK_Bool || K == SK_Byte; };
    for (const char* Via : {"FString", "FText"})     // FText: int64 has no engine Conv_Int64ToString
    {
        if (IsNumber(FromKind) && IsNumber(ToKind)) break;     // text would round or reject what C++ converts exactly
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
    std::vector<std::string> Names;
    for (const auto& F : SI.Fields) Names.push_back(F.second);
    const std::vector<const Json*> Args = StructArgs(CtorNode, Find(T), Names);
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
    const FIndex Struct = BP.ScriptStruct(PackageOf(*R), ClassOf(*R));
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
            Set.Var.S = bPlainName ? UeNameOf(R, Name(F)) : ModFieldName(PackageOf(*R), Name(F));
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

/* `{ 1, 2 }` made into a TArray, TSet or TMap in a function: the editor's Make Array / Make Set / Make Map, a temp the
   frame keeps, filled whole by EX_SetArray / EX_SetSet / EX_SetMap (`[S]` KismetCompilerVMBackend.cpp:1635), which
   empty it first (ScriptCore.cpp:3409), so a loop that comes round again makes it afresh. A set's and a map's count is
   its elements' (a map's pairs'). List is the CXXStdInitializerListExpr; a map's elements are `{ key, value }`. */
bool FCompiler::LowerContainerLiteral(const Json& List, const std::string& Type, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    const std::string T = StripTypeKeywords(Type);
    std::string Of;
    const std::string Op = TemplateArg(T, "TArray", &Of) ? "__SetArray__" : TemplateArg(T, "TSet", &Of) ? "__SetSet__"
                         : TemplateArg(T, "TMap", &Of) ? "__SetMap__" : "";
    if (Op.empty()) { *Err = "a braced list makes a TArray, TSet or TMap here, not a " + T; return false; }
    if (!CurLocals) { *Err = "internal: a container value outside a function body"; return false; }

    const std::string Tmp = "__Make" + std::to_string(ReadTmpCounter++) + "__";
    FPropertyDef PD;
    if (!TypeToProperty(T, Tmp, 0, "a " + T + " value", BP, &PD, Err)) return false;
    PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
    CurLocals->push_back(PD);

    auto Body = std::make_shared<std::vector<FStmtIR>>(1);
    FCallIR& Fill = (*Body)[0].Call;
    Fill.Intrinsic = Op;
    Fill.Args.resize(1);
    Fill.Args[0].K = FArgIR::Local;
    Fill.Args[0].S = Tmp;
    const Json* Items = &List;
    while (Items && Kind(*Items) != "InitListExpr") Items = First(*Items);
    bool bOk = true;
    if (Items)
        ForEach(*Items, [&](const Json& E) {
            if (!bOk) return;
            const Json* Pair = Op == "__SetMap__" ? Strip(&E) : nullptr;
            if (Pair && (Kind(*Pair) != "InitListExpr" || !Nth(*Pair, 1)))
            { *Err = "a map's element is a braced `{ key, value }` pair"; bOk = false; return; }
            for (const Json* V : Pair ? std::vector<const Json*>{ Nth(*Pair, 0), Nth(*Pair, 1) } : std::vector<const Json*>{ &E })
                if (bOk) bOk = LowerArg(*V, BP, Fill.Args.emplace_back(), Err);
        });
    if (!bOk) return false;

    auto Block = std::make_shared<std::vector<FStmtIR>>(1);
    (*Block)[0].K = FStmtIR::Block;
    (*Block)[0].Body = Body;
    Out.K = FArgIR::Call;
    Out.InnerType = T;
    Out.Sub = std::make_shared<FCallIR>();
    Out.Sub->Intrinsic = "__Inline__";
    Out.Sub->Inline = Block;
    Out.Sub->InlineResult = Tmp;
    Out.Sub->InlineType = T;
    return true;
}

/* A constant or an inline class variable has no storage in a Blueprint: each use is its initializer, lowered where it
   is used - a number folded to its literal, anything else the expression itself, as the editor's literal pin or Make
   node would be (a container is LowerContainerLiteral's). C++ runs the initializer once; a use here runs it each time. */
bool FCompiler::LowerInlineVar(const Json& Var, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    const Json* Init = First(Var);
    if (!Init) { *Err = Name(Var) + " is inline, so it needs its initializer where it is declared"; return false; }
    if (FConstVal V; FoldConst(*Init, V) && ConstToArg(V, TypeOf(Var), Out)) return true;
    return LowerArg(*Init, BP, Out, Err);
}

bool FCompiler::LowerField(const Json& MemberNode, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    /* A read of an inline class variable never gets here (LowerArgRaw), so a static here is written or not const. */
    if (std::string Why = StaticRefusal(MemberNode.value("referencedMemberDecl", std::string()), true); !Why.empty())
    { *Err = std::move(Why); return false; }
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
                    : ModFieldName(PackageOf(*R), Name(MemberNode));
        Out.Owner = BP.ScriptStruct(PackageOf(*R), ClassOf(*R));
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
                KeepStructLoaded(Out.Owner, ClassOf(*R), Size);
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
    Out.Owner = !R->IsNative() && R == Cur ? BP.ClassIndex() : BP.PropertyOwner(PackageOf(*R), ClassOf(*R));
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
    const Json& Ref = (*F)["referencedDecl"];
    auto Owner = MethodOwner.find(Ref.value("id", std::string()));
    if (const FRecord* R = Owner != MethodOwner.end() ? Find(Owner->second) : nullptr; R && IsInlineMethod(*R, Name(Ref)))
    { *Err = "a delegate cannot bind " + Name(Ref) + ": an inline function is expanded where it is called, no UFunction (drop `inline`)"; return false; }
    /* EX_InstanceDelegate binds the name on this object, so a function of a class this one does not derive from is
       never found there, and the broadcast or timer skips it (ScriptDelegates.h 38-49, 479-502). */
    if (Owner != MethodOwner.end() && Cur)
    {
        bool bMine = false;
        for (const FRecord* A = Cur; A && !bMine; A = A->Base.empty() ? nullptr : Find(A->Base)) bMine = A->CppName == Owner->second;
        if (!bMine)
        { *Err = "a delegate on `this` cannot bind " + Owner->second + "::" + Name(Ref) + ": the engine looks it up by name on this "
                 "object, whose class has no such function"; return false; }
    }
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
    C.bOnArg0 = true;
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

    /* The signature function is the declaring class's: this one's own, or a Blueprint parent's, imported from its
       package as the editor's broadcast node names it. */
    const std::string SigName = Name(Obj) + "__DelegateSignature";
    const FRecord* Declarer = nullptr;
    for (const FRecord* A = Cur; A && !Declarer; A = A->Base.empty() ? nullptr : Find(A->Base))
        if (A->Methods.count(SigName)) Declarer = A;
    auto Sig = CurSignatures.find(C.Args[0].S);
    if (Declarer && Declarer != Cur && (!Declarer->IsNative() || Declarer->UePackage.compare(0, 6, "/Game/") == 0))
        C.Fn = BP.EngineFunction(PackageOf(*Declarer), ClassOf(*Declarer), C.Args[0].S + "__DelegateSignature");
    else if (C.Args[0].Owner.V == BP.ClassIndex().V && Sig != CurSignatures.end())
        C.Fn = Sig->second;
    else
    {
        *Err = "TODO: Broadcast needs the dispatcher's signature function, which only a UE_DISPATCHER of this class or a "
               "Blueprint parent has: " + C.Args[0].S;
        return false;
    }
    C.Intrinsic = "__Broadcast__";
    for (const Json* A : Args)
    {
        C.Args.emplace_back();
        if (!LowerArg(*A, BP, C.Args.back(), Err)) return false;
    }
    /* A reference parameter of the signature is an out parm, which execCallMulticastDelegate steps with a null result
       pointer and copies from the address the argument left: HoistCallArgs gives anything but a variable a local. */
    if (auto M = Declarer ? Declarer->Methods.find(SigName) : Cur->Methods.end(); Declarer && M != Declarer->Methods.end())
    {
        C.RefParms.emplace_back();
        std::string WrittenRef;
        ForEach(*M->second, [&](const Json& P) {
            if (Kind(P) != "ParmVarDecl") return;
            std::string T = TypeOf(P);
            const bool bRef = !T.empty() && T.back() == '&';
            if (bRef && T.compare(0, 6, "const ") != 0 && WrittenRef.empty()) WrittenRef = Name(P);
            while (!T.empty() && (T.back() == '&' || T.back() == ' ')) T.pop_back();
            C.RefParms.push_back(bRef ? StripTypeKeywords(T) : std::string());
        });
        if (C.RefParms.size() != C.Args.size()) C.RefParms.clear();
        /* C++'s Broadcast copies a reference parameter back after the handlers ran; execCallMulticastDelegate copies the
           argument into a parameter block of its own and never back (ScriptCore.cpp 3032-3075). */
        if (!WrittenRef.empty())
        { *Err = Name(Obj) + ".Broadcast: " + WrittenRef + " is a non-const reference, which a Broadcast never writes back "
                 "to the caller; take it by value or by const reference"; return false; }
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

/* The map element a place stands on: `Map[Key]` itself, or a member chain on it (`Map[Key].A.B`). Map_Find copies
   the value out, so a store or a reference through it would change the copy. */
const Json* MapElementUnder(const Json& N)
{
    const Json* P = PeelLvalue(&N);
    while (P && Kind(*P) == "MemberExpr" && !P->value("isArrow", false) && First(*P)) P = PeelLvalue(First(*P));
    return P && IsTMapElement(*P) ? P : nullptr;
}

/* The container library functions that leave their container as it was. */
bool IsContainerRead(const std::string& Method)
{
    static const std::set<std::string> Reads = { "Length", "LastIndex", "IsValidIndex", "Contains", "Find", "Get",
                                                 "Keys", "Values", "ToArray", "Identical", "Difference",
                                                 "Intersection", "Union" };
    return Reads.count(Method) != 0;
}

/* A TMap or TSet anywhere in a property, a nested one's wrapper included: the engine replicates neither. */
bool HoldsMapOrSet(const FPropertyDef& P)
{
    if (P.Type == "MapProperty" || P.Type == "SetProperty") return true;
    if (P.Type == "StructProperty" && P.StructName.compare(0, 4, "FNC_") == 0
        && (P.StructName.find("TMap") != std::string::npos || P.StructName.find("TSet") != std::string::npos)) return true;
    return P.Inner && HoldsMapOrSet(*P.Inner);
}

/* A variable, a member of one, or a member of this: what `T& R` can be another name for. A structured binding names
   a place as well (RefAlias / RefPlace). */
bool IsAliasable(const Json& N)
{
    if (Kind(N) == "DeclRefExpr")
    {
        const std::string K = N["referencedDecl"].value("kind", std::string());
        return K == "VarDecl" || K == "ParmVarDecl" || K == "BindingDecl";
    }
    if (Kind(N) != "MemberExpr" || !First(N)) return false;
    const Json* Base = PeelLvalue(First(N));
    if (Kind(*Base) == "CXXThisExpr") return true;
    return !N.value("isArrow", false) && IsAliasable(*Base);
}

/* A place a reference can name once what locates it is fixed (StabilizeLvalue): a member of an object (`O->A`), an
   element of an array (`Arr[F()]`), a member of either (`O->S.X`), down from a variable or an object. */
bool IsPinnable(const Json& N)
{
    const Json* P = PeelLvalue(&N);
    if (Kind(*P) == "MemberExpr" && First(*P))
        return P->value("isArrow", false) || IsAliasable(*PeelLvalue(First(*P))) || IsPinnable(*First(*P));
    if (IsTArrayElement(*P) && Nth(*P, 2))
        return IsAliasable(*PeelLvalue(Nth(*P, 1))) || IsPinnable(*Nth(*P, 1));
    return false;
}

/* A reference the callee may write: `T&`, not `const T&` or `T&&`. */
bool IsMutableRef(const std::string& T)
{
    return T.size() > 1 && T.back() == '&' && T[T.size() - 2] != '&' && T.compare(0, 6, "const ") != 0;
}

/* A UFunction's `T&` bound to what Blueprint has no reference to: a map element or a member of one (Map_Find copies
   it out), or whichever of two variables `C ? X : Y` picks. The call gets a copy, stored back after it (LowerCopyBack). */
bool IsUnreferenceable(const Json& Parm, const Json& Arg)
{
    const Json* Bare = PeelLvalue(&Arg);
    return IsMutableRef(TypeOf(Parm)) && Bare && (MapElementUnder(*Bare) || Kind(*Bare) == "ConditionalOperator");
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

/* The arguments a Kismet container function writes, as FCallIR::WrittenArgs (the container is argument 0): the
   container of a mutator, each out parameter, and a same-typed input it reads in place while writing (Append's source,
   Union's sets: `A.Append(A)` would read what it grows). Any other function may write all of them. */
uint64 ContainerWrites(const std::string& Fn)
{
    static const std::map<std::string, uint64> Writes = {
        { "Array_Add", 1 }, { "Array_AddUnique", 1 }, { "Array_Append", 3 }, { "Array_Clear", 1 }, { "Array_Contains", 0 },
        { "Array_Find", 0 }, { "Array_Get", 4 }, { "Array_Identical", 0 }, { "Array_Insert", 1 }, { "Array_IsValidIndex", 0 },
        { "Array_LastIndex", 0 }, { "Array_Length", 0 }, { "Array_Random", 6 }, { "Array_RandomFromStream", 14 },
        { "Array_Remove", 1 }, { "Array_RemoveItem", 1 }, { "Array_Resize", 1 }, { "Array_Reverse", 1 }, { "Array_Set", 1 },
        { "Array_Shuffle", 1 }, { "Array_Swap", 1 },
        { "Set_Add", 1 }, { "Set_AddItems", 1 }, { "Set_Clear", 1 }, { "Set_Contains", 0 }, { "Set_Difference", 7 },
        { "Set_Intersection", 7 }, { "Set_Length", 0 }, { "Set_Remove", 1 }, { "Set_RemoveItems", 1 }, { "Set_ToArray", 2 },
        { "Set_Union", 7 },
        { "Map_Add", 1 }, { "Map_Clear", 1 }, { "Map_Contains", 0 }, { "Map_Find", 4 }, { "Map_Keys", 2 }, { "Map_Length", 0 },
        { "Map_Remove", 1 }, { "Map_Values", 2 },
    };
    auto W = Writes.find(Fn);
    return W == Writes.end() ? ~uint64(0) : W->second;
}

bool IsStored(const FArgIR& A)
{
    return A.K == FArgIR::Local || A.K == FArgIR::LocalOut || A.K == FArgIR::Field || A.K == FArgIR::Member
        || A.K == FArgIR::Index || (A.K == FArgIR::Call && A.Sub && A.Sub->Intrinsic == "__RefAtInline__");
}

/* A container operation's variable: a property, or the local an __Inline__ block leaves its value in (a Make Array, an
   inline call's result), which HoistReadsInArg puts in its place once the block has run. */
bool IsContainerVariable(const FArgIR& A)
{
    return A.K == FArgIR::Field || A.K == FArgIR::Local || A.K == FArgIR::LocalOut || A.K == FArgIR::Member
        || (A.K == FArgIR::Call && A.Sub && A.Sub->Intrinsic == "__Inline__" && !A.Sub->InlineResult.empty());
}

/* An operand whose opcode writes its value and steps nothing that could leave Stack.MostRecentPropertyAddress. */
bool IsVmConstant(const FArgIR& A)
{
    switch (A.K)
    {
    case FArgIR::Self: case FArgIR::Int: case FArgIR::Int64: case FArgIR::Float: case FArgIR::Bool: case FArgIR::Byte:
    case FArgIR::Str: case FArgIR::Name: case FArgIR::Text: case FArgIR::NullObj: case FArgIR::ObjConst:
    case FArgIR::SoftPath: case FArgIR::Delegate:
        return true;
    case FArgIR::StructLit:     // execStructConst steps each member into the struct: constants only
        return A.Sub && std::all_of(A.Sub->Args.begin(), A.Sub->Args.end(), [](const FArgIR& M) { return IsVmConstant(M); });
    default:
        return false;
    }
}

const Json* FCompiler::InlineListOf(const Json* E) const
{
    const Json* Named = Strip(E);
    const Json* Base = Named && Kind(*Named) == "MemberExpr" ? Strip(First(*Named)) : nullptr;
    const std::string Id = !Named ? "" : Kind(*Named) == "DeclRefExpr" ? (*Named)["referencedDecl"].value("id", std::string())
                         : Base && Kind(*Base) == "CXXThisExpr" ? Named->value("referencedMemberDecl", std::string()) : "";
    const auto Var = ConstVars.find(Id);
    const Json* Items = Var != ConstVars.end() ? First(*Var->second) : nullptr;
    while (Items && Kind(*Items) != "InitListExpr") Items = First(*Items);
    return Items;
}

bool FCompiler::InlineConsts(const Json& Items, const std::string& Elem, FBlueprintClass& BP, std::vector<FArgIR>* Out)
{
    bool bOk = true;
    ForEach(Items, [&](const Json& E) {
        FArgIR& A = Out->emplace_back();
        FConstVal V;
        std::string Ignored;
        bOk = bOk && ((FoldConst(E, V) && ConstToArg(V, Elem, A)) || (LowerArg(E, BP, A, &Ignored) && IsVmConstant(A)));
    });
    return bOk && !Out->empty();
}

bool FCompiler::ExactEqual(const std::string& Type, FArgIR A, FArgIR B, FBlueprintClass& BP, FArgIR& Out) const
{
    const std::string T = Canon(Type);
    Out = FArgIR();
    Out.K = FArgIR::Call;
    Out.InnerType = "bool";
    Out.Sub = std::make_shared<FCallIR>();
    Out.Sub->bPure = true;
    Out.Sub->Args = { std::move(A), std::move(B) };
    if (T == "FName" || T == "FString" || T.compare(0, 5, "TSoft") == 0)
    {
        const FOpInfo* O = FindOp("==", T, T);
        if (!O) return false;
        Out.Sub->Fn = BP.EngineFunction(O->Package, O->Class, O->Fn);
        Out.Sub->RefParms.resize(2);
        for (const size_t At : O->Refs)
            if (At < 2) Out.Sub->RefParms[At] = At == 0 ? O->Lhs : O->Rhs;
        Out.Sub->bRefsTakeConst = true;
        return true;
    }
    const char* Flavour = IsObjectType(T) ? "ObjectObject" : T == "float" ? "FloatFloat" : T == "int64" ? "Int64Int64"
                        : T == "uint8" ? "ByteByte" : T == "bool" ? "BoolBool" : T == "int" ? "IntInt" : nullptr;
    if (!Flavour) return false;
    Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", std::string("EqualEqual_") + Flavour);
    Out.Sub->bScript = false;
    return true;
}

/* `kKinds.Contains(X)` over an inline list of constants makes no array: X once, into a local, then R = X == A,
   R = R || X == B, ... one statement each, so a long list nests nothing. BooleanOR runs every comparison, which is
   harmless: each compares a local with a constant. */
bool FCompiler::LowerInlineContains(const Json& Item, const std::string& Elem, const std::vector<FArgIR>& Consts,
                                    FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    if (!CurLocals) { *Err = "internal: Contains outside a function body"; return false; }
    const std::string N = std::to_string(ReadTmpCounter++), X = "__ContainsItem" + N + "__", R = "__Contains" + N + "__";
    for (const auto& [Name, Type] : { std::pair<std::string, std::string>{ X, Elem }, { R, "bool" } })
    {
        FPropertyDef PD;
        if (!TypeToProperty(Type, Name, 0, "Contains", BP, &PD, Err)) return false;
        PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
        CurLocals->push_back(PD);
    }
    auto Local = [](const std::string& Name) { FArgIR A; A.K = FArgIR::Local; A.S = Name; return A; };
    auto Assign = [&](const std::string& Name, const std::string& Type, FArgIR Value) {
        FStmtIR St;
        St.K = FStmtIR::Assign;
        St.Var = Local(Name);
        St.Var.LetOp = LetOpFor(Type);
        St.bAssignLocal = true;
        St.Value = std::move(Value);
        return St;
    };
    FArgIR Value;
    if (!LowerArg(Item, BP, Value, Err)) return false;
    auto Body = std::make_shared<std::vector<FStmtIR>>();
    Body->push_back(Assign(X, Elem, std::move(Value)));
    for (size_t I = 0; I < Consts.size(); ++I)
    {
        FArgIR Eq;
        if (!ExactEqual(Elem, Local(X), Consts[I], BP, Eq)) { *Err = "internal: no exact == for " + Elem; return false; }
        if (I > 0)
        {
            FArgIR Or;
            Or.K = FArgIR::Call;
            Or.InnerType = "bool";
            Or.Sub = std::make_shared<FCallIR>();
            Or.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", "BooleanOR");
            Or.Sub->bScript = false;
            Or.Sub->bPure = true;
            Or.Sub->Args = { Local(R), std::move(Eq) };
            Eq = std::move(Or);
        }
        Body->push_back(Assign(R, "bool", std::move(Eq)));
    }

    auto Block = std::make_shared<std::vector<FStmtIR>>(1);
    (*Block)[0].K = FStmtIR::Block;
    (*Block)[0].Body = Body;
    Out.K = FArgIR::Call;
    Out.InnerType = "bool";
    Out.Sub = std::make_shared<FCallIR>();
    Out.Sub->Intrinsic = "__Inline__";
    Out.Sub->Inline = Block;
    Out.Sub->InlineResult = R;
    Out.Sub->InlineType = "bool";
    return true;
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

/* The TArray-shaped scratch every read through a pointer assembles into (see include/Intrin.h).
   Its layout is fixed by the engine's TArray, not by the mod, and a mod reaches a deref without
   ever asking for one - `Obj->GetOuter()` lowers to a read of OuterPrivate - so requiring the
   source to declare it was a copy-paste tax with one correct answer. A mod that declares its own
   FDeref still wins: Find sees it first and nothing is synthesized. */
bool FCompiler::HasDerefStruct(std::string* Err) const
{
    (void)Err;
    const FRecord* D = Find("FDeref");
    if (D && D->bIsStruct && !D->IsNative()) return true;
    if (bSynthDeref) return true;

    DerefAst = Json::array();
    for (const auto& [Field, Type] : { std::pair<const char*, const char*>{ "Data", "int64" },
                                       { "Num", "int32" }, { "Max", "int32" } })
        DerefAst.push_back(Json{ { "kind", "FieldDecl" }, { "name", Field },
                                 { "type", Json{ { "qualType", Type } } } });

    SynthDeref = FRecord();
    SynthDeref.CppName   = "FDeref";
    SynthDeref.UeName    = "FDeref";
    SynthDeref.UePackage = ModPackage + "/FDeref";
    SynthDeref.bIsStruct = true;
    SynthDeref.bIsLocal  = true;
    for (const Json& F : DerefAst) SynthDeref.Fields.push_back(&F);
    bSynthDeref = true;
    return true;
}

/* FScriptMap starts with its sparse array's element array, and an element is TSetElement<TPair<K, V>>: the pair, then
   HashNextId and HashIndex. The same members laid out by UStruct::Link give the same offsets and stride, while no member
   is aligned past 8 (FScriptSetLayout aligns the hash links past the whole pair). A container value is its own type
   here, not the wrapper struct the map's value property is: the wrapper has the container's layout. */
bool FCompiler::MapSlotView(const std::string& MapType, const std::string& Key, const std::string& Value, FBlueprintClass& BP,
                            FIndex* View, FIndex* Slot, std::string* Err)
{
    const std::string Tag = TypeTag(MapType), SlotName = "FMapSlot_" + Tag, ViewName = "FMapSlots_" + Tag;
    if (!SlotStructs.count(SlotName))
    {
        auto Field = [](const char* Name, const std::string& Type)
        { return Json{ { "kind", "FieldDecl" }, { "name", Name }, { "type", Json{ { "qualType", Type } } } }; };
        SlotAst[SlotName] = Json::array({ Field("Key", Key), Field("Value", Value), Field("HashNextId", "int32"),
                                          Field("HashIndex", "int32") });
        SlotAst[ViewName] = Json::array({ Field("__Slots__", "TArray<" + SlotName + ">") });
        for (const std::string& Name : { SlotName, ViewName })
        {
            FRecord& R = SlotStructs[Name];
            R.CppName = R.UeName = Name;
            R.UePackage = ModPackage + "/" + Name;
            R.bIsStruct = R.bIsLocal = true;
            for (const Json& F : SlotAst[Name]) R.Fields.push_back(&F);
        }
    }
    int32 Size = 0, Align = 0;
    if (!StructLayout(SlotStructs[SlotName], &Size, &Align, Err)) return false;
    if (Align > 8) { *Err = "internal: a TMap walked in place with an element aligned past 8 bytes"; return false; }
    *Slot = BP.ScriptStruct(ModPackage + "/" + SlotName, SlotName);
    *View = BP.ScriptStruct(ModPackage + "/" + ViewName, ViewName);
    KeepStructLoaded(*Slot, SlotName, Size);
    KeepStructLoaded(*View, ViewName, 16);
    return true;
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
    const std::string P = StripTypeKeywords(Pointee);
    /* The int32 view would read it signed: 0xFFFFFFFF would widen and compare as -1. */
    if (P == "uint32" || P == "unsigned int")
    { *Err = "TODO: reading a uint32 through a pointer (Kismet has no unsigned 32-bit int); read it as int32 or int64"; return false; }
    Out = FArgIR();
    Out.K = FArgIR::Call;
    Out.InnerType = V->ResultType;
    Out.Sub = std::make_shared<FCallIR>();
    Out.Sub->Intrinsic = V->Intrinsic;
    Out.Sub->Args.push_back(std::move(Addr));
    if (P == "int8" || P == "signed char" || P == "char")
    {
        /* The byte view reads unsigned; (B ^ 0x80) - 0x80 is the sign-extended int C++ promotes the byte to. */
        WrapInCall(Out, BP.EngineFunction("/Script/Engine", "KismetMathLibrary", "Conv_ByteToInt"));
        for (const char* Fn : { "Xor_IntInt", "Subtract_IntInt" })
        {
            WrapInCall(Out, BP.EngineFunction("/Script/Engine", "KismetMathLibrary", Fn));
            FArgIR Bias;
            Bias.K = FArgIR::Int;
            Bias.I = 0x80;
            Out.Sub->Args.push_back(Bias);
        }
        Out.InnerType = "int32";
        return true;
    }
    if (P != "bool") return true;
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
        const std::string Member = N->value("referencedMemberDecl", std::string());
        if (ConstVars.count(Member) || MutableStatics.count(Member))
        { *Err = "a static variable has no address: " + N->value("name", std::string()) + " is inline, each use its initializer"; return false; }
        const Json* Base = Nth(*N, 0);
        std::string ObjType = Base ? StripTypeKeywords(TypeOf(*Base)) : std::string();
        while (!ObjType.empty() && (ObjType.back() == '*' || ObjType.back() == ' ')) ObjType.pop_back();
        const FRecord* Owner = Find(ObjType);
        if (!Owner || Owner->bIsStruct) { *Err = "TODO: the address of a member of " + ObjType + ", which is not an object class"; return false; }
        const FRecord* Lib = Find("ReadProperty");
        if (!Lib) { *Err = "`&Obj->Member` finds the member at run time through ReadProperty::GetPropertyAddress: include ReadProperty.h"; return false; }
        FArgIR Obj;
        if (!LowerArg(*Base, BP, Obj, Err)) return false;
        const std::string Pkg = PackageOf(*Lib), Cls = ClassOf(*Lib);
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
                             std::vector<FStmtIR>& OutPre, std::string* Err, bool bAlways)
{
    if (!bAlways && IsStored(Operand)) return true;
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
    /* Strip peels every cast, but one that changes the value - to bool, to a narrower integer, float to integer - must
       still happen: `(uint8)V + X` wraps V, `(bool)F + X` adds 0 or 1. The value is lowered as that cast's type, and
       only then converted to the slot's. Over constants FoldConst runs the whole chain. */
    if (const Json* Cut = ValueCastBelow(ArgNode))
    {
        if (FConstVal V; FoldConst(ArgNode, V) && ConstToArg(V, OuterType, Out)) return true;
        return LowerArg(*Cut, BP, Out, Err) && ConvertArg(OuterType, BP, Out, Err);
    }
    const Json* N = Strip(&ArgNode);
    if (!N) { *Err = "empty argument expression"; return false; }
    /* Arithmetic, a comparison or logic over constants is the constant it comes to: no Kismet call left to run. */
    if (const std::string K = Kind(*N); !bCurNoOpt && (K == "BinaryOperator" || K == "ConditionalOperator"
        || (K == "UnaryOperator" && N->value("opcode", std::string()).find_first_of("&*+") == std::string::npos)))
        if (FConstVal V; FoldConst(ArgNode, V) && ConstToArg(V, OuterType, Out)) return true;
    if (!LowerArgRaw(*N, OuterType, BP, Out, Err)) return false;
    if (Out.InnerType.empty()) Out.InnerType = TypeOf(*N);
    /* Strip looked through the casts, but an explicit narrowing inside a wider slot (`(uint8)*P + 1`
       of a sign-extended byte, `(uint8)X` returned as int) still wraps, innermost first. So does a soft pointer made on
       the way: `FString(TSoftClassPtr<AItem>(Cls))` is the class's path, where Cls to FString would be its name. */
    std::vector<std::string> Narrowings;
    for (const Json* W = &ArgNode; W && W != N; W = First(*W))
        if (const std::string K = Kind(*W); K == "CStyleCastExpr" || K == "CXXStaticCastExpr" || K == "CXXFunctionalCastExpr"
            || (K == "CXXConstructExpr" && StripTypeKeywords(TypeOf(*W)).compare(0, 5, "TSoft") == 0))
            Narrowings.push_back(TypeOf(*W));
    for (auto It = Narrowings.rbegin(); It != Narrowings.rend(); ++It)
    {
        const EStrKind From = KindOfLowered(Out, Out.InnerType), To = StrKindOf(Canon(*It));
        const bool bSoft = Canon(*It).compare(0, 5, "TSoft") == 0 && Canon(Out.InnerType).compare(0, 5, "TSoft") != 0;
        if ((bSoft || (To == SK_Byte && (From == SK_Int || From == SK_Int64)) || (To == SK_Int && From == SK_Int64))
            && !ConvertArg(*It, BP, Out, Err))
            return false;
    }
    return ConvertArg(OuterType, BP, Out, Err);
}

bool FCompiler::LowerArgRaw(const Json& Node, const std::string& OuterType, FBlueprintClass& BP,
                            FArgIR& Out, std::string* Err)
{
    const Json* N = &Node;
    const EStrKind Slot = StrKindOf(OuterType);
    const std::string K = Kind(*N);
    if (K == "MemberExpr")
    {
        /* `this->X` of an inline class variable is X: the object says nothing about it, so it must not run anything. */
        if (const auto G = ConstVars.find(N->value("referencedMemberDecl", std::string())); G != ConstVars.end())
        {
            const Json* Obj = Strip(First(*N));
            if (Obj && Kind(*Obj) != "CXXThisExpr" && Kind(*Obj) != "DeclRefExpr")
            { *Err = Name(*G->second) + " is static: name it without the object in front, which would never be evaluated"; return false; }
            return LowerInlineVar(*G->second, BP, Out, Err);
        }
        return LowerField(*N, BP, Out, Err);
    }
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
            if (Canon(TypeOf(*Obj)) == "FName" && Canon(TypeOf(*N)) == "bool")
            {
                /* `if (Name)`: Kismet has no Conv_NameToBool, so it is Name != None. */
                FArgIR Lhs, None;
                if (!LowerArg(*Obj, BP, Lhs, Err)) return false;
                None.K = FArgIR::Name;
                None.S = "None";
                Out.K = FArgIR::Call;
                Out.Sub = std::make_shared<FCallIR>();
                Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", "NotEqual_NameName");
                Out.Sub->bPure = true;
                Out.Sub->Args = { Lhs, None };
                Out.InnerType = "bool";
                return true;
            }
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
            if (Method == "Contains" && Prefix != "Map_" && !bCurNoOpt)
            {
                std::string Elem;
                std::vector<FArgIR> Consts;
                FArgIR Probe;
                const Json* Items = InlineListOf(Obj);
                if (Items && Nth(*N, 1) && TemplateArg(C, Prefix == "Array_" ? "TArray" : "TSet", &Elem)
                    && ExactEqual(Elem, {}, {}, BP, Probe) && InlineConsts(*Items, Elem, BP, &Consts))
                    return LowerInlineContains(*Nth(*N, 1), Elem, Consts, BP, Out, Err);
            }
            /* `Map[K].Add(X)`: Blueprint has no reference to a map element, so the call runs on a copy. */
            if (MapElementUnder(Unalias(*Obj)))
            {
                Out.K = FArgIR::Call;
                Out.Sub = std::make_shared<FCallIR>();
                return LowerCopyBack(*N, { { First(*Callee), "" } }, Method, BP, *Out.Sub, Err);
            }
            FArgIR Target;
            if (!LowerArg(*Obj, BP, Target, Err)) return false;
            if (!IsContainerVariable(Target))
            { *Err = "a container operation needs a variable, not a computed value: " + Method; return false; }
            if (!IsContainerRead(Method)) WarnRpcRefWrite(Target);
            Out.K = FArgIR::Call;
            Out.Sub = std::make_shared<FCallIR>();
            Out.Sub->Fn = BP.EngineFunction("/Script/Engine", Lib, Prefix + Method);
            Out.Sub->WrittenArgs = ContainerWrites(Prefix + Method);
            Out.Sub->bOnArg0 = true;
            Out.Sub->Args.push_back(Target);
            Out.Sub->RefParms.emplace_back();
            bool bFirst = true, bOk = true;
            std::string LastType;
            ForEach(*N, [&](const Json& A) {
                if (bFirst) { bFirst = false; return; }
                if (!bOk) return;
                FArgIR V;
                bOk = LowerArg(A, BP, V, Err);
                if (bOk) Out.Sub->Args.push_back(V);
                LastType = TypeOf(A);
                /* A container argument (Append's source, Union's sets) is stepped with no result buffer and read where
                   it lies (execArray_Append), so one a call computes needs a local; an item goes into the thunk's own
                   storage (StepCompiledIn<FProperty>(StorageSpace)), whatever evaluates it. */
                std::string T = LastType;
                while (!T.empty() && (T.back() == '&' || T.back() == ' ')) T.pop_back();
                Out.Sub->RefParms.push_back(IsContainerType(T) ? StripTypeKeywords(T) : std::string());
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
        if (Kind(*Obj) == "CXXThisExpr" || Out.Sub->bReceiverIsArg) return true;
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
            if (!IsContainerVariable(Map))
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
                Out.Sub->WrittenArgs = ContainerWrites("Map_Find");
                Out.Sub->bOnArg0 = true;
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
            (*Body)[0].Call.WrittenArgs = ContainerWrites("Map_Find");
            (*Body)[0].Call.bOnArg0 = true;
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
            if (!IsContainerVariable(*Out.Base))
            { *Err = "indexing needs an array variable, not a computed value"; return false; }
            Out.Sub = std::make_shared<FCallIR>();
            FArgIR Idx;
            if (!LowerArg(*Rhs, BP, Idx, Err)) return false;
            Out.Sub->Args.push_back(Idx);
            std::string Elem = TypeOf(*N);
            while (!Elem.empty() && (Elem.back() == '&' || Elem.back() == ' ')) Elem.pop_back();
            Out.S = Out.Base->K == FArgIR::Call ? Out.Base->Sub->InlineResult : Out.Base->S;
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
                Out.Sub->RefParms.resize(Out.Sub->Args.size());
                for (const size_t At : O->Refs)
                    if (At < 2) Out.Sub->RefParms[At] = At == 0 ? O->Lhs : O->Rhs;
                Out.Sub->bRefsTakeConst = true;
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
        the EnumConstantDecl path below does the same. A uint64 literal past int64 keeps its bits, as the cast to
        a Blueprint type (none is unsigned 64) does in C++: (int64)18446744073709551615ULL is -1.
        */
        const int64 V = int64(std::strtoull(N->value("value", std::string("0")).c_str(), nullptr, 10));
        Out.I = int32(V);
        Out.I64 = V;
        Out.K = int64(Out.I) == V ? FArgIR::Int : FArgIR::Int64;
        return true;
    }
    if (K == "CharacterLiteral") { Out.K = FArgIR::Int; Out.I = int32(CharValue(*N)); Out.I64 = Out.I; return true; }
    /* strtof, not stof: a double literal past float's range is inf, as (float)1e39 is in C++, not an exception. */
    if (K == "FloatingLiteral") { Out.K = FArgIR::Float; Out.F = std::strtof(N->value("value", std::string("0")).c_str(), nullptr); return true; }
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
        /* A reference local (or a range-for binding) is the variable it names, the place it walks, or the value at the
           address it keeps. */
        if (auto A = RefAlias.find(Ref.value("id", std::string())); A != RefAlias.end()) return LowerArg(A->second, BP, Out, Err);
        if (auto P = RefPlace.find(Ref.value("id", std::string())); P != RefPlace.end()) { Out = P->second; return true; }
        if (auto C = ParmConst.find(Ref.value("id", std::string())); C != ParmConst.end()) { Out = C->second; return true; }
        if (auto G = ConstVars.find(Ref.value("id", std::string())); G != ConstVars.end())
            return LowerInlineVar(*G->second, BP, Out, Err);
        if (RefKind != "ParmVarDecl" && RefKind != "VarDecl")
        {
            *Err = "TODO: DeclRefExpr to " + RefKind;
            return false;
        }
        if (const std::string Why = StaticRefusal(Ref.value("id", std::string()), false); !Why.empty()) { *Err = Why; return false; }
        if (auto G = NsVars.find(Ref.value("id", std::string())); G != NsVars.end()) return LowerGlobal(*G->second, BP, Out, Err);
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
            /* StaticClass is declared once, by UE_CLASS on a native class, so the decl names AActor for every mod
               actor that inherits it. The class meant is the qualifier (`AMine::StaticClass()`), else the class a
               TSubclassOf<X> slot around it asks for (NewObject<T>'s default argument), when that is more derived. */
            auto Owner = MethodOwner.find((*CalleeNode)["referencedDecl"].value("id", std::string()));
            const FRecord* R = Owner == MethodOwner.end() ? nullptr : Find(Owner->second);
            std::string Slot;
            if (const FRecord* Q = NamedQualifier(*CalleeNode)) R = Q;
            else if (TemplateArg(StripTypeKeywords(OuterType), "TSubclassOf", &Slot))
                if (const FRecord* S = Find(StripTypeKeywords(Slot)); S && R && S != R && IsSubclassOf(*S, *R)) R = S;
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
            if (R->bIsInterface || (R->IsNative() && R->CppName[0] == 'I'))
            {
                /* A cast to an interface writes a 16-byte FScriptInterface (ScriptCore.cpp 3605-3645), and the `IFoo*` it
                   gives is the object: EX_InterfaceToObjCast takes that out, where an 8-byte slot would take all 16. */
                FArgIR Iface = std::move(Out);
                Out = FArgIR();
                Out.K = FArgIR::DynCast;
                Out.CastOp = EX_InterfaceToObjCast;
                Out.Owner = BP.EngineClass("/Script/CoreUObject", "Object");
                Out.Sub = std::make_shared<FCallIR>();
                Out.Sub->Args.push_back(std::move(Iface));
            }
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

            const std::string CalleePkg = PackageOf(*Cur), CalleeCls = ClassOf(*Cur);
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
        if (Op == "++" || Op == "--") return LowerUpdateValue(*N, BP, Out, Err);
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
            /* -0.0 - F is -F for every F; 0.0 - F would make -(+0.0) +0.0. */
            FArgIR Zero;
            Zero.K = bFloat ? FArgIR::Float : bInt64 ? FArgIR::Int64 : FArgIR::Int;
            Zero.F = -0.0f;
            Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary",
                                            bFloat ? "Subtract_FloatFloat" : bInt64 ? "Subtract_Int64Int64" : "Subtract_IntInt");
            Out.Sub->Args = { Zero, A };
            return true;
        }
        if (Op != "!") { *Err = "TODO: unimplemented unary operator " + Op; return false; }
        /* `!(A < B)` is `A >= B` and `!!X` is X: no Not_PreBool call. */
        if (Json Neg; !bCurNoOpt && FreeNegation(*Operand, Neg)) return LowerArg(Neg, BP, Out, Err);

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
        /* `A | B`, `A & B`, `A ^ B` over bools: C++ promotes both sides to int and the result back, four native calls
           (Conv_BoolToInt twice, Or_IntInt, Conv_IntToBool). BooleanOR / BooleanAND / BooleanXOR is one, and runs both
           sides as `|` does. A constant side decides it or drops out: `X | true` is true when X only reads. */
        auto BoolSide = [this](const Json* S) -> const Json* {
            const Json* In = S && Kind(*S) == "ImplicitCastExpr" && S->value("castKind", std::string()) == "IntegralCast" ? First(*S) : nullptr;
            return In && Canon(TypeOf(*In)) == "bool" ? In : nullptr;
        };
        if (const Json *LB = BoolSide(LhsRaw), *RB = BoolSide(RhsRaw); !bCurNoOpt && LB && RB && (Op == "|" || Op == "&" || Op == "^"))
        {
            FArgIR L, R;
            if (!LowerArg(*LB, BP, L, Err) || !LowerArg(*RB, BP, R, Err)) return false;
            if (L.K == FArgIR::Bool) std::swap(L, R);
            const FArgIR* Decided = R.K != FArgIR::Bool ? nullptr
                                  : (Op == "|" && !R.B) || (Op == "&" && R.B) || (Op == "^" && !R.B) ? &L
                                  : Op != "^" && !CallsImpure(L) ? &R : nullptr;
            if (Decided) { Out = *Decided; Out.InnerType = "bool"; return true; }
            Out = FArgIR();
            Out.K = FArgIR::Call;
            Out.InnerType = "bool";
            Out.Sub = std::make_shared<FCallIR>();
            Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary",
                                            Op == "|" ? "BooleanOR" : Op == "&" ? "BooleanAND" : "BooleanXOR");
            Out.Sub->bPure = true;
            Out.Sub->Args = { std::move(L), std::move(R) };
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
        else if (IsInt64Type(LhsC) || IsInt64Type(RhsC))     Flavour = "Int64Int64";   // Canon: an int64-backed enum too
        else if (LhsC == "uint8" && RhsC == "uint8")         Flavour = "ByteByte";
        else if (LhsC == "bool" && RhsC == "bool")           Flavour = "BoolBool";
        else                                                 Flavour = "IntInt";
        if (Op == "<<" || Op == ">>")
        {
            if (Kind(*RhsRaw) != "IntegerLiteral")
            { *Err = "bit shift with a non-constant amount (Kismet has no shift op; use explicit multiply/divide)"; return false; }
            /* A shift has the promoted LHS type (no usual arithmetic conversions): `int >> 1LL` is int32 math. */
            const bool bWide = IsInt64Type(LhsC);
            const int N = std::stoi(RhsRaw->value("value", std::string("0")));
            if (N < 0 || N > (bWide ? 63 : 31))
            { *Err = "bit shift amount out of range: " + std::to_string(N); return false; }
            const std::string Suffix = bWide ? "Int64Int64" : "IntInt";
            auto Const = [&](int64 V) {
                FArgIR C;
                C.K = bWide ? FArgIR::Int64 : FArgIR::Int;
                C.I = int32(V);
                C.I64 = V;
                return C;
            };
            auto Math = [&](const std::string& Fn, FArgIR A, FArgIR B) {
                FArgIR C;
                C.K = FArgIR::Call;
                C.Sub = std::make_shared<FCallIR>();
                C.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", Fn + Suffix);
                C.Sub->bScript = false;
                C.Sub->bPure = true;
                C.Sub->Args = { std::move(A), std::move(B) };
                return C;
            };
            FArgIR LA;
            if (!LowerArg(*LhsRaw, BP, LA, Err)) return false;
            const std::string LhsBare = StripTypeKeywords(LhsTy);
            const bool bSigned = LhsBare.find("unsigned") == std::string::npos && LhsBare.compare(0, 4, "uint") != 0;
            if (Op == ">>" && bSigned && N > 0)
            {
                /* Signed >> floors, Divide truncates toward zero (-3 >> 1 is -2, -3 / 2 is -1): clearing the low
                   N bits first makes the division exact. At N = width-1 the divisor wraps to MIN, which negates. */
                Out = Math("Divide_", Math("And_", std::move(LA), Const(~((1LL << N) - 1))), Const(1LL << N));
                if (N == (bWide ? 63 : 31)) Out = Math("Multiply_", std::move(Out), Const(-1));
                return true;
            }
            Out = Math(Op == "<<" ? "Multiply_" : "Divide_", std::move(LA), Const(1LL << N));
            return true;
        }
        const std::string MathFn = MathFuncFor(Op, Flavour);
        if (MathFn.empty())
        { *Err = "TODO: unimplemented binary operator " + Op + " on " + Flavour; return false; }
        /* `X + 0`, `X * 1` and the like are X. A float keeps its + and - (-0.0f + 0 is +0.0f). */
        if (!bCurNoOpt && (Flavour == "IntInt" || Flavour == "Int64Int64" || Flavour == "FloatFloat"))
        {
            const bool bInt = Flavour != "FloatFloat";
            auto Is = [&](const Json* Side, double Want) {
                FConstVal V;
                return FoldConst(*Side, V) && V.Num() == Want;
            };
            const Json* Kept = (Op == "*" && Is(RhsRaw, 1)) || (Op == "/" && Is(RhsRaw, 1)) ? LhsRaw
                             : Op == "*" && Is(LhsRaw, 1) ? RhsRaw
                             : bInt && (Op == "+" || Op == "-" || Op == "|" || Op == "^") && Is(RhsRaw, 0) ? LhsRaw
                             : bInt && (Op == "+" || Op == "|" || Op == "^") && Is(LhsRaw, 0) ? RhsRaw : nullptr;
            if (Kept)
            {
                if (!LowerArg(*Kept, BP, Out, Err)) return false;
                Out.InnerType = TypeOf(*N);
                return true;
            }
        }

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
    /* A container's braced list: Strip took the constructor it is the one argument of, whose type is the slot's. */
    if (K == "CXXStdInitializerListExpr") return LowerContainerLiteral(*N, OuterType, BP, Out, Err);
    if (K == "CompoundAssignOperator") return LowerUpdateValue(*N, BP, Out, Err);

    *Err = "TODO: unimplemented argument " + K;
    return false;
}

bool FCompiler::IsSubclassOf(const FRecord& Child, const FRecord& Parent) const
{
    for (const FRecord* A = &Child; A; A = A->Base.empty() ? nullptr : Find(A->Base))
        if (A == &Parent) return true;
    return false;
}

/* What a component class's native Serialize reads after UObject's part, for an archetype with no instance data: each
   class of the chain reads its own after its parent's. Every engine component Serialize override was read in the 4.27
   source (2026-09-26); these are the ones that read anything from an unversioned cooked package:
   - UStaticMeshComponent: `Ar << LODData`, an empty array's int32 count. Without it CompTest's Mesh read the next
     export as LODData and failed to load with a fatal error (DRG, 2026-09-25).
   - UInstancedStaticMeshComponent: bCooked, then PerInstanceSMData and PerInstanceSMCustomData as BulkSerialize
     writes them (element size, count: 64 per FMatrix, 4 per float), then, cooked, a uint64 RenderDataSizeBytes the
     cooker leaves 0 on an archetype. The one ISM archetype in DRG's pak ends in exactly these 32 bytes, LODData first.
   - UHierarchicalInstancedStaticMeshComponent: its ClusterTree, BulkSerialize again (64 per FClusterNode).
   - USkyAtmosphereComponent: bStaticLightingBuiltGUID, an FGuid.
   - UAtmosphericFogComponent: three empty bulk-data headers (flags, count, size on disk, offset), then CounterVal.
   Too little or too much is fatal alike: the loader checks each export's size ("Serial size mismatch"). */
std::vector<uint8> FCompiler::NativeTail(const FRecord* Component) const
{
    std::vector<const FRecord*> Chain;
    for (const FRecord* A = Component; A; A = A->Base.empty() ? nullptr : Find(A->Base)) Chain.insert(Chain.begin(), A);
    std::vector<uint8> Tail;
    auto I32 = [&](std::initializer_list<int32> Vs) {
        for (int32 V : Vs) for (int32 Shift = 0; Shift < 32; Shift += 8) Tail.push_back(uint8(V >> Shift));
    };
    for (const FRecord* A : Chain)
    {
        if (A->UeName == "StaticMeshComponent") I32({ 0 });
        else if (A->UeName == "InstancedStaticMeshComponent") I32({ 1, 64, 0, 4, 0, 0, 0 });
        else if (A->UeName == "HierarchicalInstancedStaticMeshComponent") I32({ 64, 0 });
        else if (A->UeName == "SkyAtmosphereComponent") Tail.resize(Tail.size() + 16);
        else if (A->UeName == "AtmosphericFogComponent") Tail.resize(Tail.size() + 3 * 20 + 4);
    }
    return Tail;
}

/* The qualifier token is where a qualified DeclRefExpr's range begins; the JSON gives its byte offset and length but
   not its file (that is only written when it changes, and Json's sorted keys lose the order). So each of the mod's own
   sources is tried at that offset, and a hit counts only when `<Record>::StaticClass` is what is written there.
   In a macro (`#define CLS(X) X::StaticClass()`) the range has spelling locations instead: the qualifier where the
   argument is written, StaticClass in the macro's body, which has to be `::StaticClass` then.
   ponytail: a namespace-qualified `Ns::X::StaticClass()` begins at Ns and falls back to the declaring class; so does
   a macro defined outside the mod's own sources (a UeApi header). */
const FRecord* FCompiler::NamedQualifier(const Json& Ref) const
{
    const Json Range = Ref.value("range", Json::object());
    Json Begin = Range.value("begin", Json::object()), End;
    if (Begin.contains("spellingLoc"))
    {
        End = Range.value("end", Json::object()).value("spellingLoc", Json::object());
        Begin = Begin["spellingLoc"];
        if (!End.contains("offset")) return nullptr;
    }
    if (!Begin.contains("offset") || !Begin.contains("tokLen")) return nullptr;
    const size_t Off = Begin["offset"].get<size_t>(), Len = Begin["tokLen"].get<size_t>();
    if (SourceTexts.empty())
    {
        std::error_code Ec;
        for (const auto& E : std::filesystem::directory_iterator(SourceDir, Ec))
        {
            const std::string Ext = E.path().extension().string();
            if (!E.is_regular_file() || (Ext != ".h" && Ext != ".hpp" && Ext != ".cpp")) continue;
            std::ifstream F(E.path(), std::ios::binary);
            SourceTexts.emplace_back(std::istreambuf_iterator<char>(F), std::istreambuf_iterator<char>());
        }
    }
    if (End.is_null())
    {
        for (const std::string& T : SourceTexts)
        {
            if (Off + Len > T.size()) continue;
            size_t P = Off + Len;
            while (P < T.size() && (T[P] == ' ' || T[P] == '\t')) ++P;
            if (T.compare(P, 2, "::") != 0) continue;
            P += 2;
            while (P < T.size() && (T[P] == ' ' || T[P] == '\t')) ++P;
            if (T.compare(P, 11, "StaticClass") != 0) continue;
            if (const FRecord* R = Find(T.substr(Off, Len))) return R;
        }
        return nullptr;
    }
    /* In a macro: its body has `::StaticClass` at End, and the qualifier is the whole identifier at Off. */
    const size_t At = End["offset"].get<size_t>();
    const bool bBody = std::any_of(SourceTexts.begin(), SourceTexts.end(), [&](const std::string& T) {
        size_t P = At;
        if (P + 11 > T.size() || T.compare(P, 11, "StaticClass") != 0) return false;
        while (P > 0 && (T[P - 1] == ' ' || T[P - 1] == '\t')) --P;
        return P >= 2 && T.compare(P - 2, 2, "::") == 0;
    });
    auto Ident = [](char C) { return std::isalnum(uint8(C)) || C == '_'; };
    for (const std::string& T : SourceTexts)
        if (bBody && Off + Len <= T.size() && (Off == 0 || !Ident(T[Off - 1])) && (Off + Len == T.size() || !Ident(T[Off + Len])))
            if (const FRecord* R = Find(T.substr(Off, Len))) return R;
    return nullptr;
}

/* True when evaluating N twice is the same as once: names, literals, member reads and arithmetic, no call or store. */
static bool IsSideEffectFree(const Json& N)
{
    const std::string K = Kind(N);
    const std::string Op = N.value("opcode", std::string());
    if (K == "BinaryOperator")
    {
        if (Op == "=") return false;
    }
    else if (K == "UnaryOperator")
    {
        if (Op == "++" || Op == "--") return false;
    }
    else if (K != "DeclRefExpr" && K != "CXXThisExpr" && K != "MemberExpr" && K != "ImplicitCastExpr"
          && K != "ParenExpr" && K != "ConstantExpr" && K != "CStyleCastExpr" && K != "CXXStaticCastExpr"
          && K != "IntegerLiteral" && K != "FloatingLiteral" && K != "CXXBoolLiteralExpr"
          && K != "CharacterLiteral" && K != "StringLiteral" && K != "CXXNullPtrLiteralExpr")
        return false;
    bool bFree = true;
    ForEach(N, [&](const Json& C) { bFree = bFree && IsSideEffectFree(C); });
    return bFree;
}

/* True for `this` and literals: nothing the code in between runs can change them. */
static bool IsFixedValue(const Json& N)
{
    const std::string K = Kind(N);
    if (K == "ParenExpr" || K == "ImplicitCastExpr" || K == "CStyleCastExpr" || K == "CXXStaticCastExpr" || K == "ConstantExpr")
        return N.contains("inner") && N["inner"].size() == 1 && IsFixedValue(N["inner"][0]);
    return K == "CXXThisExpr" || K.find("Literal") != std::string::npos;
}

/* IsSideEffectFree, also allowing TArray element reads. */
static bool IsPlainRead(const Json& N)
{
    if (IsTArrayElement(N) && N["inner"].size() == 3)
        return IsPlainRead(N["inner"][1]) && IsPlainRead(N["inner"][2]);
    if (!N.contains("inner")) return IsSideEffectFree(N);
    Json Node = N;
    Node.erase("inner");
    bool bFree = IsSideEffectFree(Node);
    ForEach(N, [&](const Json& C) { bFree = bFree && IsPlainRead(C); });
    return bFree;
}

/* What locates lvalue N (a pointer, an index, a key): bReads when any of it can change, bActs when any of it has
   side effects. A variable named directly is located by nothing; a TArray element always reads the array's storage,
   which a resize moves. */
static void Locators(const Json& N, bool& bReads, bool& bActs)
{
    const std::string K = Kind(N);
    auto Value = [&](const Json& V) { bReads = bReads || !IsFixedValue(V); bActs = bActs || !IsPlainRead(V); };
    if (K == "ParenExpr" || (K == "ImplicitCastExpr" && N.value("castKind", std::string()) == "NoOp")
        || (K == "MemberExpr" && !N.value("isArrow", false)))
        Locators(N["inner"][0], bReads, bActs);
    else if (K == "MemberExpr" || (K == "UnaryOperator" && N.value("opcode", std::string()) == "*"))
        Value(N["inner"][0]);
    else if ((IsTArrayElement(N) || IsTMapElement(N)) && N["inner"].size() == 3)
    {
        bReads = bReads || IsTArrayElement(N);
        Locators(N["inner"][1], bReads, bActs);
        Value(N["inner"][2]);
    }
    else if (K != "DeclRefExpr")
        bReads = bActs = true;
}

/* A fresh local initialised from Init, declared into Pre; returns an rvalue read of it. */
Json FCompiler::SynthLocal(const std::string& Type, const Json& Init, Json& Pre)
{
    const int32 N = ReadTmpCounter++;
    const std::string Id = "upd" + std::to_string(N), LocalN = "__Upd" + std::to_string(N) + "__";
    const Json T = { { "qualType", Type } };
    Pre.push_back({ { "kind", "DeclStmt" }, { "inner", Json::array({
        { { "kind", "VarDecl" }, { "id", Id }, { "name", LocalN }, { "type", T }, { "init", "c" }, { "inner", Json::array({ Init }) } } }) } });
    const Json Ref = { { "kind", "DeclRefExpr" }, { "type", T }, { "valueCategory", "lvalue" },
                       { "referencedDecl", { { "id", Id }, { "kind", "VarDecl" }, { "name", LocalN }, { "type", T } } } };
    return { { "kind", "ImplicitCastExpr" }, { "castKind", "LValueToRValue" }, { "type", T }, { "valueCategory", "prvalue" },
             { "inner", Json::array({ Ref }) } };
}

/* A read of lvalue L, and `To = From`: statements the desugarings build. */
static Json ReadOf(const Json& L)
{
    return { {"kind", "ImplicitCastExpr"}, {"castKind", "LValueToRValue"}, {"type", L.value("type", Json::object())}, {"inner", Json::array({L})} };
}
static Json AssignOf(const Json& To, const Json& From)
{
    return { {"kind", "BinaryOperator"}, {"opcode", "="}, {"type", To.value("type", Json::object())}, {"inner", Json::array({To, From})} };
}

/* An rvalue N, parked in a local unless reading it again is harmless. bPin also parks a variable, which code
   run in between may reassign. */
Json FCompiler::HoistExpr(const Json& N, Json& Pre, bool bPin)
{
    if (bPin ? IsFixedValue(N) : IsSideEffectFree(N)) return N;
    return SynthLocal(StripTypeKeywords(TypeOf(N)), N, Pre);
}

/* The same place as lvalue N, with whatever locates it (an index, a pointer from a call) evaluated into Pre
   once. The place itself is not copied: the store has to land in it. bPin fixes the place for a stretch of
   code that may reassign what locates it (`P = Q` in a range-for body), not just for side effects. */
Json FCompiler::StabilizeLvalue(const Json& N, Json& Pre, bool bPin)
{
    if (!bPin && IsSideEffectFree(N)) return N;
    const std::string K = Kind(N);
    Json Out = N;
    if (K == "ParenExpr" || (K == "ImplicitCastExpr" && N.value("castKind", std::string()) == "NoOp")
        || (K == "MemberExpr" && !N.value("isArrow", false)))
        Out["inner"][0] = StabilizeLvalue(N["inner"][0], Pre, bPin);
    else if (K == "MemberExpr" || (K == "UnaryOperator" && N.value("opcode", std::string()) == "*"))
        Out["inner"][0] = HoistExpr(N["inner"][0], Pre, bPin);            // `P->X`, `*P`: the pointer is a value
    else if ((IsTArrayElement(N) || IsTMapElement(N)) && N["inner"].size() == 3)
    {
        Out["inner"][1] = StabilizeLvalue(N["inner"][1], Pre, bPin);     // the container is a place too
        Out["inner"][2] = HoistExpr(N["inner"][2], Pre, bPin);
    }
    // ponytail: anything else (a call returning T&) is left to evaluate twice; a pointer local fixes it if one shows up.
    return Out;
}

bool FCompiler::DesugarUpdate(const Json& S, Json& Wrap, std::string* Result, std::string* Err)
{
    const std::string K = Kind(S);
    const std::string Op = S.value("opcode", std::string());
    const bool bStep = K == "UnaryOperator";
    const Json* Orig = Nth(S, 0);
    if (!Orig || (!bStep && !Nth(S, 1))) { *Err = "`" + Op + "` with no destination"; return false; }

    /* X is located once. C++17 sequences Y before X in `X op= Y`, so a Y that must not move past what locates X
       (its side effects, or an index Y changes) goes first. */
    Json Pre = Json::array(), LhsPre = Json::array();
    const Json LhsNode = StabilizeLvalue(*Orig, LhsPre);
    Json Rhs = bStep ? Json{ {"kind", "IntegerLiteral"}, {"type", {{"qualType", "int"}}}, {"value", "1"} } : *Nth(S, 1);
    bool bLocReads = false, bLocActs = false;
    Locators(*Orig, bLocReads, bLocActs);
    if (bLocReads && (bLocActs || !IsPlainRead(Rhs))) Rhs = HoistExpr(Rhs, Pre, true);
    for (Json& D : LhsPre) Pre.push_back(std::move(D));
    const Json* Lhs = &LhsNode;

    const Json LhsType = Lhs->value("type", Json::object());
    const bool bPointer = !PointeeOf(*Lhs).empty();
    std::string OpType = bStep ? TypeOf(*Lhs) : S.value("computeLHSType", Json::object()).value("qualType", TypeOf(*Lhs));
    if (bStep && !bPointer)
    {
        const std::string C = Canon(OpType);
        OpType = C == "float" ? "float" : IsInt64Type(OpType) ? "int64" : "int";
    }
    Json Read = { {"kind", "ImplicitCastExpr"}, {"castKind", "LValueToRValue"}, {"type", LhsType}, {"inner", Json::array({*Lhs})} };
    const Json PlainRead = Read;
    if (!bPointer && StripTypeKeywords(OpType) != StripTypeKeywords(TypeOf(*Lhs)))
        Read = { {"kind", "ImplicitCastExpr"}, {"castKind", "IntegralCast"}, {"type", {{"qualType", OpType}}}, {"inner", Json::array({Read})} };
    if (bStep && !bPointer && OpType != "int")
        Rhs = { {"kind", "ImplicitCastExpr"}, {"castKind", "IntegralCast"}, {"type", {{"qualType", OpType}}}, {"inner", Json::array({Rhs})} };
    const std::string BinOp = bStep ? std::string(Op == "++" ? "+" : "-") : Op.substr(0, Op.size() - 1);
    Json Value = { {"kind", "BinaryOperator"}, {"opcode", BinOp}, {"type", bPointer ? LhsType : Json{{"qualType", OpType}}},
                   {"inner", Json::array({Read, Rhs})} };
    if (!bPointer && StripTypeKeywords(OpType) != StripTypeKeywords(TypeOf(*Lhs)))
        Value = { {"kind", "ImplicitCastExpr"}, {"castKind", "IntegralCast"}, {"type", LhsType}, {"inner", Json::array({Value})} };

    /* The value of `X--` is X before the store; of `++X` and `X op= Y`, X after it. */
    const bool bPostfix = bStep && S.value("isPostfix", false);
    const std::string ValueType = StripTypeKeywords(TypeOf(*Lhs));
    auto KeepResult = [&]() {
        const Json ResRef = SynthLocal(ValueType, PlainRead, Pre);
        *Result = ResRef["inner"][0]["referencedDecl"].value("name", std::string());
    };
    if (Result && bPostfix) KeepResult();
    Pre.push_back({ {"kind", "BinaryOperator"}, {"opcode", "="}, {"type", LhsType}, {"inner", Json::array({*Lhs, Value})} });
    if (Result && !bPostfix) KeepResult();
    Wrap = { {"kind", "CompoundStmt"}, {"inner", std::move(Pre)} };
    return true;
}

/* `return Any |= Bad;`, `Items[I++]`: the update's statements run first, inline, and its value is a local. */
bool FCompiler::LowerUpdateValue(const Json& N, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    if (!CurLocals) { *Err = "internal: an update expression outside a function body"; return false; }
    Json Wrap;
    std::string Result;
    auto Body = std::make_shared<std::vector<FStmtIR>>();
    if (!DesugarUpdate(N, Wrap, &Result, Err) || !LowerBody(Wrap, BP, *Body, *CurLocals, Err)) return false;
    const std::string Type = StripTypeKeywords(TypeOf(*Nth(N, 0)));
    auto Block = std::make_shared<std::vector<FStmtIR>>(1);
    (*Block)[0].K = FStmtIR::Block;
    (*Block)[0].Body = Body;
    Out.K = FArgIR::Call;
    Out.InnerType = Type;
    Out.Sub = std::make_shared<FCallIR>();
    Out.Sub->Intrinsic = "__Inline__";
    Out.Sub->Inline = Block;
    Out.Sub->InlineResult = Result;
    Out.Sub->InlineType = Type;
    return true;
}

/* `Add5(M[1])`, `Add5(C ? X : Y)`: Blueprint has no reference to a map element (Map_Find copies it out) or to
   whichever variable `C ? X : Y` picks. So the call is sugar for
       <what locates the place: key, condition, object, pinned>  T Copy = <the place>;  <the call on Copy>;  <the place> = Copy;
   the place fixed before the call, as a reference is bound. A copy is not a reference, though: code that reads the
   place while the call runs sees it unchanged, hence the warning.
   An empty parameter name is the receiver of a container method, `Map[K].Add(X)`: the library function runs no code
   of the mod's, so nothing can tell the copy from the place and there is no warning. What C++ evaluates between
   locating the place and the call, the arguments, goes before the copy; a method that only reads stores nothing. */
bool FCompiler::LowerCopyBack(const Json& Call, const std::vector<std::pair<const Json*, std::string>>& Refs,
                              const std::string& Method, FBlueprintClass& BP, FCallIR& Out, std::string* Err)
{
    if (!CurLocals) { *Err = "internal: a call outside a function body"; return false; }
    Json Pre = Json::array(), Back = Json::array(), Again = Call;
    for (const auto& [Arg, Parm] : Refs)
    {
        const bool bReceiver = Parm.empty();
        size_t At = 1;
        while (!bReceiver && At < Call["inner"].size() && &Call["inner"][At] != Arg) ++At;
        if (bReceiver ? !First(Call) || Kind(*First(Call)) != "MemberExpr" || First(*First(Call)) != Arg : At == Call["inner"].size())
        { *Err = "internal: " + Method + "'s " + (bReceiver ? "object" : "argument for " + Parm) + " is not in the call"; return false; }
        Json& Slot = bReceiver ? Again["inner"][0]["inner"][0] : Again["inner"][At];
        const Json& Bare = *PeelLvalue(Arg);
        const bool bSel = Kind(Bare) == "ConditionalOperator";
        Json Place = Bare;
        if (bSel)
        {
            /* Both sides are located up front, so neither may act: only the picked one would, in C++. */
            bool bReads = false, bActs = false;
            Locators(Bare["inner"][1], bReads, bActs);
            Locators(Bare["inner"][2], bReads, bActs);
            if (bActs)
            { *Err = Method + ": TODO: its reference parameter " + Parm + " is bound to `C ? X : Y` where X or Y is found by a call"; return false; }
            Place["inner"][0] = HoistExpr(Bare["inner"][0], Pre, true);
            Place["inner"][1] = StabilizeLvalue(Bare["inner"][1], Pre, true);
            Place["inner"][2] = StabilizeLvalue(Bare["inner"][2], Pre, true);
        }
        else Place = StabilizeLvalue(Bare, Pre, true);
        if (bReceiver)
            for (size_t I = 1; I < Again["inner"].size(); ++I)
            {
                // ponytail: a value only; an argument bound to a place (`T&`) is left where it is.
                Json& A = Kind(Again["inner"][I]) == "MaterializeTemporaryExpr" ? Again["inner"][I]["inner"][0] : Again["inner"][I];
                if (A.value("valueCategory", std::string()) == "prvalue" && Kind(A) != "CXXDefaultArgExpr") A = HoistExpr(A, Pre);
            }
        const Json Copy = SynthLocal(StripTypeKeywords(TypeOf(Bare)), ReadOf(Place), Pre);
        if (!bReceiver || !IsContainerRead(Method))
            Back.push_back(bSel ? Json{ {"kind", "IfStmt"}, {"inner", Json::array({Place["inner"][0], AssignOf(Place["inner"][1], Copy),
                                                                                     AssignOf(Place["inner"][2], Copy)})} }
                                : AssignOf(Place, Copy));
        Slot = Copy["inner"][0];      // the local itself, which the call takes by reference
        if (bReceiver) continue;
        const Json Seen = Unalias(Bare);
        const std::string What = IsTMapElement(*PeelLvalue(&Seen)) ? "a map element" : MapElementUnder(Seen) ? "a map element's member"
                               : bSel ? "`C ? X : Y`" : "that expression";
        char Line[512];
        snprintf(Line, sizeof(Line), "  warning: %s::%s: %s's reference parameter %s is bound to %s: Blueprint has no reference "
                 "to it, so %s gets a copy, stored back after the call\n", Cur ? Cur->CppName.c_str() : "", CurFnName.c_str(),
                 Method.c_str(), Parm.c_str(), What.c_str(), Parm.c_str());
        if (WarnedCopies.insert(Line).second) fputs(Line, stdout);
    }
    const std::string Type = StripTypeKeywords(TypeOf(Call));
    std::string Result;
    if (Type.empty() || Type == "void") Pre.push_back(Again);
    else Result = SynthLocal(Type, Again, Pre)["inner"][0]["referencedDecl"].value("name", std::string());
    for (Json& B : Back) Pre.push_back(std::move(B));
    const Json Wrap = { {"kind", "CompoundStmt"}, {"inner", std::move(Pre)} };
    auto Body = std::make_shared<std::vector<FStmtIR>>();
    if (!LowerBody(Wrap, BP, *Body, *CurLocals, Err)) return false;
    Out = FCallIR();
    Out.Intrinsic = "__Inline__";
    Out.Inline = std::make_shared<std::vector<FStmtIR>>(1);
    (*Out.Inline)[0].K = FStmtIR::Block;
    (*Out.Inline)[0].Body = Body;
    Out.InlineResult = Result;
    if (!Result.empty()) Out.InlineType = Type;
    return true;
}

/* What a defaulted argument stands for. clang 18 writes the CXXDefaultArgExpr with no child, and the default is the
   parameter's own initialiser (instantiated, in a template's instantiation); a newer clang nests it in the node.
   Either way the answer is the default itself, so a caller sees the same node from both. */
const Json* DefaultedArg(const Json& Arg, const Json* Parm)
{
    if (Kind(Arg) != "CXXDefaultArgExpr") return &Arg;
    if (const Json* Nested = First(Arg)) return Nested;
    if (!Parm) return &Arg;
    const Json* Init = nullptr;
    ForEach(*Parm, [&](const Json& C) {
        const std::string K = Kind(C);
        if (!Init && (K.size() < 4 || K.compare(K.size() - 4, 4, "Attr") != 0)) Init = &C;
    });
    return Init ? Init : &Arg;
}

/* A struct value's arguments in the order of `Fields`. A constructor call puts each argument on the member its parameter
   is named after, a defaulted one being that parameter's default: the stub may take them in the engine's C++ order
   (`FColor(R, G, B, A = 255)`) while the members keep the reflected one (B, G, R, A). Braces, and a constructor whose
   parameters do not all name members, go by position. */
std::vector<const Json*> FCompiler::StructArgs(const Json& Value, const FRecord* R, const std::vector<std::string>& Fields) const
{
    std::vector<const Json*> Args;
    ForEach(Value, [&](const Json& A) { Args.push_back(&A); });
    const Json* Ctor = nullptr;
    if (R && Kind(Value) != "InitListExpr")
        for (const Json* C : R->Ctors) if (!Args.empty() && ParmNames(*C).size() == Args.size()) Ctor = C;
    if (!Ctor || Args.size() != Fields.size()) return Args;
    std::vector<const Json*> Parms;
    ForEach(*Ctor, [&](const Json& C) { if (Kind(C) == "ParmVarDecl") Parms.push_back(&C); });
    std::vector<const Json*> Out(Fields.size(), nullptr);
    for (size_t I = 0; I < Args.size(); ++I)
    {
        const auto At = std::find(Fields.begin(), Fields.end(), Name(*Parms[I]));
        if (At == Fields.end() || Out[size_t(At - Fields.begin())]) return Args;
        Out[size_t(At - Fields.begin())] = DefaultedArg(*Args[I], Parms[I]);
    }
    return Out;
}

bool FCompiler::LowerCall(const Json& CallExprNode, FBlueprintClass& BP, FCallIR& Out, std::string* Err)
{
    const std::string K = Kind(CallExprNode);
    const Json* Callee = Strip(First(CallExprNode));
    if (!Callee) { *Err = "call with no callee"; return false; }

    std::string DeclId, MethodName;
    const Json* FullDecl = nullptr;     // the UFunction's own signature, whichever overload was called
    const Json* Receiver = nullptr;     // a forwarded method's object, first among the arguments (UObject::GetOuter)
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
    /* SpawnActor returns None while a construction script runs (bIsRunningConstructionScript, LevelActor.cpp), and the
       editor keeps spawning out of the construction script graph. */
    const bool bSpawn = MethodName == "BeginDeferredActorSpawnFromClass" || MethodName == "BeginSpawningActorFromClass"
                     || MethodName == "FinishSpawningActor";
    if (bSpawn && CurFnName == "UserConstructionScript")
    { *Err = "UserConstructionScript: " + MethodName + " spawns an actor, which the engine refuses while a construction "
             "script runs (it returns None); spawn in ReceiveBeginPlay"; return false; }
    /* A helper may run elsewhere too, so it only warns. */
    if (bSpawn && Cur && UcsReached.count(CurFnName) && WarnedUcsSpawn.insert(CurFnName).second)
        printf("  warning: %s::%s spawns an actor and UserConstructionScript calls it: the engine refuses a spawn while a "
               "construction script runs (it returns None)\n", Cur->CppName.c_str(), CurFnName.c_str());
    /* AddComponent finds its template by name in the calling class's ComponentTemplates (ActorConstruction.cpp
       1100-1125), and a mod class has none, so it would always return None. */
    if (MethodName == "AddComponent" && K == "CXXMemberCallExpr")
    { *Err = CurFnName + ": AddComponent looks up a component template by name, and a mod class has no component "
             "templates, so it returns None; add one by class with AddComponentByClass"; return false; }
    /* A dispatcher's signature function is a stub the editor's backend never calls (KismetCompilerVMBackend.cpp
       1248-1252): calling it runs nothing and broadcasts nothing. */
    if (MethodName.size() > 19 && MethodName.compare(MethodName.size() - 19, 19, "__DelegateSignature") == 0)
    { *Err = CurFnName + ": " + MethodName + " is the dispatcher's signature, which does nothing when called; call "
             + MethodName.substr(0, MethodName.size() - 19) + ".Broadcast(...)"; return false; }
    /* K2_SetTimer(this, "Name", ...) finds Name on the object at run time and sets no timer when it is missing or takes
       parameters (KismetSystemLibrary.cpp 449-497); an inline method is no function of the class. */
    if (MethodName == "K2_SetTimer" && Cur)
    {
        const Json* Obj = Strip(Nth(CallExprNode, 1));
        const Json* FnArg = Nth(CallExprNode, 2);
        std::string Target;
        if (Obj && Kind(*Obj) == "CXXThisExpr" && FnArg && FindLiteral(*FnArg, Target))
        {
            const Json* Found = nullptr;
            const FRecord* In = nullptr;
            for (const FRecord* A = Cur; A && !Found; A = A->Base.empty() ? nullptr : Find(A->Base))
                if (auto M = A->Methods.find(Target); M != A->Methods.end()) { Found = M->second; In = A; }
            const char* Why = !Found ? "names no function of the class"
                            : IsInlineMethod(*In, Target) ? "names an inline method, which is no function of the class"
                            : !ParmNames(*Found).empty() ? "names a function that takes parameters" : nullptr;
            if (Why)
            { *Err = CurFnName + ": K2_SetTimer by name " + Target + " " + Why + ", so the engine sets no timer; "
                     "name a method without parameters, or pass a delegate"; return false; }
        }
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
        if (auto Tm = MemberTemplates.find(DeclId); Tm != MemberTemplates.end())
            return ExpandInline(CallExprNode, *Tm->second, MethodName + "<" + DeclId + ">", true, BP, Out, Err);
        auto Owner = MethodOwner.find(DeclId);
        if (Owner == MethodOwner.end()) { *Err = "call to an unknown function: " + MethodName; return false; }
        const FRecord* R = Find(Owner->second);
        if (!R) { *Err = "call to a function on an unknown class: " + Owner->second; return false; }

        auto Decl = R->Methods.find(MethodName);
        /* `Obj->GetOuter()`: no UFunction of Object. The header forwards it to the static that does it
           (`UKismetSystemLibrary::GetOuterObject`), and Obj goes first among the arguments. From here on the call IS
           that static's: its declaration, purity, flags and world-context wiring. */
        if (auto Fw = R->Forwards.find(MethodName); Decl != R->Methods.end() && Fw != R->Forwards.end())
        {
            if (K == "CXXMemberCallExpr") Receiver = Strip(First(*Callee));
            if (!Receiver) { *Err = MethodName + "(): TODO: only a call on an object (`Obj->" + MethodName + "()`)"; return false; }
            /* No `::`: a free inline function (UObject_GetOuter), expanded here with the object as its first argument. */
            if (Fw->second.find("::") == std::string::npos)
            {
                const Json* Def = nullptr;
                for (const auto& [Id, N] : FreeInlines) if (Name(*N) == Fw->second) { Def = N; break; }
                if (!Def) { *Err = MethodName + "() is " + Fw->second + ", which is not declared here: include its UeApi header"; return false; }
                Out.bReceiverIsArg = true;
                return ExpandInline(CallExprNode, *Def, Fw->second, false, BP, Out, Err, Receiver);
            }
            const size_t Sep = Fw->second.rfind("::");
            const FRecord* Lib = Sep == std::string::npos ? nullptr : Find(Fw->second.substr(0, Sep));
            const std::string Target = Sep == std::string::npos ? std::string() : Fw->second.substr(Sep + 2);
            if (!Lib || !Lib->Methods.count(Target))
            { *Err = MethodName + "() is " + Fw->second + ", which is not declared here: include its UeApi header"; return false; }
            R = Lib;
            MethodName = Target;
            Decl = R->Methods.find(MethodName);
            Out.bReceiverIsArg = true;
        }
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
            Out.WrittenArgs = bOutParm ? ~uint64(0) : 0;
            Out.bPure = !bOutParm && (IsPureDecl(*Decl->second) || (Def != R->MethodDefs.end() && IsPureDecl(*Def->second)));
        }
        /* The overload called, by its decl id: same-name inline overloads are all legal, none is a UFunction. */
        if (auto Inl = R->Inlines.find(DeclId); Inl != R->Inlines.end())
            return ExpandInline(CallExprNode, *Inl->second, R->CppName + "::" + MethodName, true, BP, Out, Err);
        /* A non-inline overload of a name Generate skips as inline (it goes by name): there is no UFunction to call. */
        if (Decl != R->Methods.end() && IsInlineMethod(*R, MethodName))
        {
            *Err = R->CppName + "::" + MethodName + ": TODO: an overload set may not mix inline and non-inline functions";
            return false;
        }

        /* `Base::Method()` on this, from a class that declares Method itself. C++ name hiding leaves only the qualified
           spelling to reach the ancestor's (clang's JSON drops the qualifier, the referenced decl's owner keeps it), and
           it means THAT implementation: the editor's "call to parent function", EX_FinalFunction on the parent's own
           UFunction (K2Node_CallParentFunction) - which Out.Fn below already is. By name the call would come straight
           back to the override making it, forever: a shipping build has no script recursion guard. A native ancestor's
           takes the final form anyway. */
        /* The class the call is written in: an inline method expanded into a subclass keeps its own class's view, so
           PBase::Twice's `Speak()` stays a call by name in Kid, not Kid's call to its parent's Speak. */
        const FRecord* Written = Cur;
        if (!InlineStack.empty())
            if (auto O = MethodOwner.find(InlineStack.back()->value("id", std::string())); O != MethodOwner.end())
                Written = Find(O->second);
        bool bParentCall = false;
        if (Written && R != Written && Kind(CallExprNode) == "CXXMemberCallExpr")
        {
            const Json* Callee = Strip(First(CallExprNode));
            const Json* Obj = Callee ? Strip(First(*Callee)) : nullptr;
            if (Obj && Kind(*Obj) == "CXXThisExpr")
                for (const FRecord* A = Written; A && A != R && !bParentCall; A = A->Base.empty() ? nullptr : Find(A->Base))
                    bParentCall = A->Methods.count(MethodName) != 0;
        }
        /* `final` (FinalOwner): the one version of the method every object the call can run on reaches, called as
           that function instead of by name. On `this` the class asked is the one being compiled, not the one the call
           is written in, since a call by name from an inherited body reaches the object's version; on another object,
           the object's type. */
        const Json* On = K == "CXXMemberCallExpr" ? Strip(First(*Callee)) : nullptr;
        const bool bOnThis = !On || Kind(*On) == "CXXThisExpr";
        const FRecord* Bound = nullptr;
        if (!R->IsNative() && !bStatic && !bParentCall && !Out.bReceiverIsArg)
        {
            std::string Of = bOnThis ? std::string() : StripTypeKeywords(TypeOf(*On));
            while (!Of.empty() && (Of.back() == '*' || Of.back() == ' ')) Of.pop_back();
            if ((Bound = FinalOwner(bOnThis ? Cur : Find(Of), MethodName)) && Bound->IsNative()) Bound = nullptr;
        }
        const FRecord* Called = Bound ? Bound : R;
        /* KismetCompilerVMBackend.cpp picks the local form unless the callee is native, a net function, authority
           only or cosmetic. A method declared only by mod classes, without an RPC marker, is none of those; an
           override of a native function keeps whatever flags it inherits, so it stays EX_VirtualFunction. */
        bool bLocal = true;
        for (const FRecord* A = Called; A && bLocal; A = A->Base.empty() ? nullptr : Find(A->Base))
        {
            auto M = A->Methods.find(MethodName);
            if (M == A->Methods.end()) continue;
            if (A->IsNative() || NetFlagsOf(*M->second) || AccessFlagsOf(*M->second)) bLocal = false;
            if (auto D = A->MethodDefs.find(MethodName); D != A->MethodDefs.end() && (NetFlagsOf(*D->second) || AccessFlagsOf(*D->second)))
                bLocal = false;
        }
        if (!R->IsNative() && !bStatic && !bParentCall && !Bound)
            Out.VirtualName = UeNameOf(R, MethodName);      // an override of `Set is Extruded` is found by that name
        Out.bLocal = (!Out.VirtualName.empty() || Bound) && bLocal;
        /* A call whose one body is known here - bound on `this`, a parent's, or a static of this mod - is that body,
           expanded in place. The function stays, for delegates, timers, other mods and the editor. */
        if (const FRecord* In = bStatic || bParentCall ? R : Bound; In && bOnThis && !Out.bReceiverIsArg && CurLocals)
            if (const Json* Def = Expandable(*In, MethodName, CallExprNode, FullDecl))
                return ExpandInline(CallExprNode, *Def, In->CppName + "::" + MethodName, true, BP, Out, Err, nullptr, bStatic);

        const std::string CalleePackage = PackageOf(*Called), CalleeName = ClassOf(*Called);
        Out.Fn = BP.EngineFunction(CalleePackage, CalleeName, UeNameOf(Called, MethodName));
        Out.bScript = CalleePackage.compare(0, 6, "/Game/") == 0;
        Out.bInstance = !bStatic;
        /* A Blueprint static needs the CDO context; EX_CallMath finds it itself for a native, but never asks the
           callspace an authority-only or cosmetic one has (KismetCompilerVMBackend.cpp 1222-1231): that one is called
           through CallFunction on the CDO, as the editor calls it. */
        if (bStatic && (Out.bScript || (FullDecl && AccessFlagsOf(*FullDecl))))
            Out.Context = BP.ClassDefaultObject(CalleePackage, CalleeName);
    }

    /* The called declaration's parameters, for a defaulted argument: FullDecl, when it has the call's arity. */
    std::vector<const Json*> CalledParms;
    if (FullDecl) ForEach(*FullDecl, [&](const Json& C) { if (Kind(C) == "ParmVarDecl") CalledParms.push_back(&C); });
    if (CalledParms.size() != (Receiver ? 1u : 0u) + (CallExprNode.contains("inner") ? CallExprNode["inner"].size() - 1 : 0u))
        CalledParms.clear();
    std::vector<std::pair<const Json*, std::string>> CopyBacks;
    for (size_t I = Receiver ? 1 : 0, At = 1; I < CalledParms.size(); ++I, ++At)
        if (IsUnreferenceable(*CalledParms[I], Unalias(CallExprNode["inner"][At])))
            CopyBacks.emplace_back(&CallExprNode["inner"][At], Name(*CalledParms[I]));
    if (!CopyBacks.empty()) return LowerCopyBack(CallExprNode, CopyBacks, MethodName, BP, Out, Err);

    /* inner[0] is the callee. */
    bool bFirst = true, bOk = true;
    std::vector<bool> Defaulted;
    if (Receiver)
    {
        FArgIR A;
        bOk = LowerArg(*Receiver, BP, A, Err);
        if (bOk) Out.Args.push_back(A);
        Defaulted.push_back(false);
    }
    ForEach(CallExprNode, [&](const Json& C) {
        if (bFirst) { bFirst = false; return; }
        if (!bOk) return;
        FArgIR A;
        const size_t I = Defaulted.size();
        bOk = LowerArg(*DefaultedArg(C, I < CalledParms.size() ? CalledParms[I] : nullptr), BP, A, Err);
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
    bool bGivesInfo = Hidden && Out.Args.size() == Parms.size();
    if (Hidden && CallExprNode.contains("inner"))       // `Delay(1.0f, Info)`: one short, so the count alone misses it
        for (size_t At = 1; At < CallExprNode["inner"].size(); ++At)
            bGivesInfo = bGivesInfo || StripTypeKeywords(TypeOf(CallExprNode["inner"][At])) == "FLatentActionInfo";
    if (bGivesInfo)
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
        const bool bSelf = CurrentWco.empty()
                        && (Out.Args.size() + 1 + Omitted == Parms.size() || (I < Defaulted.size() && Defaulted[I]));
        if (Out.Args.size() + 1 + Omitted == Parms.size()) Out.Args.insert(Out.Args.begin() + I, Wco);
        else if (I < Defaulted.size() && Defaulted[I]) Out.Args[I] = Wco;
        /* The editor wires self to the pin only in a class that implements GetWorld (CallFunctionHandler.cpp 547-598);
           any other object finds a world only through its Outer (UObject::GetWorld). */
        bool bOwnWorld = false;
        for (const FRecord* A = Cur; A && !bOwnWorld; A = A->Base.empty() ? nullptr : Find(A->Base))
            bOwnWorld = A->UeName == "Actor" || A->UeName == "ActorComponent" || A->UeName == "UserWidget"
                     || A->UeName == "GameInstance" || A->UeName == "Subsystem";
        if (bSelf && Cur && !bOwnWorld && WarnedNoWorld.insert(Cur->CppName + "::" + CurFnName).second)
            printf("  warning: %s::%s passes self as %s's world context, and an object of this class finds its world only "
                   "through its Outer: make it with an actor or component as Outer, or the call finds no world\n",
                   Cur->CppName.c_str(), CurFnName.c_str(), MethodName.c_str());
        break;
    }
    /* A reference parameter, const or not, is CPF_OutParm. A script callee steps its argument with no result buffer to
       take the address (ProcessScriptFunction), and a native reads it through Stack.MostRecentPropertyAddress whenever
       the argument left one (P_GET_PROPERTY_REF), which a nested call does - the address of whatever ITS arguments read
       last. HoistCallArgs gives such an argument a local to live in. genueapi keeps a native's const reference
       parameters `const T&` for this (Dumper-7's flags; its spelling alone cannot tell one from a by-value string). */
    auto FillRefParms = [&] {
        if (Out.Args.size() != Parms.size()) return;
        Out.RefParms.clear();
        Out.bRefsTakeConst = !Out.bScript;
        ForEach(*FullDecl, [&](const Json& C) {
            if (Kind(C) != "ParmVarDecl") return;
            std::string T = TypeOf(C);
            const bool bRef = !T.empty() && T.back() == '&';
            while (!T.empty() && (T.back() == '&' || T.back() == ' ')) T.pop_back();
            Out.RefParms.push_back(bRef ? StripTypeKeywords(T) : std::string());
        });
    };
    if (!Hidden) FillRefParms();
    if (Hidden)
    {
        if (!LatentRefusal.empty()) { *Err = "latent call " + MethodName + ": " + LatentRefusal; return false; }
        std::vector<std::pair<size_t, FArgIR>> Fill;
        FArgIR L;
        L.K = FArgIR::LatentInfo;
        L.Owner = BP.ScriptStruct("/Script/Engine", "LatentActionInfo");
        L.S = "ExecuteUbergraph_" + Cur->CppName;
        /* The latent action manager keys pending actions by UUID per callback target: one per call site. FNV-1a 64,
           what MSVC's std::hash<std::string> is: libstdc++'s differs, and the asset should not depend on the host. */
        uint64 Uuid = 14695981039346656037ull;
        for (const char Ch : Cur->CppName + "." + CurFnName + "#" + std::to_string(++LatentCount))
            Uuid = (Uuid ^ uint8(Ch)) * 1099511628211ull;
        L.I = int32(Uuid);
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
        FillRefParms();
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

/* Two property definitions a frame lays out identically, whatever they are called. */
static std::string PropKey(const FPropertyDef& P)
{
    std::string K = P.Type + "|" + std::to_string(P.ArrayDim) + "|" + std::to_string(P.ElementSize) + "|" + std::to_string(P.PropertyFlags)
                  + "|" + std::to_string(P.Extra.V) + "|" + std::to_string(P.Extra2.V) + "|" + P.StructName + "|" + P.EnumZero;
    if (P.Inner) K += "<" + PropKey(*P.Inner) + ">";
    if (P.Value) K += "<" + PropKey(*P.Value) + ">";
    if (P.Members) for (const FPropertyDef& M : *P.Members) K += "{" + PropKey(M) + "}";
    return K;
}

/*
Every inline expansion, pointer read and hoisted operand mints a local of its own, so a function built from a few
helpers ends up with hundreds of properties in its frame. Here, once the body is final, the compiler's temps and the
function's own locals are packed like registers: statements are numbered in emission order, each spans its first to
last mention, and those of one layout whose spans do not overlap share a property. A copy `A = B` that is B's last
mention and A's first shares too, which leaves it copying a property onto itself, so it goes: the value an inline
body returns, a switch's subject, `T X = Inline()`.

That is sound because a compiler temp is always written before it is read within its span, on every path: it is
stored by the statement hoisted right before its reader, or at the top of the inline block it belongs to. A local
the source declares is too, since C++ scopes it: its declaration comes first, and one in a loop is made fresh each
round (its __Fresh twin). A declaration without an initializer is the exception: it reads as the frame's zero, a
container as empty. It joins
a shared slot only when it can be reset to that at the declaration (ZeroOf, ClearFn), and every such occupant of a
shared slot is, the first one too, which a loop may bring back round after a later one. Kept out: those of any
other type, __Make (a braced struct relies on the frame's default members), the zero-kept __Fresh twins, and the
scratch the pointer reads name implicitly. A loop repeats its body, so a temp
mentioned in its condition, increment or break trailer, or both inside and outside it, spans the whole loop.
Not run with a source goto (any label re-enters) or a latent call (the ubergraph frame outlives the call); the jumps
ThreadBranches makes only go forward, out of a body into the code after it.
*/
/* What a declaration without an initializer resets a shared temp to, so it still reads as the frame's zero: a
   constant for a scalar or pointer, `ClearFn` for a container. False for a struct (EX_StructConst cannot say every
   default) and the rest, which then keep a property of their own. */
static bool ZeroOf(const FPropertyDef& P, FArgIR& Out)
{
    Out = FArgIR();
    if (P.Type == "IntProperty") Out.K = FArgIR::Int;
    else if (P.Type == "Int64Property") Out.K = FArgIR::Int64;
    else if (P.Type == "FloatProperty") Out.K = FArgIR::Float;
    else if (P.Type == "BoolProperty") Out.K = FArgIR::Bool;
    else if (P.Type == "ByteProperty") Out.K = FArgIR::Byte;
    else if (P.Type == "NameProperty") { Out.K = FArgIR::Name; Out.S = "None"; }
    else if (P.Type == "StrProperty") Out.K = FArgIR::Str;
    else if (P.Type == "ObjectProperty" || P.Type == "ClassProperty") Out.K = FArgIR::NullObj;
    else return false;
    return true;
}
static const char* ClearFn(const FPropertyDef& P, const char** Lib)
{
    if (P.Type == "ArrayProperty") { *Lib = "KismetArrayLibrary"; return "Array_Clear"; }
    if (P.Type == "SetProperty") { *Lib = "BlueprintSetLibrary"; return "Set_Clear"; }
    if (P.Type == "MapProperty") { *Lib = "BlueprintMapLibrary"; return "Map_Clear"; }
    return nullptr;
}
/* `A = B`, both whole locals. */
static bool IsLocalCopy(const FStmtIR& St)
{
    return (St.K == FStmtIR::Assign || (St.K == FStmtIR::Decl && St.bHasValue)) && St.Var.K == FArgIR::Local && !St.Var.Base
        && St.Value.K == FArgIR::Local && !St.Value.Base;
}

void FCompiler::CoalesceTemps(std::vector<FStmtIR>& Stmts, std::vector<FPropertyDef>& Locals, FBlueprintClass& BP)
{
    static const char* const kPooled[] = { "__Inl", "__DerefTmp", "__MapGet", "__Upd", "__PtrTmp", "__Switch" };
    auto Pooled = [](const std::string& N) {
        for (const char* P : kPooled) if (N.compare(0, strlen(P), P) == 0) return true;
        return false;
    };
    struct FSpan { int32 First = INT32_MAX, Last = -1; bool bOut = false; };
    std::map<std::string, FSpan> Spans;
    struct FLoop { int32 Start, End; std::set<std::string> Head; };
    std::vector<FLoop> Loops;
    std::map<int32, std::pair<std::string, std::string>> Copies;    // position -> {A, B} of `A = B` there
    int32 Pos = 0;
    auto Touch = [&](const std::string& N) {
        if (N.empty()) return;
        FSpan& S = Spans[N];
        S.First = std::min(S.First, Pos);
        S.Last = std::max(S.Last, Pos);
    };

    /* An expression's own mentions, at Pos; an __Inline__ body inside it was walked (and numbered) before it. */
    std::function<void(const std::vector<FStmtIR>&)> Walk;
    std::function<void(const FArgIR&, bool)> Arg;
    std::function<void(const FCallIR&, bool)> Call = [&](const FCallIR& C, bool bBodies) {
        if (bBodies && C.Inline) Walk(*C.Inline);
        if (!bBodies) Touch(C.InlineResult);
        if (C.Target) Arg(*C.Target, bBodies);
        for (const FArgIR& A : C.Args) Arg(A, bBodies);
    };
    Arg = [&](const FArgIR& A, bool bBodies) {
        if (!bBodies && (A.K == FArgIR::Local || A.K == FArgIR::LocalOut)) Touch(A.S);
        if (A.Sub) Call(*A.Sub, bBodies);
        if (A.Base) Arg(*A.Base, bBodies);
    };
    auto Own = [&](const FStmtIR& St, bool bBodies) {
        Call(St.Target, bBodies);
        Call(St.Call, bBodies);
        for (const FArgIR* A : { &St.Var, &St.Value, &St.Cond, &St.SwitchValue }) Arg(*A, bBodies);
        for (const FArgIR& A : St.CaseTests) Arg(A, bBodies);
    };
    /* A loop's head (condition, increment, break trailer) is recorded by diffing the span map around its walk. */
    auto Snapshot = [&]() { std::map<std::string, int32> M; for (auto& [N, S] : Spans) M[N] = S.Last; return M; };
    auto Mark = [&](const std::map<std::string, int32>& Old, std::set<std::string>& Head) {
        for (auto& [N, S] : Spans) if (auto It = Old.find(N); It == Old.end() || It->second != S.Last) Head.insert(N);
    };
    Walk = [&](const std::vector<FStmtIR>& List) {
        for (const FStmtIR& St : List)
        {
            if (St.K != FStmtIR::While)
            {
                Own(St, true);
                ++Pos;
                if (St.K == FStmtIR::Decl && !St.bHasValue && Spans.find(St.Var.S) == Spans.end()) Spans[St.Var.S].bOut = true;
                if (IsLocalCopy(St)) Copies[Pos] = { St.Var.S, St.Value.S };
                Own(St, false);
                for (const auto* L : { &St.Then, &St.Else, &St.Body }) if (*L) Walk(**L);
                continue;
            }
            FLoop Loop{ ++Pos, 0, {} };
            { auto Old = Snapshot(); Own(St, true); ++Pos; Own(St, false); Mark(Old, Loop.Head); }
            if (St.Body) Walk(*St.Body);
            for (const auto* L : { &St.Inc, &St.Trailer })
                if (*L) { auto Old = Snapshot(); ++Pos; Walk(**L); Mark(Old, Loop.Head); }
            Loop.End = ++Pos;
            Loops.push_back(std::move(Loop));   // an inner loop ends, so lands here, before the one around it
        }
    };
    Walk(Stmts);

    for (const FLoop& L : Loops)
        for (auto& [N, S] : Spans)
            if (L.Head.count(N) || (S.First < L.Start && S.Last >= L.Start) || (S.First <= L.End && S.Last > L.End))
            {
                S.First = std::min(S.First, L.Start);
                S.Last = std::max(S.Last, L.End);
            }

    /* Greedy interval colouring per layout, in order of first mention. */
    std::vector<const FPropertyDef*> Cands;
    for (const FPropertyDef& L : Locals)
        if ((Pooled(L.Name) || L.Name.compare(0, 2, "__") != 0) && L.Name.compare(0, 7, "__Fresh") != 0)
            if (auto S = Spans.find(L.Name); S != Spans.end() && S->second.Last >= 0)
            {
                FArgIR Zero;
                const char* Lib = nullptr;
                if (!S->second.bOut || ZeroOf(L, Zero) || ClearFn(L, &Lib)) Cands.push_back(&L);
            }
    /* Stable: spans can start together, and std::sort orders ties differently under MSVC and libstdc++. */
    std::stable_sort(Cands.begin(), Cands.end(), [&](const FPropertyDef* A, const FPropertyDef* B) { return Spans[A->Name].First < Spans[B->Name].First; });
    struct FSlot { std::string Name; int32 End; };
    std::map<std::string, std::vector<FSlot>> Slots;
    std::map<std::string, std::pair<std::string, size_t>> SlotOf;    // name -> its layout and slot there
    std::map<std::string, std::string> Rename;
    for (const FPropertyDef* L : Cands)
    {
        const FSpan& S = Spans[L->Name];
        const std::string Key = PropKey(*L);
        std::vector<FSlot>& Free = Slots[Key];
        /* `A = B` that starts A's span where B's ends: A takes B's property, and the copy is a no-op (dropped below). */
        size_t At = Free.size();
        if (auto C = Copies.find(S.First); C != Copies.end() && C->second.first == L->Name)
            if (auto B = SlotOf.find(C->second.second); B != SlotOf.end() && B->second.first == Key && Free[B->second.second].End == S.First)
                At = B->second.second;
        if (At == Free.size())
            At = std::find_if(Free.begin(), Free.end(), [&](const FSlot& F) { return F.End < S.First; }) - Free.begin();
        SlotOf[L->Name] = { Key, At };
        if (At == Free.size()) { Free.push_back({ L->Name, S.Last }); continue; }
        Rename[L->Name] = Free[At].Name;
        Free[At].End = S.Last;
    }
    if (Rename.empty()) return;

    /* The no-initializer declarations in a slot shared with anything are reset where they stand. */
    std::set<std::string> Shared;
    for (const auto& [From, To] : Rename) { Shared.insert(From); Shared.insert(To); }
    std::map<std::string, const FPropertyDef*> Resets;
    for (const FPropertyDef* L : Cands) if (Spans[L->Name].bOut && Shared.count(L->Name)) Resets[L->Name] = L;

    std::function<void(std::vector<FStmtIR>&)> RWalk;
    std::function<void(FArgIR&)> RArg;
    std::function<void(FCallIR&)> RCall = [&](FCallIR& C) {
        if (auto R = Rename.find(C.InlineResult); R != Rename.end()) C.InlineResult = R->second;
        if (C.Inline) RWalk(*C.Inline);
        if (C.Target) RArg(*C.Target);
        for (FArgIR& A : C.Args) RArg(A);
    };
    RArg = [&](FArgIR& A) {
        if (A.K == FArgIR::Local || A.K == FArgIR::LocalOut)
            if (auto R = Rename.find(A.S); R != Rename.end()) A.S = R->second;
        if (A.Sub) RCall(*A.Sub);
        if (A.Base) RArg(*A.Base);
    };
    RWalk = [&](std::vector<FStmtIR>& List) {
        for (size_t I = 0; I < List.size(); ++I)
        {
            FStmtIR& St = List[I];
            if (auto R = St.K == FStmtIR::Decl && !St.bHasValue ? Resets.find(St.Var.S) : Resets.end(); R != Resets.end())
            {
                const char* Lib = nullptr;
                if (const char* Fn = ClearFn(*R->second, &Lib))
                {
                    FArgIR Var = St.Var;
                    St = FStmtIR();
                    St.K = FStmtIR::StaticCall;
                    St.Call.Fn = BP.EngineFunction("/Script/Engine", Lib, Fn);
                    St.Call.Args = { Var };
                }
                else
                {
                    St.bHasValue = ZeroOf(*R->second, St.Value);
                }
            }
            RCall(St.Target);
            RCall(St.Call);
            for (FArgIR* A : { &St.Var, &St.Value, &St.Cond, &St.SwitchValue }) RArg(*A);
            for (FArgIR& A : St.CaseTests) RArg(A);
            for (auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer }) if (*L) RWalk(**L);
            if (IsLocalCopy(St) && St.Var.S == St.Value.S) List.erase(List.begin() + I--);
        }
    };
    RWalk(Stmts);
    Locals.erase(std::remove_if(Locals.begin(), Locals.end(), [&](const FPropertyDef& L) { return Rename.count(L.Name) != 0; }), Locals.end());
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

/* Whether a jump from outside Stmts could land inside them: a switch case or a goto label. */
static bool HasLabel(const std::vector<FStmtIR>& Stmts)
{
    for (const FStmtIR& St : Stmts)
    {
        if (St.K == FStmtIR::Label || St.K == FStmtIR::GotoLabel) return true;
        for (const auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer })
            if (*L && HasLabel(**L)) return true;
    }
    return false;
}

/*
A condition that came to a constant decides at compile time: an `if` is the branch it takes, a `while (false)` is
nothing, and a `while (true)` or `do {} while (false)` needs no test. Code that a label makes reachable stays.
*/
void FCompiler::PruneConstBranches(std::vector<FStmtIR>& Stmts)
{
    for (size_t I = 0; I < Stmts.size(); ++I)
    {
        FStmtIR& St = Stmts[I];
        for (auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer })
            if (*L)
            {
                *L = std::make_shared<std::vector<FStmtIR>>(**L);
                PruneConstBranches(**L);
            }
        if (St.Cond.K != FArgIR::Bool) continue;
        if (St.K == FStmtIR::If)
        {
            /* A jump-out `if` holds `!C`, and its break / continue is taken when that is false. */
            const bool bThen = St.bJumpOut ? !St.Cond.B : St.Cond.B;
            const auto Taken = bThen ? St.Then : St.Else, Dropped = bThen ? St.Else : St.Then;
            if (Dropped && HasLabel(*Dropped)) continue;
            const std::vector<FStmtIR> Keep = Taken ? *Taken : std::vector<FStmtIR>();
            Stmts.erase(Stmts.begin() + I);
            Stmts.insert(Stmts.begin() + I, Keep.begin(), Keep.end());
            I += Keep.size();
            --I;
        }
        else if (St.K == FStmtIR::While && !St.Cond.B && !St.bPostTest)
        {
            if (St.Body && HasLabel(*St.Body)) continue;
            Stmts.erase(Stmts.begin() + I--);
        }
        else if (St.K == FStmtIR::While && (St.Cond.B || !St.Trailer))
            St.bConstCond = true;
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
    /* The event is bound by name on self, found on the most derived class first: under a Blueprint parent, whose own
       generated events this compile cannot see, the class's name keeps a child's apart from the parent's. */
    bool bBlueprintParent = false;
    for (const FRecord* A = Cur && !Cur->Base.empty() ? Find(Cur->Base) : nullptr; A && !bBlueprintParent;
         A = A->Base.empty() ? nullptr : Find(A->Base))
        bBlueprintParent = !A->IsNative() || A->UePackage.compare(0, 6, "/Game/") == 0;
    const std::string Named = bBlueprintParent ? LeafOf(Cur->CppName) + "_" + Stem : Stem;
    std::string Event;
    for (int32 N = 0; Taken(Event = Named + "_" + std::to_string(N)); ++N) {}
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
    /* The editor's async node tests the proxy with IsValid and binds its dispatchers and calls Activate on the true
       branch only (K2Node_BaseAsyncTask.cpp 393-408, 440-448): through a None object each would be an 'Accessed None'
       script warning (ScriptCore.cpp 2904-2937) that binds nothing (3085-3101). The run ends at the await either way. */
    auto Binds = Body;
    if (Disp.Base && Disp.Base->K != FArgIR::Self)
    {
        FStmtIR Gate;
        Gate.K = FStmtIR::If;
        Gate.Cond = *Disp.Base;
        WrapInCall(Gate.Cond, BP.EngineFunction("/Script/Engine", "KismetSystemLibrary", "IsValid"));
        Gate.Cond.InnerType = "bool";
        Gate.Then = Binds = std::make_shared<std::vector<FStmtIR>>();
        Body->push_back(std::move(Gate));
    }
    auto Add = [&](FCallIR Call) { Binds->emplace_back(); Binds->back().K = FStmtIR::StaticCall; Binds->back().Call = std::move(Call); };
    FCallIR Bind;
    Bind.Intrinsic = "__AddDelegate__";
    Bind.Args.push_back(Disp);
    Bind.Args.emplace_back();
    Bind.Args.back().K = FArgIR::Delegate;
    Bind.Args.back().S = C.Event;
    Add(Bind);

    /* K2Node_AsyncAction activates a UBlueprintAsyncActionBase once its outputs are bound. Every await on an action
       gets the call as an __Activate__ marker; PlaceActivations keeps it where the variable's action has not been
       activated yet on any path that reaches the await, and drops it where it has, so a second await waits on the
       action already running. */
    std::string ObjType = StripTypeKeywords(TypeOf(*Strip(First(*Arg))));
    while (!ObjType.empty() && (ObjType.back() == '*' || ObjType.back() == ' ')) ObjType.pop_back();
    bool bAction = false;
    for (const FRecord* A = Find(ObjType); A; A = A->Base.empty() ? nullptr : Find(A->Base))
        if (A->UeName == "BlueprintAsyncActionBase") bAction = true;
    if (bAction)
    {
        FCallIR Activate;
        Activate.Intrinsic = "__Activate__";
        Activate.Fn = BP.EngineFunction("/Script/Engine", "BlueprintAsyncActionBase", "Activate");
        Activate.bInstance = true;
        if (Disp.Base) Activate.Target = Disp.Base;
        Activate.Resume = C.Resume;                     // the await it belongs to
        Add(Activate);
    }
    FCallIR Point;
    Point.Intrinsic = "__AwaitPoint__";
    Point.Resume = C.Resume;
    Point.Target = Disp.Base;                           // the object the resume finds its action active on
    Body->emplace_back();
    Body->back().K = FStmtIR::StaticCall;
    Body->back().Call = std::move(Point);

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

namespace
{
/* Every statement of Stmts and of the lists nested in them, in the order FFlowWalk numbers them (each before its own
   lists: Then, Else, Body, Inc, Trailer). Each list is copied before Fn sees it, so a change made through one place
   never reaches another that shares the list. */
void ForEachStmt(std::vector<FStmtIR>& Stmts, const std::function<void(std::vector<FStmtIR>&, size_t)>& Fn)
{
    for (size_t I = 0; I < Stmts.size(); ++I)
    {
        Fn(Stmts, I);
        for (auto* L : { &Stmts[I].Then, &Stmts[I].Else, &Stmts[I].Body, &Stmts[I].Inc, &Stmts[I].Trailer })
            if (*L)
            {
                *L = std::make_shared<std::vector<FStmtIR>>(**L);
                ForEachStmt(**L, Fn);
            }
    }
}

/*
A forward walk of one function's statements along every path its bytecode can take, as EmitStmts lays them out: both
arms of an `if`, a loop's rounds and its exits, a switch's cases, break / continue / goto, an inline block's returns,
a latent call and an await. It carries facts per variable, bit masks joined by OR where paths meet; Step applies one
statement's own effect (its expressions and its store), in the order the VM runs them. A loop head, a goto label and an
await's resume take their state from paths the walk reaches later, so Run walks again until none of them grows; a
statement's number (Seq) is its place in that walk, the same each time.

An await is where control leaves the function's shape: its run ends at the await's return, and the event it bound
re-enters the ubergraph at the offset EmitStmts gave the LAST copy of that await in the function (PeelFirstRound
copies a loop's first round, the await's completion shared). Nothing reaches the code after an earlier copy; the code
after the last one is reached from every copy, with the state AfterAwait makes of each.
*/
struct FFlowWalk
{
    struct FState
    {
        bool bLive = false;                             // false: no path reaches here
        std::map<std::string, uint8> Bits;
    };
    std::function<void(const FStmtIR&, int32, FState&)> Step;
    std::function<void(const FCallIR&, FState&)> AfterAwait;

    static bool Join(FState& Into, const FState& From)
    {
        if (!From.bLive) return false;
        if (!Into.bLive) { Into = From; return true; }
        bool bGrew = false;
        for (const auto& [K, V] : From.Bits)
        {
            uint8& B = Into.Bits[K];
            if ((B | V) != B) { B |= V; bGrew = true; }
        }
        return bGrew;
    }

    void Run(const std::vector<FStmtIR>& Stmts, const FState& Entry)
    {
        std::function<void(const std::vector<FStmtIR>&)> Last = [&](const std::vector<FStmtIR>& List) {
            for (const FStmtIR& St : List)
            {
                if (St.K == FStmtIR::StaticCall && St.Call.Intrinsic == "__AwaitPoint__") LastCopy[St.Call.Resume.get()] = Seq;
                ++Seq;
                for (const auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer }) if (*L) Last(**L);
            }
        };
        Seq = 0;
        Last(Stmts);
        for (int32 Pass = 0; Pass < 64; ++Pass)
        {
            Seq = Loops = 0;
            bGrew = false;
            FState S = Entry;
            List(Stmts, S);
            if (!bGrew) return;
        }
    }

private:
    std::map<int32, FState> Heads, Labels;              // by loop number / LabelId: what paths walked later bring
    std::map<const int32*, FState> Resumes;             // by an await's resume sink: the state its event re-enters with
    std::map<const int32*, int32> LastCopy;
    std::vector<FState*> Breaks, Continues, Returns;
    int32 Seq = 0, Loops = 0;
    bool bGrew = false;

    void Grow(FState& Into, const FState& From) { bGrew = Join(Into, From) || bGrew; }
    void List(const std::vector<FStmtIR>& Stmts, FState& S) { for (const FStmtIR& St : Stmts) One(St, S); }

    void One(const FStmtIR& St, FState& S)
    {
        const int32 Me = Seq++;
        const FState Dead;
        switch (St.K)
        {
        case FStmtIR::If:
        {
            Step(St, Me, S);
            FState Then = S, Else = S;
            if (St.Then) List(*St.Then, Then);          // a jump-out `if`: Then is the break / continue itself
            if (St.Else) List(*St.Else, Else);
            if (!St.bJumpOut) { S = Then; Join(S, Else); }
            break;
        }
        case FStmtIR::While:
        {
            const int32 Loop = Loops++;
            FState Cur = S, Exit, Out, Round;
            Join(Cur, Heads[Loop]);
            if (!St.bPostTest && !St.bConstCond) { Step(St, Me, Cur); Join(Exit, Cur); }
            Breaks.push_back(&Out);
            Continues.push_back(&Round);
            if (St.Body) List(*St.Body, Cur);
            Breaks.pop_back();
            Continues.pop_back();
            Join(Cur, Round);
            if (St.Inc) List(*St.Inc, Cur);
            if (St.bPostTest && !St.bConstCond) { Step(St, Me, Cur); Join(Exit, Cur); }
            if (!St.bConstCond || St.Cond.B) Grow(Heads[Loop], Cur);
            else Join(Exit, Cur);                       // `do {} while (false)` runs once
            if (St.Trailer) List(*St.Trailer, Out);     // runs on `break` only
            Join(Exit, Out);
            S = Exit;
            break;
        }
        case FStmtIR::Switch:
        {
            Step(St, Me, S);
            const FState Head = S;
            FState Out;
            Breaks.push_back(&Out);
            S = Dead;
            if (St.Body)
                for (const FStmtIR& B : *St.Body)
                {
                    if (B.K == FStmtIR::Label && B.LabelId >= 0) Join(S, Head);
                    One(B, S);
                }
            Breaks.pop_back();
            Join(S, Out);
            if (St.LabelId < 0) Join(S, Head);          // no default: a miss goes past the body
            break;
        }
        case FStmtIR::Block:
        {
            /* An inline body's own break / continue belong to loops inside it. */
            FState Ends;
            std::vector<FState*> OuterBreaks, OuterContinues;
            OuterBreaks.swap(Breaks);
            OuterContinues.swap(Continues);
            Returns.push_back(&Ends);
            if (St.Body) List(*St.Body, S);
            Returns.pop_back();
            Breaks.swap(OuterBreaks);
            Continues.swap(OuterContinues);
            Join(S, Ends);
            break;
        }
        case FStmtIR::Break:
        case FStmtIR::Continue:
        case FStmtIR::InlineReturn:
        {
            std::vector<FState*>& To = St.K == FStmtIR::Break ? Breaks : St.K == FStmtIR::Continue ? Continues : Returns;
            if (!To.empty()) Join(*To.back(), S);
            S = Dead;
            break;
        }
        case FStmtIR::Return:
            Step(St, Me, S);
            S = Dead;
            break;
        case FStmtIR::Goto:
            Grow(Labels[St.LabelId], S);
            S = Dead;
            break;
        case FStmtIR::GotoLabel:
            Join(S, Labels[St.LabelId]);
            break;
        case FStmtIR::Label:
            break;
        default:
            if (St.K == FStmtIR::StaticCall && St.Call.Intrinsic == "__AwaitPoint__")
            {
                const int32* Key = St.Call.Resume.get();
                if (S.bLive)
                {
                    FState Back = S;
                    AfterAwait(St.Call, Back);
                    Grow(Resumes[Key], Back);
                }
                S = LastCopy[Key] == Me ? Resumes[Key] : Dead;
                break;
            }
            Step(St, Me, S);
            break;
        }
    }
};

/* The variable an await or an __Activate__ marker is about: its Target's name, "this" for self. */
std::string AwaitedOn(const FCallIR& C) { return C.Target ? C.Target->S : std::string("this"); }

/* Whether a statement list holds the __Activate__ marker of the await whose resume sink is Await, at any depth. */
bool HoldsActivate(const std::vector<FStmtIR>& Stmts, const int32* Await)
{
    for (const FStmtIR& St : Stmts)
    {
        if (St.K == FStmtIR::StaticCall && St.Call.Intrinsic == "__Activate__" && St.Call.Resume.get() == Await) return true;
        for (const auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer })
            if (*L && HoldsActivate(**L, Await)) return true;
    }
    return false;
}

/* Whether Stmts, nested lists included, hold an await, or a goto label that a copy would define twice. */
bool HoldsAwaitOrLabel(const std::vector<FStmtIR>& Stmts)
{
    for (const FStmtIR& St : Stmts)
    {
        if (St.K == FStmtIR::GotoLabel || (St.K == FStmtIR::StaticCall && St.Call.Intrinsic == "__AwaitPoint__")) return true;
        for (const auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer })
            if (*L && HoldsAwaitOrLabel(**L)) return true;
    }
    return false;
}

/* A copy of a loop body's statements that runs outside the loop: its own `break` (the loop's Trailer, then a goto to
   Exit) and `continue` (a goto to Next) become jumps; those of a loop inside it stay, as a switch's own break does. */
std::vector<FStmtIR> Unlooped(const std::vector<FStmtIR>& Stmts, const std::shared_ptr<std::vector<FStmtIR>>& Trailer,
                              int32 Exit, int32 Next, bool bInSwitch, bool* bNext)
{
    auto Jump = [](int32 Label) { FStmtIR G; G.K = FStmtIR::Goto; G.LabelId = Label; return G; };
    auto Leave = [&](bool bBreak) {
        std::vector<FStmtIR> Out;
        if (bBreak && Trailer) Out = *Trailer;
        if (!bBreak) *bNext = true;
        Out.push_back(Jump(bBreak ? Exit : Next));
        return Out;
    };
    std::vector<FStmtIR> Out;
    for (FStmtIR St : Stmts)
    {
        const bool bLoopBreak = St.K == FStmtIR::Break && !bInSwitch;
        if (bLoopBreak || St.K == FStmtIR::Continue)
        {
            for (FStmtIR& L : Leave(bLoopBreak)) Out.push_back(std::move(L));
            continue;
        }
        if (St.K == FStmtIR::If && St.bJumpOut)
        {
            /* Then is the break / continue, taken when Cond is FALSE: now an else that jumps. */
            const bool bBreak = (*St.Then)[0].K == FStmtIR::Break;
            if (!bBreak || !bInSwitch)
            {
                St.bJumpOut = false;
                St.Else = std::make_shared<std::vector<FStmtIR>>(Leave(bBreak));
                St.Then = std::make_shared<std::vector<FStmtIR>>();
            }
        }
        else if (St.K == FStmtIR::If)
        {
            if (St.Then) St.Then = std::make_shared<std::vector<FStmtIR>>(Unlooped(*St.Then, Trailer, Exit, Next, bInSwitch, bNext));
            if (St.Else) St.Else = std::make_shared<std::vector<FStmtIR>>(Unlooped(*St.Else, Trailer, Exit, Next, bInSwitch, bNext));
        }
        else if (St.K == FStmtIR::Switch && St.Body)
            St.Body = std::make_shared<std::vector<FStmtIR>>(Unlooped(*St.Body, Trailer, Exit, Next, true, bNext));
        Out.push_back(std::move(St));
    }
    return Out;
}
}   // namespace

/*
A loop whose await finds its action fresh on the first round and active on the rest cannot keep or drop that await's
Activate: it would start the action again each round, or never. So the first round, up to and including the await,
runs as a copy before the loop: `if (Cond) { <round> } else goto Exit;` (no test for a loop that always runs once),
the loop itself after it. The copy's await keeps its Activate and resumes in the loop's copy, which is the last one
(FFlowWalk), so the loop's own await sees the action active on every path and drops it. Only an await at the top
level of the body, with no other await or goto label before it, where a copy cannot tell which one resumes.
*/
bool FCompiler::PeelFirstRound(std::vector<FStmtIR>& Stmts, const int32* Await)
{
    for (size_t I = 0; I < Stmts.size(); ++I)
    {
        FStmtIR& St = Stmts[I];
        if (St.K == FStmtIR::While && St.Body)
        {
            const std::vector<FStmtIR>& Body = *St.Body;
            size_t At = 0;
            while (At < Body.size() && !(Body[At].K == FStmtIR::Block && HoldsActivate({ Body[At] }, Await))) ++At;
            if (At < Body.size())
            {
                const std::vector<FStmtIR> Before(Body.begin(), Body.begin() + At);
                if (HoldsAwaitOrLabel(Before)) return false;
                const int32 Exit = NextGotoLabel++, Next = NextGotoLabel++;
                bool bNext = false;
                std::vector<FStmtIR> Round = Unlooped(Before, St.Trailer, Exit, Next, false, &bNext);
                Round.push_back(Body[At]);
                FStmtIR Label;
                Label.K = FStmtIR::GotoLabel;
                Label.LabelId = Exit;
                if (bNext)
                {
                    /* `continue` in the first round goes on at the loop's increment, where the loop's own lands. */
                    FStmtIR Mark = Label;
                    Mark.LabelId = Next;
                    St.Inc = std::make_shared<std::vector<FStmtIR>>(St.Inc ? *St.Inc : std::vector<FStmtIR>());
                    St.Inc->insert(St.Inc->begin(), Mark);
                }
                std::vector<FStmtIR> First;
                if (!St.bPostTest && !St.bConstCond)
                {
                    First.emplace_back();
                    First.back().K = FStmtIR::If;
                    First.back().Cond = St.Cond;
                    First.back().Then = std::make_shared<std::vector<FStmtIR>>(std::move(Round));
                    First.back().Else = std::make_shared<std::vector<FStmtIR>>(1);
                    (*First.back().Else)[0].K = FStmtIR::Goto;
                    (*First.back().Else)[0].LabelId = Exit;
                }
                else First = std::move(Round);
                Stmts.insert(Stmts.begin() + I + 1, Label);
                Stmts.insert(Stmts.begin() + I, First.begin(), First.end());
                return true;
            }
        }
        for (auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer })
            if (*L && HoldsActivate(**L, Await))
            {
                *L = std::make_shared<std::vector<FStmtIR>>(**L);
                return PeelFirstRound(**L, Await);
            }
    }
    return false;
}

/*
Decides each __Activate__ marker LowerAwait left (K2Node_BaseAsyncTask.cpp 410-467: an async node binds its outputs,
then activates its proxy, once per run of the node): Activate may broadcast before it returns, and a second call
starts an action such as AsyncLoadPrimaryAsset again (AsyncActionLoadPrimaryAsset.cpp 6-35). Walked in flow order,
each action variable is fresh once assigned (or on entry) and active once an Activate ran or its await resumed; a
marker every path reaches with the action fresh becomes the call, one every path reaches active goes. One reached
both ways is a loop's first round against the rest, split by PeelFirstRound; anything else is refused, since either
choice is wrong on some path.
*/
bool FCompiler::PlaceActivations(std::vector<FStmtIR>& Stmts, std::string* Err)
{
    enum : uint8 { Fresh = 1, Active = 2 };
    std::set<std::string> Actions;
    ForEachStmt(Stmts, [&](std::vector<FStmtIR>& L, size_t I) {
        if (L[I].K == FStmtIR::StaticCall && L[I].Call.Intrinsic == "__Activate__") Actions.insert(AwaitedOn(L[I].Call));
    });
    if (Actions.empty()) return true;
    FFlowWalk::FState Entry;
    Entry.bLive = true;
    for (const std::string& A : Actions) Entry.Bits[A] = Fresh;
    std::set<const int32*> Peeled;
    for (;;)
    {
        std::map<int32, uint8> Reached;                 // by the marker's Seq: how its action stands there
        FFlowWalk Walk;
        Walk.Step = [&](const FStmtIR& St, int32 Seq, FFlowWalk::FState& S) {
            if ((St.K == FStmtIR::Assign || St.K == FStmtIR::Decl) && (St.Var.K == FArgIR::Local || St.Var.K == FArgIR::Field)
                && Actions.count(St.Var.S))
                S.Bits[St.Var.S] = Fresh;
            if (St.K == FStmtIR::StaticCall && St.Call.Intrinsic == "__Activate__" && S.bLive)
            {
                Reached[Seq] |= S.Bits[AwaitedOn(St.Call)];
                S.Bits[AwaitedOn(St.Call)] = Active;
            }
        };
        Walk.AfterAwait = [&](const FCallIR& Point, FFlowWalk::FState& S) {
            if (Actions.count(AwaitedOn(Point))) S.Bits[AwaitedOn(Point)] = Active;
        };
        Walk.Run(Stmts, Entry);

        /* Numbered as the walk numbered them: a marker reached with the action fresh becomes the call (one no path
           reaches too, as it costs nothing), one reached active goes. */
        const int32* Both = nullptr;
        std::string Var;
        int32 Seq = 0;
        ForEachStmt(Stmts, [&](std::vector<FStmtIR>& L, size_t I) {
            const int32 Me = Seq++;
            if (L[I].K == FStmtIR::StaticCall && L[I].Call.Intrinsic == "__Activate__" && Reached[Me] == (Fresh | Active) && !Both)
            { Both = L[I].Call.Resume.get(); Var = AwaitedOn(L[I].Call); }
        });
        if (!Both)
        {
            Seq = 0;
            std::function<void(std::vector<FStmtIR>&)> Decide = [&](std::vector<FStmtIR>& List) {
                for (size_t I = 0; I < List.size(); ++I)
                {
                    const int32 Me = Seq++;
                    for (auto* L : { &List[I].Then, &List[I].Else, &List[I].Body, &List[I].Inc, &List[I].Trailer })
                        if (*L) { *L = std::make_shared<std::vector<FStmtIR>>(**L); Decide(**L); }
                    if (List[I].K != FStmtIR::StaticCall || List[I].Call.Intrinsic != "__Activate__") continue;
                    if (Reached[Me] == Active) List.erase(List.begin() + I--);
                    else List[I].Call.Intrinsic.clear();
                }
            };
            Decide(Stmts);
            return true;
        }
        if (!Peeled.insert(Both).second || !PeelFirstRound(Stmts, Both))
        {
            *Err = "UE_AWAIT on " + Var + ": some paths reach it with " + Var + "'s async action already activated and "
                   "some without, so it would start twice or never; await " + Var + " before the branch or loop, or "
                   "assign " + Var + " again on each path";
            return false;
        }
    }
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
        /* A loop's condition runs on every trip too, so an inline expanded in it re-enters (the __Fresh twin);
           it is not LoopDepth, which would let a break / continue there through. */
        auto LowerCond = [&](const Json& Cond) -> bool {
            ++ReEntered;
            const bool bCondOk = LowerArg(Cond, BP, St.Cond, Err);
            --ReEntered;
            if (!bCondOk) bOk = false;
            return bCondOk;
        };
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
                const std::string DeclId = D.value("id", std::string());
                if (Init)
                {
                    Ds.bHasValue = true;
                    if (!LowerArg(*First(D), BP, Ds.Value, Err)) { bOk = false; return; }
                    /* A constant nothing writes is used in place, the way an inlined parameter is (ExpandInline):
                       the reads become the const and neither the property nor its store is compiled. A repeated
                       expansion of the same inline function reaches this declaration again, so the binding a
                       previous one left goes first. */
                    ParmConst.erase(DeclId);
                    if (!bCurNoOpt && IsFoldableConst(Ds.Value) && ReadOnlyLocal(DeclId))
                    {
                        ParmConst[DeclId] = Ds.Value;
                        return;
                    }
                }
                /* A static keeps its value from one call to the next, and the one frame that outlives a call is the
                   ubergraph's, one per object: a latent function's locals live there, so a static is one of them
                   (Generate refuses it in any other function, once the body shows whether it waits). Its initialiser
                   runs once, `if (!__Once<N>) { X = Init; __Once<N> = true; }`, the flag false and X zero as the
                   frame starts. A Blueprint local has no scope, so another local named X would be this same property
                   and reset it: the static takes a name no C++ local can have. One that holds a constant nothing
                   writes is that constant, kept or not. */
                if (IsStaticDecl(D) && !(Ds.bHasValue && IsFoldableConst(Ds.Value) && ReadOnlyLocal(DeclId)))
                {
                    if (!InlineStack.empty())
                    { *Err = "static " + Name(D) + " in an inline function: each expansion would keep its own"; bOk = false; return; }
                    if (StaticLocal.empty()) StaticLocal = Name(D);
                    const std::string N = std::to_string(ReadTmpCounter++);
                    const std::string Kept = "__Static" + N + "_" + Name(D), Flag = "__Once" + N;
                    LocalRename[DeclId] = Kept;
                    Ds.Var.S = Kept;
                    FPropertyDef KeptPD;
                    if (!TypeToProperty(VarType, Kept, 0, "static " + Name(D), BP, &KeptPD, Err)) { bOk = false; return; }
                    KeptPD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
                    Locals.push_back(KeptPD);
                    if (!Ds.bHasValue) return;
                    FPropertyDef FlagPD;
                    if (!TypeToProperty("bool", Flag, 0, "static " + Name(D), BP, &FlagPD, Err)) { bOk = false; return; }
                    FlagPD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
                    Locals.push_back(FlagPD);
                    FStmtIR Once, Set;
                    Once.K = FStmtIR::If;
                    Once.Cond.K = FArgIR::Local;
                    Once.Cond.S = Flag;
                    Once.Cond = NotOf(Once.Cond, BP);
                    Set.K = FStmtIR::Assign;
                    Set.Var.K = FArgIR::Local;
                    Set.Var.S = Flag;
                    Set.Var.LetOp = LetOpFor("bool");
                    Set.bAssignLocal = true;
                    Set.Value.K = FArgIR::Bool;
                    Set.Value.B = true;
                    Once.Then = std::make_shared<std::vector<FStmtIR>>(std::vector<FStmtIR>{ Ds, Set });
                    Out.push_back(std::move(Once));
                    return;
                }
                if ((LoopDepth > 0 || bBodyHasGoto || ReEntered > 0) && !Ds.bHasValue)
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
        /* A result the statement throws away is stepped into a 64-byte buffer nothing constructs or destroys
           (ScriptCore.cpp 1058, 1120): one with a destructor, or a bigger one, lands in a local instead, as the editor
           gives every return pin a term (KismetCompilerMisc.cpp 1841-1871). */
        auto KeepResult = [&] {
            if (St.K != FStmtIR::StaticCall || !CurLocals || (St.Call.Fn.V == 0 && St.Call.VirtualName.empty())
                || !NeedsResultLocal(TypeOf(*S))) return true;
            FPropertyDef PD;
            const std::string Kept = "__Dropped" + std::to_string(ReadTmpCounter++) + "__";
            if (!TypeToProperty(TypeOf(*S), Kept, 0, "a discarded result", BP, &PD, Err)) return false;
            PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
            CurLocals->push_back(PD);
            FArgIR Value;
            Value.K = FArgIR::Call;
            Value.Sub = std::make_shared<FCallIR>(std::move(St.Call));
            St = FStmtIR();
            St.K = FStmtIR::Assign;
            St.Var.K = FArgIR::Local;
            St.Var.S = Kept;
            St.Var.LetOp = LetOpFor(TypeOf(*S));
            St.bAssignLocal = true;
            St.Value = std::move(Value);
            return true;
        };
        if (K == "CXXMemberCallExpr")
        {
            /* Same lowering as a call used for its value; the target rides in FCallIR::Target. */
            FArgIR V;
            St.K = FStmtIR::StaticCall;
            bOk = LowerArgRaw(*S, TypeOf(*S), BP, V, Err);
            if (bOk) St.Call = *V.Sub;
            if (bOk && St.Call.Inline) { for (FStmtIR& B : *St.Call.Inline) Out.push_back(std::move(B)); return; }
            bOk = bOk && KeepResult();
        }
        else if (K == "CallExpr")
        {
            bOk = LowerCall(*S, BP, St.Call, Err);
            if (bOk && St.Call.Inline) { for (FStmtIR& B : *St.Call.Inline) Out.push_back(std::move(B)); return; }
            bOk = bOk && KeepResult();
        }
        else if (K == "CompoundAssignOperator"
              || (K == "UnaryOperator" && (S->value("opcode", std::string()) == "++" || S->value("opcode", std::string()) == "--")))
        {
            /* `X op= Y` / `++X` / `X--` as statements are `X = X op Y` / `X = X +- 1`, X located once. */
            Json Wrap;
            bOk = DesugarUpdate(*S, Wrap, nullptr, Err) && LowerBody(Wrap, BP, Out, Locals, Err);
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
            /* C++17 sequences the right side of `=` before the left, but the engine locates a Let's destination
               (its object, array and index) first, and a map store takes its key first. So a value that must not
               move past what locates the destination goes into a local first. */
            bool bLocReads = false, bLocActs = false;
            Locators(*Lhs, bLocReads, bLocActs);
            /* V of a TMap walked in place is a slot of the map's element array, located as an array element is. */
            const Json* Root = Lhs;
            while (Kind(*Root) == "MemberExpr" && !Root->value("isArrow", false) && First(*Root)) Root = PeelLvalue(First(*Root));
            bLocReads = bLocReads || (Kind(*Root) == "DeclRefExpr" && RefPlace.count((*Root)["referencedDecl"].value("id", std::string())));
            Json Parked;
            if (bLocReads && !IsFixedValue(*Rhs) && (bLocActs || !IsPlainRead(*Rhs)))
            {
                Json Pre = Json::array();
                Parked = SynthLocal(StripTypeKeywords(TypeOf(*Rhs)), *Rhs, Pre);
                const Json Wrap = { { "kind", "CompoundStmt" }, { "inner", std::move(Pre) } };
                if (!LowerBody(Wrap, BP, Out, Locals, Err)) { bOk = false; return; }
                Rhs = &Parked;
            }
            const std::string LK = Kind(*Lhs);
            const Json Rooted = LK == "MemberExpr" ? Unalias(*Lhs) : Json();
            if (IsDerefLvalue(*Lhs))
            {
                FArgIR Addr;
                std::string Pointee;
                St.K = FStmtIR::Assign;
                bOk = LowerAddress(*Lhs, BP, Addr, &Pointee, Err) && RefThrough(std::move(Addr), Pointee, St.Var, Err)
                   && LowerArg(*Rhs, BP, St.Value, Err);
            }
            else if (const Json* Elem = LK == "MemberExpr" ? MapElementUnder(Rooted) : nullptr)
            {
                /* `Map[Key].A.B = v` (or `V.A.B = v`, V an `auto& [K, V]`): Map_Find copies the value out, so the store
                   is `T E = Map[Key]; E.A.B = v; Map[Key] = E;`, v first as C++17 sequences it, and the key once. */
                Json Pre = Json::array();
                const Json Value = HoistExpr(*Rhs, Pre);
                const Json Place = StabilizeLvalue(*Elem, Pre);
                const Json E = SynthLocal(StripTypeKeywords(TypeOf(*Elem)), ReadOf(Place), Pre);
                std::function<Json(const Json&)> Reroot = [&](const Json& N) {
                    if (&N == Elem) return Json(E["inner"][0]);
                    Json Up = N;
                    Up["inner"][0] = Reroot(N["inner"][0]);
                    return Up;
                };
                Pre.push_back(AssignOf(Reroot(Rooted), Value));
                Pre.push_back(AssignOf(Place, E));
                const Json Wrap = { { "kind", "CompoundStmt" }, { "inner", std::move(Pre) } };
                bOk = LowerBody(Wrap, BP, Out, Locals, Err);
                return;
            }
            else if (LK == "MemberExpr")
            {
                St.K = FStmtIR::Assign;
                bOk = LowerField(*Lhs, BP, St.Var, Err) && LowerArg(*Rhs, BP, St.Value, Err);
                if (bOk) { SetOn = RecordOfFieldAccess(*Lhs); SetField = St.Var.S; SetObject = St.Var.Base; }
            }
            else if (LK == "DeclRefExpr" && NsVars.count((*Lhs)["referencedDecl"].value("id", std::string())))
            {
                St.K = FStmtIR::Assign;     // a global: its generated class's default object, see LowerGlobal
                bOk = LowerGlobal(*NsVars[(*Lhs)["referencedDecl"].value("id", std::string())], BP, St.Var, Err)
                   && LowerArg(*Rhs, BP, St.Value, Err);
            }
            else if (LK == "DeclRefExpr" && RefPlace.count((*Lhs)["referencedDecl"].value("id", std::string())))
            {
                St.K = FStmtIR::Assign;     // V of a TMap walked in place: its slot's Value
                St.Var = RefPlace[(*Lhs)["referencedDecl"].value("id", std::string())];
                bOk = LowerArg(*Rhs, BP, St.Value, Err);
            }
            else if (LK == "DeclRefExpr")
            {
                const Json& Ref = (*Lhs)["referencedDecl"];
                const std::string RefKind = Ref.value("kind", std::string());
                if (RefKind != "ParmVarDecl" && RefKind != "VarDecl")
                { *Err = "TODO: assignment to a DeclRefExpr of kind " + RefKind; bOk = false; return; }
                if (std::string Why = StaticRefusal(Ref.value("id", std::string()), true); !Why.empty())
                { *Err = std::move(Why); bOk = false; return; }
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
                St.Call.WrittenArgs = ContainerWrites("Map_Add");
                St.Call.bOnArg0 = true;
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
            /* Leaving the body's by-reference TMap range-fors: their values go back first. */
            Out.insert(Out.end(), WriteBacks.rbegin(), WriteBacks.rend());
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
            /* Leaving by-reference TMap range-fors: the value is taken first (it may still change theirs), then each
               loop's value goes back into its map, innermost first. */
            if (bOk && !WriteBacks.empty())
            {
                const std::string RetTy = St.bHasValue ? StripTypeKeywords(TypeOf(*First(*S))) : std::string();
                if (!RetTy.empty() && RetTy != "void")
                {
                    const std::string Temp = "__Ret" + std::to_string(ReadTmpCounter++) + "__";
                    FPropertyDef PD;
                    if (!TypeToProperty(RetTy, Temp, 0, "return value", BP, &PD, Err)) { bOk = false; return; }
                    PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
                    Locals.push_back(PD);
                    FStmtIR Store;
                    Store.K = FStmtIR::Assign;
                    Store.Var.K = FArgIR::Local;
                    Store.Var.S = Temp;
                    Store.Var.LetOp = LetOpFor(RetTy);
                    Store.bAssignLocal = true;
                    Store.Value = std::move(St.Value);
                    Out.push_back(std::move(Store));
                    St.Value = FArgIR();
                    St.Value.K = FArgIR::Local;
                    St.Value.S = Temp;
                }
                Out.insert(Out.end(), WriteBacks.rbegin(), WriteBacks.rend());
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

            /* `if constexpr`: clang has chosen, and an instantiation leaves the discarded branch empty. Only the kept
               one is lowered, so no `JumpIfNot False` chain is left behind (MakeValue<T>'s six-way dispatch). */
            if (FConstVal V; S->value("isConstexpr", false) && FoldConst(*Cond, V))
            {
                if (const Json* Kept = V.Num() != 0 ? Then : Else)
                {
                    Json Wrap = Kind(*Kept) == "CompoundStmt" ? *Kept : Json{ {"kind", "CompoundStmt"}, {"inner", Json::array({ *Kept })} };
                    if (!LowerBody(Wrap, BP, Out, Locals, Err)) bOk = false;
                }
                return;
            }

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

            /* A negation is a Kismet call (Not_PreBool) that the jumps can absorb. `if (C) break;` jumps straight out
               on `!C` when that is free (FreeNegation), `if (A || B) break;` is two of those, and `if (!X) A else B`
               is `if (X) B else A`. */
            const Json* Lone = Kind(*Then) == "CompoundStmt" && Then->contains("inner") && (*Then)["inner"].size() == 1
                             ? Nth(*Then, 0) : Then;
            const bool bLoneJump = !bCurNoOpt && !Else && Lone && (Kind(*Lone) == "BreakStmt" || Kind(*Lone) == "ContinueStmt");
            const Json* C = Strip(Cond);
            if (bLoneJump && Kind(*C) == "BinaryOperator" && C->value("opcode", std::string()) == "||")
            {
                Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({
                    { {"kind", "IfStmt"}, {"inner", Json::array({ *Nth(*C, 0), *Lone })} },
                    { {"kind", "IfStmt"}, {"inner", Json::array({ *Nth(*C, 1), *Lone })} } })} };
                if (!LowerBody(Wrap, BP, Out, Locals, Err)) bOk = false;
                return;
            }
            Json Neg;
            const bool bSwap = !bCurNoOpt && Kind(*C) == "UnaryOperator" && C->value("opcode", std::string()) == "!"
                            && First(*C) && !FreeNegation(*First(*C), Neg);
            St.K = FStmtIR::If;
            St.bJumpOut = bLoneJump && FreeNegation(*Cond, Neg);
            if (!LowerArg(St.bJumpOut ? Neg : bSwap ? *First(*C) : *Cond, BP, St.Cond, Err)) { bOk = false; return; }

            /* A single-statement branch is wrapped in a synthetic CompoundStmt. */
            auto LowerBranch = [&](const Json& Branch, std::shared_ptr<std::vector<FStmtIR>>& OutBody) -> bool
            {
                OutBody = std::make_shared<std::vector<FStmtIR>>();
                if (Kind(Branch) == "CompoundStmt") return LowerBody(Branch, BP, *OutBody, Locals, Err);
                Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({Branch})} };
                return LowerBody(Wrap, BP, *OutBody, Locals, Err);
            };
            const bool bFlip = bSwap && !St.bJumpOut;
            if (!LowerBranch(*Then, bFlip ? St.Else : St.Then)) { bOk = false; return; }
            if (Else && !LowerBranch(*Else, bFlip ? St.Then : St.Else)) { bOk = false; return; }
            if (!St.Then) St.Then = std::make_shared<std::vector<FStmtIR>>();
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
            /* The promoted value of a byte is 0..255 (-128..127 signed): a case outside that never matches, and its
               ByteConst would wrap onto one that does. */
            const std::string ByteTy = Width == 1 ? StripTypeKeywords(TypeOf(*Strip(Cond))) : std::string();
            const bool bSignedByte = ByteTy == "int8" || ByteTy == "signed char" || ByteTy == "char";

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
            /* A byte-sized value is stored as the byte it is. The widening to int around it (C++'s promotion, or a cast)
               would evaluate a 4-byte result into the 1-byte temp: EX_Let evaluates straight into the variable. */
            const Json* Stored = Cond;
            while (Width == 1 && First(*Stored) && (Kind(*Stored) == "ParenExpr"
                   || (Stored->value("castKind", std::string()) == "IntegralCast" && SwitchWidth(TypeOf(*First(*Stored))) == 1)))
                Stored = First(*Stored);
            if (!LowerArg(*Stored, BP, Store.Value, Err)) { bOk = false; return; }
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
                    else if (Width == 1)
                    {
                        if (V < (bSignedByte ? -128 : 0) || V > (bSignedByte ? 127 : 255))
                        {
                            L.LabelId = -1;                 // reached only by falling through
                            St.Body->push_back(L);
                            return !Sub || Flatten(*Sub);
                        }
                        Const.K = FArgIR::Byte; Const.I = int32(V);
                    }
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
        else if (K == "NullStmt") return;   // `while (C);`, `[[fallthrough]];`, `Label: ;`
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
            if (!LowerCond(*Cond)) return;
            St.Body = std::make_shared<std::vector<FStmtIR>>();
            Json Wrap = Kind(*Body) == "CompoundStmt" ? *Body
                      : Json{ {"kind", "CompoundStmt"}, {"inner", Json::array({*Body})} };
            ++LoopDepth;
            if (!LowerBody(Wrap, BP, *St.Body, Locals, Err)) { bOk = false; return; }
            --LoopDepth;
        }
        else if (K == "GotoStmt")
        {
            if (!WriteBacks.empty())
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
            if (!LowerCond(*Cond)) return;
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
               [init, condVar, cond, inc, body], clang writing an absent part as {}. No condition is `true`. */
            auto AtOr = [&](size_t I) -> const Json* {
                const Json* P = Nth(*S, I);
                return (P && !P->is_null() && !P->empty()) ? P : nullptr;
            };
            const Json True = { {"kind", "CXXBoolLiteralExpr"}, {"type", { {"qualType", "bool"} }}, {"value", true} };
            const Json* Init = AtOr(0);
            const Json* Cond = AtOr(2) ? AtOr(2) : &True;
            const Json* Inc = AtOr(3);
            const Json* Body = AtOr(4);
            if (!Body) { *Err = "`for` with no body"; bOk = false; return; }
            if (const Json* Var = AtOr(1))
            {
                /* `for (Init; T V = E; Inc) Body`: V is made again each time round, as in
                   `for (Init;; Inc) { T V = E; if (!V) break; Body }`, where a `continue` still reaches Inc. */
                Json Not = { {"kind", "UnaryOperator"}, {"opcode", "!"}, {"type", { {"qualType", "bool"} }}, {"inner", Json::array({*Cond})} };
                Json Exit = { {"kind", "IfStmt"}, {"inner", Json::array({ Not, Json{ {"kind", "BreakStmt"} } })} };
                Json Loop = *S;
                Loop["inner"][1] = Loop["inner"][2] = Json::object();
                Loop["inner"][4] = { {"kind", "CompoundStmt"}, {"inner", Json::array({ *Var, Exit, *Body })} };
                Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({ Loop })} };
                if (!LowerBody(Wrap, BP, Out, Locals, Err)) bOk = false;
                return;
            }

            if (Init)
            {
                Json WrapInit = { {"kind", "CompoundStmt"}, {"inner", Json::array({*Init})} };
                if (!LowerBody(WrapInit, BP, Out, Locals, Err)) { bOk = false; return; }
            }

            St.K = FStmtIR::While;
            if (!LowerCond(*Cond)) return;
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
        /* The flush, the write and the notify each name the object: one that is computed (`Me()->Score = 9`) goes into a
           local once. SetObject is the write's own Base, so all three see the local. A parked right side is already out. */
        if (bOk && (bFlush || !Notify.empty()) && SetObject && !IsStored(*SetObject)
            && !HoistOperand(*SetObject, BP, Locals, Out, Err)) { bOk = false; return; }
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
            Call.Call.bLocal = !SetObject;
            Out.push_back(std::move(Call));
        }
    });
    return bOk;
}

/* Every use of the declaration Id in N is a plain read (an lvalue-to-rvalue conversion), so nothing writes it,
   binds a reference to it or takes its address. `C ? A : B` over two lvalues is an lvalue itself, converted as a
   whole: A and B are read when it is. */
bool OnlyRead(const Json& N, const std::string& Id)
{
    bool bOk = true;
    std::function<void(const Json&, bool)> Walk = [&](const Json& X, bool bRead) {
        if (!bOk || !X.is_object()) return;
        const std::string K = Kind(X);
        if (K == "DeclRefExpr" && X.contains("referencedDecl") && X["referencedDecl"].value("id", std::string()) == Id)
            bOk = bRead;
        const bool bToRvalue = K == "ImplicitCastExpr" && X.value("castKind", std::string()) == "LValueToRValue";
        int32 I = 0;
        ForEach(X, [&](const Json& C) {
            const bool bBranch = K == "ConditionalOperator" && I++ > 0;
            Walk(C, bToRvalue || (bRead && (bBranch || K == "ParenExpr")));
        });
    };
    Walk(N, false);
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
int32 Mentions(const FStmtIR& St, const std::string& Name)
{
    int32 N = Mentions(St.Target, Name) + Mentions(St.Call, Name) + Mentions(St.Var, Name);
    for (const FArgIR* A : { &St.Value, &St.Cond, &St.SwitchValue }) N += Mentions(*A, Name);
    for (const FArgIR& A : St.CaseTests) N += Mentions(A, Name);
    for (const auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer }) if (*L) N += Mentions(**L, Name);
    return N;
}
int32 Mentions(const std::vector<FStmtIR>& Stmts, const std::string& Name)
{
    int32 N = 0;
    for (const FStmtIR& St : Stmts) N += Mentions(St, Name);
    return N;
}

/* C may bind argument I to a reference: one it writes (a T& parameter, a container method's array or out value), or
   one it reads where the argument lies (RefParms), which must stay a variable unless Value, what would go there
   instead, is a constant a native reads from its own buffer (see HoistCallArgs). */
bool MayWriteArg(const FCallIR& C, size_t I, const FArgIR* Value = nullptr)
{
    if (I < C.RefParms.size() && !C.RefParms[I].empty() && !(Value && C.bRefsTakeConst && IsVmConstant(*Value)))
        return true;
    return !C.bPure && !IsBranch(C.Intrinsic) && (I >= 64 || (C.WrittenArgs >> I & 1));
}

/* The read of local Name that runs exactly once whenever A does: not under a branch's later operands, an inline
   body, an object or struct base (which may need a variable), or a call's target. Nor an argument bRefSlot says may
   be written: the variable is the argument there, and another in its place would take the write. Value: what is to
   replace the read. */
FArgIR* FindPlainRead(FArgIR& A, const std::string& Name, const FArgIR* Value, bool bRefSlot = false)
{
    if (A.K == FArgIR::Local && A.S == Name && !A.Base) return bRefSlot ? nullptr : &A;
    if (A.K != FArgIR::Call || !A.Sub || A.Sub->Inline) return nullptr;
    const size_t Count = IsBranch(A.Sub->Intrinsic) ? std::min<size_t>(1, A.Sub->Args.size()) : A.Sub->Args.size();
    for (size_t I = 0; I < Count; ++I)
        if (FArgIR* F = FindPlainRead(A.Sub->Args[I], Name, Value, MayWriteArg(*A.Sub, I, Value))) return F;
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

/* Whether evaluating A may store to local Name: an argument its call may write, or anything an inline body in it names. */
bool MayStore(const FArgIR& A, const std::string& Name);
bool MayStore(const FCallIR& C, const std::string& Name)
{
    if ((C.Inline && Mentions(*C.Inline, Name) != 0) || C.InlineResult == Name) return true;
    if (C.Target && MayStore(*C.Target, Name)) return true;
    for (size_t I = 0; I < C.Args.size(); ++I)
        if ((MayWriteArg(C, I) && Mentions(C.Args[I], Name) != 0) || MayStore(C.Args[I], Name)) return true;
    return false;
}
bool MayStore(const FArgIR& A, const std::string& Name)
{
    return (A.Sub && MayStore(*A.Sub, Name)) || (A.Base && MayStore(*A.Base, Name));
}

/* Whether X comes to the same value before E runs as after it: constants, the pointer self or a fixed object is, locals
   E cannot store to, elements and struct members of those, and pure functions of such values. Not an object's property,
   a reference parameter (the caller's variable, perhaps an object's), nor a pure call on an object (a getter): E may
   change those. */
bool Unaffected(const FArgIR& X, const FArgIR& E)
{
    auto Value = [&](const FArgIR& A) {
        return A.K != FArgIR::Self && A.K != FArgIR::ObjConst && !IsObjectType(A.InnerType) && Unaffected(A, E);
    };
    switch (X.K)
    {
    case FArgIR::Int: case FArgIR::Int64: case FArgIR::Float: case FArgIR::Bool: case FArgIR::Byte: case FArgIR::Str:
    case FArgIR::Name: case FArgIR::Text: case FArgIR::Self: case FArgIR::NullObj: case FArgIR::ObjConst: case FArgIR::SoftPath:
        return true;
    case FArgIR::Local:
        return !X.Base && !MayStore(E, X.S);
    case FArgIR::Member:
        return X.Base && Unaffected(*X.Base, E);
    case FArgIR::Index:
        return X.Base && X.Base->K == FArgIR::Local && !X.Base->Base && !MayStore(E, X.Base->S) && X.Sub
            && std::all_of(X.Sub->Args.begin(), X.Sub->Args.end(), [&](const FArgIR& A) { return Unaffected(A, E); });
    case FArgIR::Call:
        return X.Sub && X.Sub->bPure && X.Sub->Intrinsic.empty() && !X.Sub->Inline && !X.Sub->Target
            && std::all_of(X.Sub->Args.begin(), X.Sub->Args.end(), Value);
    default:
        return false;
    }
}

/* What may run before E instead of after it: nothing that acts, and when E acts, nothing that reads what E could
   change. */
bool MayRunFirst(const FArgIR& X, const FArgIR& E, bool bActs)
{
    return !CallsImpure(X) && (!bActs || Unaffected(X, E));
}

/* Whether E reads something it does not name: a pointer read reads the scratch an earlier statement pointed; an inline
   body is statements of its own. */
bool HidesReads(const FArgIR& E)
{
    if (E.Base && HidesReads(*E.Base)) return true;
    if (!E.Sub) return false;
    if (!E.Sub->Intrinsic.empty() || E.Sub->Inline || (E.Sub->Target && HidesReads(*E.Sub->Target))) return true;
    return std::any_of(E.Sub->Args.begin(), E.Sub->Args.end(), HidesReads);
}

/* Whether `T = E` may move past S to a read of T after it: S stores to a local of the frame, or to a struct member of
   one, that E neither names nor may store to, and runs nothing; when E acts, S also reads nothing E could change. */
bool MayPass(const FStmtIR& S, const FArgIR& E, bool bActs)
{
    if (!(S.K == FStmtIR::Assign || (S.K == FStmtIR::Decl && S.bHasValue)) || HidesReads(E)) return false;
    const FArgIR* Root = &S.Var;
    while (Root->K == FArgIR::Member && Root->Base) Root = Root->Base.get();
    if (Root->K != FArgIR::Local || Root->Base || Mentions(E, Root->S) != 0 || MayStore(E, Root->S)) return false;
    return MayRunFirst(S.Value, E, bActs);
}

/* A store's destination, which EX_Let locates before it evaluates the value: whether it is the same place with E run
   after it. Nothing in it acts, and when E acts, E cannot move it: a local, a property of self, of a fixed object or of
   the object in a local E cannot store to, or a struct member of such. A pointer's place (__RefAtInline__) reads only its
   scratch, which E does not name. */
bool PlaceWaits(const FArgIR& P, const FArgIR& E, bool bActs)
{
    if (P.K == FArgIR::Call && P.Sub && P.Sub->Intrinsic == "__RefAtInline__") return Mentions(E, P.S) == 0 && !MayStore(E, P.S);
    if (CallsImpure(P)) return false;
    if (!bActs) return true;
    switch (P.K)
    {
    case FArgIR::Local: return !P.Base;
    case FArgIR::Field: return !P.Base || P.Base->K == FArgIR::Self || P.Base->K == FArgIR::ObjConst
                             || (P.Base->K == FArgIR::Local && !P.Base->Base && !MayStore(E, P.Base->S));
    case FArgIR::Member: return P.Base && PlaceWaits(*P.Base, E, true);
    default: return false;
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

        const FArgIR& Arg = Body[K].Value;
        FArgIR* Read = nullptr;
        FArgIR* Scope = nullptr;
        if (First.K == FStmtIR::StaticCall && !First.Target.Target && First.Target.Args.empty())
        {
            for (size_t I = 0; I < First.Call.Args.size() && !Read; ++I)
                if ((Read = FindPlainRead(First.Call.Args[I], Name, &Arg, MayWriteArg(First.Call, I, &Arg))))
                    Scope = &First.Call.Args[I];
        }
        else if ((First.K == FStmtIR::Assign || First.K == FStmtIR::Decl || First.K == FStmtIR::Return) && !First.Var.Base)
            Read = FindPlainRead(*(Scope = &First.Value), Name, &Arg);
        else if (First.K == FStmtIR::If)
            Read = FindPlainRead(*(Scope = &First.Cond), Name, &Arg);
        if (!Read) return;

        const bool bActs = CallsImpure(Arg);
        const std::function<bool(const FArgIR&)> Pred = [&](const FArgIR& X) { return MayRunFirst(X, Arg, bActs); };
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

/*
`T = E; <S reads T>`, T read nowhere else: E goes where S reads T, and T is gone (DropUnusedLocals takes the property).
The same conditions as ArgumentsInPlace: the read runs exactly once whenever S does, and running E there instead of
where it was cannot be seen. What S evaluates before the read, its destination included, runs nothing, and when E acts
(a call with an out parameter is never pure, so it acts), reads nothing E could change. S need not be the next
statement: E moves past stores to frame locals that it does not read and that run nothing (MayPass), which is what
a pointer store puts between a value and its use (`__Upd = V; Scratch.Num = 1; Scratch.Data = P; *view = __Upd`).
*/
void FCompiler::ForwardSingleUse(std::vector<FStmtIR>& Stmts, const std::vector<FStmtIR>& All)
{
    for (FStmtIR& St : Stmts)
        for (auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer })
            if (*L)
            {
                *L = std::make_shared<std::vector<FStmtIR>>(**L);
                ForwardSingleUse(**L, All);
            }
    /* Last to first, so `A = ..; B = ..; S(A, B)` forwards B, then A into what S has become. */
    for (size_t D = Stmts.size(); D-- > 0;)
    {
        const FStmtIR& Def = Stmts[D];
        const std::string& Name = Def.Var.S;
        if (!((Def.K == FStmtIR::Decl && Def.bHasValue) || (Def.K == FStmtIR::Assign && Def.bAssignLocal))
            || Def.Var.K != FArgIR::Local || Def.Var.Base || !CurLocals
            || std::none_of(CurLocals->begin(), CurLocals->end(), [&](const FPropertyDef& L) { return L.Name == Name; })
            || Mentions(All, Name) != 2 || Mentions(Def.Value, Name) != 0)
            continue;
        const FArgIR& E = Def.Value;
        const bool bActs = CallsImpure(E);
        size_t U = D + 1;
        while (U < Stmts.size() && Mentions(Stmts[U], Name) == 0 && MayPass(Stmts[U], E, bActs)) ++U;
        if (U == Stmts.size()) continue;
        FStmtIR& Next = Stmts[U];
        FArgIR* Read = nullptr;
        FArgIR* Scope = nullptr;
        if (Next.K == FStmtIR::StaticCall && !Next.Target.Target && Next.Target.Args.empty())
        {
            for (size_t I = 0; I < Next.Call.Args.size() && !Read; ++I)
                if ((Read = FindPlainRead(Next.Call.Args[I], Name, &E, MayWriteArg(Next.Call, I, &E))))
                    Scope = &Next.Call.Args[I];
        }
        else if ((Next.K == FStmtIR::Assign || Next.K == FStmtIR::Decl || Next.K == FStmtIR::Return)
                 && (!Next.Var.Base || PlaceWaits(Next.Var, E, bActs)))
            Read = FindPlainRead(*(Scope = &Next.Value), Name, &E);
        else if (Next.K == FStmtIR::If)
            Read = FindPlainRead(*(Scope = &Next.Cond), Name, &E);
        if (!Read) continue;

        const std::function<bool(const FArgIR&)> Pred = [&](const FArgIR& X) { return MayRunFirst(X, E, bActs); };
        bool bOk = OffPath(*Scope, Read, Pred);
        if (Next.K == FStmtIR::StaticCall)
        {
            if (Next.Call.Target) bOk = bOk && Pred(*Next.Call.Target);
            for (const FArgIR& A : Next.Call.Args) if (&A != Scope) bOk = bOk && Pred(A);
        }
        if (!bOk) continue;
        *Read = E;
        Stmts.erase(Stmts.begin() + D);
    }
}

/* An inline body with no early return is just its statements: no jump lands past it, and a break in it would have
   been refused. Spliced into the list around it, its result local meets its use (ForwardSingleUse). */
static bool HasInlineReturn(const std::vector<FStmtIR>& Stmts)
{
    for (const FStmtIR& St : Stmts)
    {
        if (St.K == FStmtIR::InlineReturn) return true;
        if (St.K != FStmtIR::Block)
            for (const auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer })
                if (*L && HasInlineReturn(**L)) return true;
    }
    return false;
}

void FCompiler::FlattenBlocks(std::vector<FStmtIR>& Stmts)
{
    for (size_t I = 0; I < Stmts.size(); ++I)
    {
        FStmtIR& St = Stmts[I];
        for (auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer })
            if (*L)
            {
                *L = std::make_shared<std::vector<FStmtIR>>(**L);
                FlattenBlocks(**L);
            }
        if (St.K != FStmtIR::Block || (St.Body && HasInlineReturn(*St.Body))) continue;
        const std::vector<FStmtIR> Body = St.Body ? *St.Body : std::vector<FStmtIR>();
        Stmts.erase(Stmts.begin() + I);
        Stmts.insert(Stmts.begin() + I, Body.begin(), Body.end());
        I += Body.size();
        --I;
    }
}

/*
`if (Found(X))` over an inline bool function whose every return is a constant: each `return true` / `return false`
jumps straight to the branch it picks, where it stored the value for the `if` to test. The body's own end, a constant
too, falls into its branch, laid out first; the other follows behind a jump, and goes when no return picks it. A branch
that is a lone `return` of a constant or a variable is copied to the returns that pick it. Only forward jumps, into
code after the body: what CoalesceTemps then sees of each local's span is still where it is live.
*/
void FCompiler::ThreadBranches(std::vector<FStmtIR>& Stmts, const std::vector<FStmtIR>& All, std::vector<FPropertyDef>& Locals)
{
    for (size_t I = 0; I < Stmts.size(); ++I)
    {
        for (auto* L : { &Stmts[I].Then, &Stmts[I].Else, &Stmts[I].Body, &Stmts[I].Inc, &Stmts[I].Trailer })
            if (*L)
            {
                *L = std::make_shared<std::vector<FStmtIR>>(**L);
                ThreadBranches(**L, All, Locals);
            }
        if (Stmts[I].K != FStmtIR::Block || !Stmts[I].Body || Stmts[I].Body->empty() || I + 1 == Stmts.size()) continue;
        const FStmtIR If = Stmts[I + 1];
        if (If.K != FStmtIR::If || If.bJumpOut || If.Cond.K != FArgIR::Local || If.Cond.Base
            || (If.Then && HasLabel(*If.Then)) || (If.Else && HasLabel(*If.Else)))
            continue;
        const std::string R = If.Cond.S;
        auto Store = [&](const FStmtIR& St) {
            return St.K == FStmtIR::Assign && St.Var.K == FArgIR::Local && !St.Var.Base && St.Var.S == R && St.Value.K == FArgIR::Bool;
        };
        /* Every return of this body (not of an inline inside it) right after a constant store to R. */
        int32 Stores = 0;
        std::function<bool(const std::vector<FStmtIR>&)> Exits = [&](const std::vector<FStmtIR>& List) {
            for (size_t K = 0; K < List.size(); ++K)
            {
                if (List[K].K == FStmtIR::InlineReturn && (K == 0 || !Store(List[K - 1]))) return false;
                Stores += List[K].K == FStmtIR::InlineReturn;
                if (List[K].K != FStmtIR::Block)
                    for (const auto* L : { &List[K].Then, &List[K].Else, &List[K].Body, &List[K].Inc, &List[K].Trailer })
                        if (*L && !Exits(**L)) return false;
            }
            return true;
        };
        std::vector<FStmtIR>& Body = *Stmts[I].Body;
        if (!Store(Body.back()) || !Exits(Body) || Mentions(Body, R) != Stores + 1 || Mentions(All, R) != Stores + 2) continue;

        const bool bEnd = Body.back().Value.B;
        auto Branch = [&](bool C) { return C ? If.Then : If.Else; };
        auto Lone = [&](bool C) {
            const auto B = Branch(C);
            return B && B->size() == 1 && (*B)[0].K == FStmtIR::Return
                && (!(*B)[0].bHasValue || IsVmConstant((*B)[0].Value) || ((*B)[0].Value.K == FArgIR::Local && !(*B)[0].Value.Base));
        };
        int32 Label[2] = { -1, -1 };
        std::function<void(std::vector<FStmtIR>&)> Rewrite = [&](std::vector<FStmtIR>& List) {
            for (size_t K = 0; K < List.size(); ++K)
            {
                if (List[K].K == FStmtIR::InlineReturn)
                {
                    const bool C = List[K - 1].Value.B;
                    FStmtIR Jump;
                    if (Lone(C)) Jump = (*Branch(C))[0];
                    else
                    {
                        if (Label[C] < 0) Label[C] = NextGotoLabel++;
                        Jump.K = FStmtIR::Goto;
                        Jump.LabelId = Label[C];
                    }
                    List.erase(List.begin() + K - 1);
                    List[--K] = std::move(Jump);
                    continue;
                }
                if (List[K].K != FStmtIR::Block)
                    for (auto* L : { &List[K].Then, &List[K].Else, &List[K].Body, &List[K].Inc, &List[K].Trailer })
                        if (*L)
                        {
                            *L = std::make_shared<std::vector<FStmtIR>>(**L);
                            Rewrite(**L);
                        }
            }
        };
        Body.pop_back();
        Rewrite(Body);

        /* The branch the end falls into, then the other one if a return jumps there. */
        std::vector<FStmtIR> Seq;
        auto Mark = [](int32 Id) { FStmtIR L; L.K = FStmtIR::GotoLabel; L.LabelId = Id; return L; };
        auto Lay = [&](bool C) {
            if (Label[C] >= 0) Seq.push_back(Mark(Label[C]));
            if (const auto B = Branch(C)) Seq.insert(Seq.end(), B->begin(), B->end());
        };
        Lay(bEnd);
        if (Label[!bEnd] >= 0)
        {
            int32 End = -1;
            if (!NeverFallsThrough(Seq))
            {
                FStmtIR Over;
                Over.K = FStmtIR::Goto;
                Over.LabelId = End = NextGotoLabel++;
                Seq.push_back(std::move(Over));
            }
            Lay(!bEnd);
            if (End >= 0) Seq.push_back(Mark(End));
        }
        Stmts.erase(Stmts.begin() + I + 1);
        Stmts.insert(Stmts.begin() + I + 1, Seq.begin(), Seq.end());     // walked next, for a pattern of their own
        Locals.erase(std::remove_if(Locals.begin(), Locals.end(), [&](const FPropertyDef& L) { return L.Name == R; }), Locals.end());
    }
}

/* Whether the first thing Stmts does to local Name, on every path, is store a whole new value that does not read it. */
static bool StoresFirst(const std::vector<FStmtIR>& Stmts, const std::string& Name)
{
    if (Stmts.empty()) return false;
    const FStmtIR& St = Stmts.front();
    if ((St.K == FStmtIR::Assign || (St.K == FStmtIR::Decl && St.bHasValue)) && St.Var.K == FArgIR::Local && !St.Var.Base
        && St.Var.S == Name)
        return Mentions(St.Value, Name) == 0;
    return St.K == FStmtIR::If && !St.bJumpOut && Mentions(St.Cond, Name) == 0 && St.Then && St.Else
        && StoresFirst(*St.Then, Name) && StoresFirst(*St.Else, Name);
}

/* `int32 R = 0; if (C) R = 1; else R = 2;`: a store to a local that the next statement overwrites on every path before
   reading it does nothing, unless its value calls something that acts. */
void FCompiler::DropOverwritten(std::vector<FStmtIR>& Stmts)
{
    for (size_t I = 0; I < Stmts.size(); ++I)
    {
        FStmtIR& St = Stmts[I];
        for (auto* L : { &St.Then, &St.Else, &St.Body, &St.Inc, &St.Trailer })
            if (*L)
            {
                *L = std::make_shared<std::vector<FStmtIR>>(**L);
                DropOverwritten(**L);
            }
        if ((St.K == FStmtIR::Assign || (St.K == FStmtIR::Decl && St.bHasValue)) && St.Var.K == FArgIR::Local && !St.Var.Base
            && !CallsImpure(St.Value) && I + 1 < Stmts.size()
            && StoresFirst(std::vector<FStmtIR>(Stmts.begin() + I + 1, Stmts.begin() + I + 2), St.Var.S))
            Stmts.erase(Stmts.begin() + I--);
    }
}

bool FCompiler::IsInlineMethod(const FRecord& R, const std::string& Method) const
{
    auto Decl = R.Methods.find(Method);
    auto Def = R.MethodDefs.find(Method);
    return (Decl != R.Methods.end() && Decl->second->value("inline", false))
        || (Def != R.MethodDefs.end() && Def->second->value("inline", false));
}

/* A class's variables and functions share one FName namespace with its ancestors', and an FName ignores case. The
   editor refuses a duplicate function and renames a clashing variable (KismetCompiler.cpp 570-616, 1737-1747); here
   each is refused: two members of one name (overloads included, inline ones aside, which are no UFunction), two that
   differ only in case, and a member reusing an inherited name - save a function overriding one of the same spelling. */
bool FCompiler::CheckMemberNames(const FRecord& R, std::string* Err) const
{
    std::map<std::string, std::pair<std::string, bool>> Own;       // lower-case name -> (as written, is a function)
    auto Claim = [&](const std::string& N, bool bFunction) {
        auto [It, bNew] = Own.emplace(Lower(N), std::make_pair(N, bFunction));
        if (bNew) return true;
        *Err = R.CppName + "::" + N + (It->second.first == N
            ? std::string(": a second ") + (bFunction ? "function" : "member") + " of that name; a Blueprint class has one "
              "member per name, so rename one"
            : ": differs from " + It->second.first + " only in case, and an FName ignores case; rename one");
        return false;
    };
    for (const Json* F : R.Fields) if (!Claim(Name(*F), false)) return false;
    for (const Json* M : R.AllMethods)
        if (!M->value("inline", false) && !IsInlineMethod(R, Name(*M)) && !Claim(Name(*M), true)) return false;
    /* A class's latent calls resume through its ubergraph, found by name on the object (most derived first), so a
       function of that name in this class or a subclass would catch its resumes. */
    for (const Json* M : R.AllMethods)
        if (Lower(Name(*M)).compare(0, 17, "executeubergraph_") == 0)
        { *Err = R.CppName + "::" + Name(*M) + ": ExecuteUbergraph_<Class> is the name of a class's ubergraph, whose latent "
                 "calls resume through it; rename it"; return false; }
    if (R.bIsStruct) return true;
    /* A component is an object named after its variable under the actor: DefaultSceneRoot is the node the SCS adds
       when no component of its own is a scene root, and a native default subobject is created under its own name
       before any SCS node, so a second object of that name is refused (the loader finds objects by outer and name). */
    for (const std::string& C : R.Components)
        if (Lower(C) == "defaultsceneroot")
        { *Err = R.CppName + "::" + C + ": DefaultSceneRoot is the root the construction script adds; rename the component"; return false; }
    for (const FRecord* A = R.Base.empty() ? nullptr : Find(R.Base); A; A = A->Base.empty() ? nullptr : Find(A->Base))
        for (const auto& [Member, Spec] : A->Subobjects)
            for (const std::string& C : R.Components)
                if (Lower(C) == Lower(Spec.substr(0, Spec.find(' '))))
                { *Err = R.CppName + "::" + C + ": " + A->CppName + " already has a default subobject " + Spec.substr(0, Spec.find(' '))
                         + " (its " + Member + "); rename the component"; return false; }
    for (const FRecord* A = R.Base.empty() ? nullptr : Find(R.Base); A; A = A->Base.empty() ? nullptr : Find(A->Base))
    {
        auto Inherited = [&](const std::string& CppN, bool bFunction) {
            auto U = A->UeNames.find(CppN);
            const std::string N = U == A->UeNames.end() ? CppN : U->second;
            auto It = Own.find(Lower(N));
            if (It == Own.end() || (bFunction && It->second.second && It->second.first == N)) return true;   // an override
            *Err = R.CppName + "::" + It->second.first + ": " + A->CppName + " already has " + (bFunction ? "a function " : "a variable ")
                 + N + ", and an FName ignores case; rename it";
            return false;
        };
        for (const Json* F : A->Fields) if (!Inherited(Name(*F), false)) return false;
        for (const auto& [N, D] : A->Methods)
            if (!D->value("inline", false) && !IsInlineMethod(*A, N) && !Inherited(N, true)) return false;
    }
    return true;
}

/* Whether a body holds a `goto`, which can run a declaration again the way a loop does. */
bool HasGoto(const Json& N)
{
    if (Kind(N) == "GotoStmt") return true;
    bool bFound = false;
    ForEach(N, [&](const Json& C) { bFound = bFound || HasGoto(C); });
    return bFound;
}

const FRecord* FCompiler::FinalOwner(const FRecord* Of, const std::string& Method) const
{
    for (const FRecord* A = Of; A; A = A->Base.empty() ? nullptr : Find(A->Base))
        if (auto M = A->Methods.find(Method); M != A->Methods.end() && !IsInlineMethod(*A, Method))
            return (Of->bFinal || A->FinalMethods.count(Method)) && !IsStaticDecl(*M->second) ? A : nullptr;
    return nullptr;
}

/* Every call whose body is known here expands, unless: the function is not a Blueprint function of this mod with a
   body; it or an ancestor's version is an RPC, authority only or cosmetic, or overrides an engine function (the
   engine's routing must see the call); it is noinline or UE_NO_OPTIMIZE, or the caller is UE_NO_OPTIMIZE; it is
   the function being compiled or expanded already (recursion stays a call); it makes a latent call or an await,
   which would move the caller into the ubergraph, or holds a goto, which turns the caller's optimizer off; or the
   call does not match it, an overload's or another class's version with other parameters. */
const Json* FCompiler::Expandable(const FRecord& In, const std::string& Method, const Json& Call, const Json* Picked) const
{
    auto Decl = In.Methods.find(Method);
    if (bCurNoOpt || !In.IsGenerated() || In.bIsStruct || In.bIsInterface || Decl == In.Methods.end() || IsInlineMethod(In, Method))
        return nullptr;
    auto DefIt = In.MethodDefs.find(Method);
    const Json* Def = DefIt != In.MethodDefs.end() ? DefIt->second : Decl->second;
    const Json* Body = nullptr;
    ForEach(*Def, [&](const Json& C) { if (Kind(C) == "CompoundStmt") Body = &C; });
    if (!Body || Def == CurFnDef || std::find(InlineStack.begin(), InlineStack.end(), Def) != InlineStack.end()) return nullptr;
    for (const Json* D : { Decl->second, Def })
        if (HasAttr(*D, "NoInlineAttr") || IsNoOptDecl(*D)) return nullptr;
    for (const FRecord* A = &In; A; A = A->Base.empty() ? nullptr : Find(A->Base))
    {
        auto M = A->Methods.find(Method);
        if (M == A->Methods.end()) continue;
        if (A->IsNative() || NetFlagsOf(*M->second) || AccessFlagsOf(*M->second)) return nullptr;
        if (auto D = A->MethodDefs.find(Method); D != A->MethodDefs.end() && (NetFlagsOf(*D->second) || AccessFlagsOf(*D->second)))
            return nullptr;
    }
    auto Types = [](const Json& D) {
        std::vector<std::string> T;
        ForEach(D, [&](const Json& C) { if (Kind(C) == "ParmVarDecl") T.push_back(TypeOf(C)); });
        return T;
    };
    if (Types(*Def).size() + 1 != (Call.contains("inner") ? Call["inner"].size() : 0)) return nullptr;
    if (Picked && Picked != Decl->second && Types(*Picked) != Types(*Decl->second)) return nullptr;
    std::set<const Json*> Seen;
    return HasGoto(*Body) || ResumesLater(*Body, Seen) ? nullptr : Def;
}

/* Whether running N makes a latent call or an await of its own, itself or in an inline body it always expands: a call
   to a function with an FLatentActionInfo parameter (genueapi's overload leaves it out, the UFunction, the longest
   overload, has it) or to __Await__. A call to a Blueprint function of the mod does not: it returns at its first. */
bool FCompiler::ResumesLater(const Json& N, std::set<const Json*>& Seen) const
{
    const std::string K = Kind(N);
    if (const Json* Callee = K == "CallExpr" || K == "CXXMemberCallExpr" ? Strip(First(N)) : nullptr)
    {
        const bool bMember = Kind(*Callee) == "MemberExpr", bRef = Kind(*Callee) == "DeclRefExpr";
        const Json* Ref = bRef && Callee->contains("referencedDecl") ? &(*Callee)["referencedDecl"] : nullptr;
        const std::string Id = bMember ? Callee->value("referencedMemberDecl", std::string()) : Ref ? Ref->value("id", std::string()) : "";
        const std::string Fn = bMember ? Name(*Callee) : Ref ? Name(*Ref) : "";
        if (Fn == "__Await__") return true;
        const Json* Always = nullptr;
        if (auto F = FreeInlines.find(Id); F != FreeInlines.end()) Always = F->second;
        else if (auto T = MemberTemplates.find(Id); T != MemberTemplates.end()) Always = T->second;
        else if (auto O = MethodOwner.find(Id); O != MethodOwner.end())
            if (const FRecord* R = Find(O->second))
            {
                if (auto I = R->Inlines.find(Id); I != R->Inlines.end()) Always = I->second;
                else if (auto M = R->Methods.find(Fn); M != R->Methods.end())
                {
                    bool bLatent = false;
                    ForEach(*M->second, [&](const Json& C) {
                        bLatent = bLatent || (Kind(C) == "ParmVarDecl" && StripTypeKeywords(TypeOf(C)) == "FLatentActionInfo");
                    });
                    if (bLatent) return true;
                }
            }
        if (Always && Seen.insert(Always).second && ResumesLater(*Always, Seen)) return true;
    }
    bool bFound = false;
    ForEach(N, [&](const Json& C) { bFound = bFound || ResumesLater(C, Seen); });
    return bFound;
}

/* The call becomes one Block statement in Out.Inline:
       <each by-value parameter> = <its argument>;
       <the body, locals renamed __Inl<N>_<name>, `return X` as `__Inl<N>_ReturnValue = X` + a jump to the end>
   A reference parameter bound to a place (a variable, `O->A`, `Arr[I]`) is another name for it; bound to a map element
   or `C ? X : Y`, a copy stored back after the body when the body writes it; bound to a value, a copy.
   Only calls on `this` (or a static) expand, since the body's `this` stays the caller's self. */
bool FCompiler::ExpandInline(const Json& CallNode, const Json& Def, const std::string& Method, bool bMethod, FBlueprintClass& BP,
                             FCallIR& Out, std::string* Err, const Json* Receiver, bool bStaticCall)
{
    if (!CurLocals) { *Err = "internal: an inline call outside a function body"; return false; }
    if (std::find(InlineStack.begin(), InlineStack.end(), &Def) != InlineStack.end())
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
    if (Receiver) Args.push_back(Receiver);     // a forwarded method: the object is the free function's first parameter
    ForEach(CallNode, [&](const Json& C) { if (bFirst) { bFirst = false; return; } Args.push_back(&C); });
    if (Args.size() != Parms.size()) { *Err = "inline call to " + Method + " with " + std::to_string(Args.size()) + " arguments"; return false; }
    /* A world context left to its default (`UObject* WorldContextObject = nullptr`) is the caller's, as for a
       UFunction (LowerCall): the editor wires the hidden pin to self. */
    std::vector<bool> WcoDefault(Args.size());
    for (size_t I = 0; I < Args.size(); ++I) WcoDefault[I] = Kind(*Args[I]) == "CXXDefaultArgExpr" && IsWcoName(Name(*Parms[I]));
    for (size_t I = 0; I < Args.size(); ++I) Args[I] = DefaultedArg(*Args[I], Parms[I]);
    /* A reference parameter is another name for a variable, or for a place under an object or an index (`O->A`,
       `Arr[F()]`, `O->S.X`) fixed at the call, as binding a reference fixes it. */
    auto Aliased = [&](size_t I) -> const Json* {
        const std::string Type = TypeOf(*Parms[I]);
        const Json* Bare = PeelLvalue(Args[I]);
        if (Type.empty() || Type.back() != '&' || !Bare || IsDerefLvalue(*Bare)) return nullptr;
        return IsAliasable(*Bare) || IsAliasable(Unalias(*Bare)) || IsPinnable(*Bare) ? Bare : nullptr;
    };
    /* A reference the body writes, bound to what it cannot name (a map element, `C ? X : Y`), gets a copy stored back
       after the body. */
    std::vector<std::pair<const Json*, std::string>> CopyBacks;
    for (size_t I = 0; I < Parms.size(); ++I)
        if (const Json* Bare = PeelLvalue(Args[I]); IsMutableRef(TypeOf(*Parms[I])) && Bare && !Aliased(I) && !IsDerefLvalue(*Bare)
            && !OnlyRead(*Body, Parms[I]->value("id", std::string())))
            CopyBacks.emplace_back(Args[I], Name(*Parms[I]));
    if (!CopyBacks.empty()) return LowerCopyBack(CallNode, CopyBacks, Method, BP, Out, Err);
    /* Every argument is lowered before any parameter is bound: an argument can expand this same function again
       (`Twice(Twice(V))`), and that expansion binds the parameters for itself. For a reference, that is what fixes
       its place: the object and the index, into locals (Pins). */
    std::vector<FArgIR> Values(Parms.size());
    std::vector<Json> Places(Parms.size());
    std::vector<std::vector<FStmtIR>> Pins(Parms.size());
    for (size_t I = 0; I < Parms.size(); ++I)
        if (const Json* A = Aliased(I))
        {
            Json Pre = Json::array();
            Places[I] = StabilizeLvalue(*A, Pre, true);
            const Json Wrap = { {"kind", "CompoundStmt"}, {"inner", std::move(Pre)} };
            if (!LowerBody(Wrap, BP, Pins[I], Locals, Err)) return false;
        }
        else if (WcoDefault[I]) { if (!CurrentWco.empty()) { Values[I].K = FArgIR::Local; Values[I].S = CurrentWco; } }
        else if (!LowerArg(*Args[I], BP, Values[I], Err)) return false;
    /* The caller's own local can stand in for a parameter the body only reads, as a constant does, when nothing could
       change it before the body is done: no other argument that names it runs anything or may store to it (a frame
       local has no address, so one that does not name it cannot), and no mutable reference parameter is bound to
       anything that names it, the only way the body could reach a caller's local. A reference the caller holds (an
       outer inline's alias) may name anything, so one bound to a reference parameter keeps every local out. The
       caller's own reference parameter (LocalOut) never stands in: it may be an object's property the body writes. */
    bool bVarsInPlace = !bCurNoOpt;
    std::set<std::string> RefBound;
    std::function<void(const Json&)> NamedIn = [&](const Json& N) {
        if (Kind(N) == "DeclRefExpr" && N.contains("referencedDecl")) RefBound.insert(N["referencedDecl"].value("id", std::string()));
        ForEach(N, NamedIn);
    };
    for (size_t I = 0; I < Parms.size(); ++I)
        if (IsMutableRef(TypeOf(*Parms[I]))) NamedIn(*Args[I]);
    for (const std::string& Id : RefBound) bVarsInPlace = bVarsInPlace && !RefAlias.count(Id);
    std::vector<bool> InPlace(Parms.size());
    for (size_t I = 0; I < Parms.size(); ++I)
    {
        const Json* D = Strip(Args[I]);
        const std::string Id = D && Kind(*D) == "DeclRefExpr" ? (*D)["referencedDecl"].value("id", std::string()) : std::string();
        const FArgIR& X = Values[I];
        InPlace[I] = bVarsInPlace && !Id.empty() && !RefBound.count(Id) && !RefAlias.count(Id) && X.K == FArgIR::Local && !X.Base;
        for (size_t J = 0; J < Parms.size() && InPlace[I]; ++J)
            InPlace[I] = J == I || (Mentions(Pins[J], X.S) == 0 && !MayStore(Values[J], X.S)
                                    && (Mentions(Values[J], X.S) == 0 || !CallsImpure(Values[J])));
    }
    for (size_t I = 0; I < Parms.size(); ++I)
    {
        const std::string Id = Parms[I]->value("id", std::string());
        std::string Type = TypeOf(*Parms[I]);
        /* A previous expansion of the same function left its own binding for this parameter. */
        RefAlias.erase(Id);
        LocalRename.erase(Id);
        ParmConst.erase(Id);
        while (!Type.empty() && (Type.back() == '&' || Type.back() == ' ')) Type.pop_back();
        Type = StripTypeKeywords(Type);
        const std::string Local = Prefix + Name(*Parms[I]);
        if (Aliased(I))
        {
            for (FStmtIR& P : Pins[I]) B.Body->push_back(std::move(P));
            RefAlias[Id] = std::move(Places[I]);
            continue;
        }
        FStmtIR Bind;
        Bind.Value = std::move(Values[I]);
        /* A constant the body only reads is used in place: no local, no copy. */
        if (IsFoldableConst(Bind.Value) && OnlyRead(*Body, Id)) { ParmConst[Id] = Bind.Value; continue; }
        if (InPlace[I] && Canon(Bind.Value.InnerType) == Canon(Type) && (OnlyRead(*Body, Id) || OnlyReadValue(*Body, Id)))
        { ParmConst[Id] = Bind.Value; continue; }
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

    /* In place of a call to a static Blueprint function, a world context the body's calls leave out is the function's
       own parameter, as its own compile wires it (Generate): what the call passed for it. An inline function's is the
       caller's, as documented. */
    std::string Wco = CurrentWco;
    for (size_t I = 0; I < Parms.size() && bStaticCall; ++I)
    {
        if (!IsWcoName(Name(*Parms[I]))) continue;
        const std::string Id = Parms[I]->value("id", std::string());
        if (const auto L = LocalRename.find(Id); L != LocalRename.end()) Wco = L->second;
        else if (const auto C = ParmConst.find(Id); C != ParmConst.end() && (C->second.K == FArgIR::Local || C->second.K == FArgIR::Self))
            Wco = C->second.K == FArgIR::Local ? C->second.S : std::string();
        break;
    }
    const std::string SavedWco = CurrentWco;
    CurrentWco = Wco;

    InlineStack.push_back(&Def);
    InlineResults.emplace_back(Out.InlineResult, RetType);
    const int32 SavedLoops = LoopDepth, SavedSwitches = SwitchDepth;
    std::vector<FStmtIR> SavedWriteBacks;
    SavedWriteBacks.swap(WriteBacks);
    /* The body's break / continue are its own, so the caller's loops are not LoopDepth here; but the body still runs
       once per trip round them, and a declaration in it must start fresh each time (the __Fresh twin below). */
    const int32 SavedReEntered = ReEntered;
    ReEntered += (SavedLoops > 0 || bBodyHasGoto) ? 1 : 0;
    LoopDepth = SwitchDepth = 0;
    /* Each expansion lowers the body again, so its labels get ids of their own. */
    std::map<std::string, int32> SavedLabels;
    SavedLabels.swap(GotoLabels);
    const bool bSavedHasGoto = bBodyHasGoto;
    bBodyHasGoto = HasGoto(*Body);
    bFnHasGoto |= bBodyHasGoto;
    const bool bOk = LowerBody(*Body, BP, *B.Body, Locals, Err);
    CurrentWco = SavedWco;
    LoopDepth = SavedLoops;
    ReEntered = SavedReEntered;
    SwitchDepth = SavedSwitches;
    WriteBacks.swap(SavedWriteBacks);
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
             V to a local. `auto& [K, V]` writes V back with Map_Add after each iteration, `break` included, so the
             body changes values in place - unless the body only reads V (`V->X = 1` included: that writes the
             object, not the map); `auto [K, V]` is a copy, as in C++, and writes nothing back.
   `auto& [K, V]` and `const auto& [K, V]` over a TMap walk its slots in place instead (see below).
   ponytail: a TSet iterates a copy: its elements are const, so walking it in place would only save the copy. */

/* Whether Body adds to, removes from or reorders a TMap of MapType, or assigns a whole one: a walk over such a map's slots
   in place would lose its place. The map called on is not told apart from another of the same type; one written through
   a call the body makes is not seen, as C++'s own ensure sees it only at run time. */
bool FCompiler::ChangesMapCount(const Json& Body, const std::string& MapType) const
{
    static const std::set<std::string> Moving = { "Add", "Emplace", "FindOrAdd", "Remove", "RemoveAndCopyValue",
        "FindAndRemoveChecked", "Empty", "Reset", "Append", "Compact", "CompactStable", "Shrink", "KeySort", "ValueSort",
        "KeyStableSort", "ValueStableSort" };
    const std::string K = Kind(Body);
    auto Of = [&](const Json* N) { return N && TypeTag(TypeOf(*N)) == TypeTag(MapType); };
    if (K == "MemberExpr" && Moving.count(Body.value("name", std::string())) && Of(First(Body))) return true;
    if (K == "CXXOperatorCallExpr")
        if (const Json* Callee = First(Body) ? Strip(First(Body)) : nullptr; Callee && Kind(*Callee) == "DeclRefExpr"
            && Name((*Callee)["referencedDecl"]) == "operator=" && Of(Nth(Body, 1))) return true;
    bool bFound = false;
    ForEach(Body, [&](const Json& C) { bFound = bFound || ChangesMapCount(C, MapType); });
    return bFound;
}

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

    /* C++ binds the range once: whatever locates it (`Pick()->Items`, `Cur->Items` with `Cur = Next` in the
       body) is evaluated into a local up front. */
    Json RangePre = Json::array();
    const Json StableRange = StabilizeLvalue(*RangeExpr, RangePre, true);
    if (!RangePre.empty())
    {
        Json Wrap = { {"kind", "CompoundStmt"}, {"inner", std::move(RangePre)} };
        if (!LowerBody(Wrap, BP, Out, Locals, Err)) return false;
    }
    RangeExpr = &StableRange;

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
        St.Call.WrittenArgs = ContainerWrites(Fn);
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

    /* `for (T X : kPrimes)` over an inline array of constants makes no array: each pass sets X with an EX_SwitchValue on
       the index, which must be a variable (execSwitchValue compares the cases with it where it lies). Every element is a
       case of its own, as one that falls through to the default throws a script exception first, a logged warning. */
    // ponytail: pass I compares I + 1 cases, N^2/2 in all; past 16 elements the Make Array walk below costs less.
    const Json* Items = InlineListOf(RangeExpr);
    std::string LoopTy = TypeOf(*LoopDecl);
    while (!LoopTy.empty() && (LoopTy.back() == '&' || LoopTy.back() == ' ')) LoopTy.pop_back();
    if (LoopTy.size() > 6 && LoopTy.compare(LoopTy.size() - 6, 6, " const") == 0) LoopTy.erase(LoopTy.size() - 6);  // `auto`: `int const &`
    std::vector<FArgIR> Consts;
    const bool bConsts = Which == 'A' && Items && Kind(*LoopDecl) == "VarDecl" && Canon(LoopTy) == Canon(Args[0])
                      && InlineConsts(*Items, Args[0], BP, &Consts);
    if (bConsts && Consts.size() <= 16)
    {
        const std::string Idx = "__RangeIdx" + N + "__", Var = "__RangeVar" + N + "__";
        if (!AddLocal(Idx, "int32") || !AddLocal(Var, Args[0])) return false;
        FArgIR Pick, Zero, One, Count;
        Zero.K = One.K = Count.K = FArgIR::Int;
        One.I = 1;
        Count.I = int32(Consts.size());
        Pick.K = FArgIR::Call;
        Pick.InnerType = Args[0];
        Pick.Sub = std::make_shared<FCallIR>();
        Pick.Sub->Intrinsic = "__SwitchValue__";
        Pick.Sub->Args.push_back(LocalArg(Idx));
        for (size_t I = 0; I < Consts.size(); ++I)
        {
            FArgIR Case;
            Case.K = FArgIR::Int;
            Case.I = int32(I);
            Pick.Sub->Args.push_back(Case);
            Pick.Sub->Args.push_back(Consts[I]);
        }
        Pick.Sub->Args.push_back(Consts.back());        // the default, which no pass reaches
        Out.push_back(AssignStmt(Idx, "int32", Zero));
        FStmtIR Loop;
        Loop.K = FStmtIR::While;
        Loop.Cond = Math("Less_IntInt", LocalArg(Idx), Count);
        Loop.Body = std::make_shared<std::vector<FStmtIR>>(1, AssignStmt(Var, Args[0], std::move(Pick)));
        Loop.Inc = std::make_shared<std::vector<FStmtIR>>(1, AssignStmt(Idx, "int32", Math("Add_IntInt", LocalArg(Idx), One)));
        RefAlias[LoopDecl->value("id", std::string())] = RefToLocal(Var, Args[0]);
        const Json BodyWrap = Kind(*Body) == "CompoundStmt" ? *Body : Json{ {"kind", "CompoundStmt"}, {"inner", Json::array({ *Body })} };
        ++LoopDepth;
        const bool bBodyOk = LowerBody(BodyWrap, BP, *Loop.Body, Locals, Err);
        --LoopDepth;
        if (!bBodyOk) return false;
        Out.push_back(std::move(Loop));
        return true;
    }

    FArgIR Range;
    if (!LowerArg(*RangeExpr, BP, Range, Err)) return false;
    /* A container the range makes, an inline variable's braced list or an inline call's result: made once, before the
       loop, as C++ binds the range to one reference first, and the loop reads the local it was made in. */
    Json Made;
    if (Range.K == FArgIR::Call && Range.Sub && Range.Sub->Intrinsic == "__Inline__")
    {
        if (!HoistReadsInArg(Range, BP, Locals, Out, Err)) return false;
        Made = RefToLocal(Range.S, RangeTy);
        RangeExpr = &Made;
    }
    if (Range.K != FArgIR::Field && Range.K != FArgIR::Local && Range.K != FArgIR::LocalOut && Range.K != FArgIR::Member)
    { *Err = "range-for needs a container variable, not a computed value"; return false; }

    /* `auto& [K, V]` and `const auto& [K, V]` over a TMap walk the map's own slots (MapSlotView): V is the value where it
       lives, so a write through it is what `Map[K]` reads next, a T& binds it, and a container value changes in place.
       Each use re-reads the element array, so a map that grows under the walk is still read where it is. A map with free
       slots (it lost elements) is packed first, by copying it out and back, as FMapProperty copies densely. A body that
       adds to or removes from a map of this type would move the walk under it (C++ answers with an ensure): it walks a
       copy of the keys below, as `auto [K, V]` does. */
    std::vector<const Json*> Binds;
    if (Which == 'M') ForEach(*LoopDecl, [&](const Json& C) { if (Kind(C) == "BindingDecl") Binds.push_back(&C); });
    const std::string BoundAs = TypeOf(*LoopDecl);
    int32 KeySize = 0, KeyAlign = 1, ValueSize = 0, ValueAlign = 1;
    std::string NoLayout;
    if (Which == 'M' && Kind(*LoopDecl) == "DecompositionDecl" && Binds.size() == 2 && !BoundAs.empty() && BoundAs.back() == '&'
        && LayoutOf(Args[0], &KeySize, &KeyAlign, &NoLayout) && LayoutOf(Args[1], &ValueSize, &ValueAlign, &NoLayout)
        && std::max(KeyAlign, ValueAlign) <= 8 && !ChangesMapCount(*Body, RangeTy))
    {
        FIndex View, Slot;
        if (!MapSlotView(RangeTy, Args[0], Args[1], BP, &View, &Slot, Err)) return false;
        const std::string SlotTy = "FMapSlot_" + TypeTag(RangeTy);
        auto Member = [&](const char* Field, FIndex Owner, FArgIR Base, const std::string& Type) {
            FArgIR M;
            M.K = FArgIR::Member;
            M.S = Field;
            M.Owner = Owner;
            M.Base = std::make_shared<FArgIR>(std::move(Base));
            M.LetOp = LetOpFor(Type);
            M.InnerType = Type;
            return M;
        };
        auto Count = [&](const char* Lib, const char* Fn, FArgIR Of) {
            FArgIR C;
            C.K = FArgIR::Call;
            C.InnerType = "int32";
            C.Sub = std::make_shared<FCallIR>();
            C.Sub->Fn = BP.EngineFunction("/Script/Engine", Lib, Fn);
            C.Sub->WrittenArgs = ContainerWrites(Fn);
            C.Sub->Args = { std::move(Of) };
            return C;
        };
        const FArgIR Slots = Member("__Slots__", View, Range, "TArray<" + SlotTy + ">");
        const std::string Packed = "__RangePack" + N + "__", Idx = "__RangeIdx" + N + "__", Key = "__RangeKey" + N + "__";
        if (!AddLocal(Packed, RangeTy) || !AddLocal(Idx, "int32") || !AddLocal(Key, Args[0])) return false;

        /* if (slots != elements) { Packed = Map; Map.Empty(); Map = Packed; Packed.Empty(); } - the Empty between keeps
           the two copies from ever becoming `Map = Map`, which empties it. */
        FArgIR SlotCount = Count("KismetArrayLibrary", "Array_Length", Slots);
        FArgIR ElemCount = Count("BlueprintMapLibrary", "Map_Length", Range);
        FStmtIR Pack;
        Pack.K = FStmtIR::If;
        Pack.Cond = Math("NotEqual_IntInt", std::move(SlotCount), std::move(ElemCount));
        Pack.Then = std::make_shared<std::vector<FStmtIR>>();
        Pack.Then->push_back(AssignStmt(Packed, RangeTy, Range));
        Pack.Then->push_back(CallStmt("BlueprintMapLibrary", "Map_Clear", { Range }));
        FStmtIR Back;
        Back.K = FStmtIR::Assign;
        Back.Var = Range;
        Back.Var.LetOp = LetOpFor(RangeTy);
        Back.bAssignLocal = Range.K == FArgIR::Local;
        Back.bAssignOutParm = Range.K == FArgIR::LocalOut;
        Back.Value = LocalArg(Packed);
        Pack.Then->push_back(std::move(Back));
        Pack.Then->push_back(CallStmt("BlueprintMapLibrary", "Map_Clear", { LocalArg(Packed) }));
        Out.push_back(std::move(Pack));

        FArgIR Zero, One;
        Zero.K = One.K = FArgIR::Int;
        One.I = 1;
        Out.push_back(AssignStmt(Idx, "int32", Zero));
        FStmtIR Loop;
        Loop.K = FStmtIR::While;
        FArgIR Trips = Count("KismetArrayLibrary", "Array_Length", Slots);
        Loop.Cond = Math("Less_IntInt", LocalArg(Idx), std::move(Trips));
        Loop.Body = std::make_shared<std::vector<FStmtIR>>();
        Loop.Inc = std::make_shared<std::vector<FStmtIR>>();
        FArgIR At;
        At.K = FArgIR::Index;
        At.Base = std::make_shared<FArgIR>(Slots);
        At.Sub = std::make_shared<FCallIR>();
        At.Sub->Args = { LocalArg(Idx) };
        At.S = Slots.S;
        At.Owner = Slots.Owner;
        At.LetOp = LetOpFor(SlotTy);
        At.InnerType = SlotTy;
        Loop.Body->push_back(AssignStmt(Key, Args[0], Member("Key", Slot, At, Args[0])));
        RefAlias[Binds[0]->value("id", std::string())] = RefToLocal(Key, Args[0]);
        RefPlace[Binds[1]->value("id", std::string())] = Member("Value", Slot, At, Args[1]);
        const Json BodyWrap = Kind(*Body) == "CompoundStmt" ? *Body : Json{ {"kind", "CompoundStmt"}, {"inner", Json::array({ *Body })} };
        ++LoopDepth;
        const bool bBodyOk = LowerBody(BodyWrap, BP, *Loop.Body, Locals, Err);
        --LoopDepth;
        if (!bBodyOk) return false;
        Loop.Inc->push_back(AssignStmt(Idx, "int32", Math("Add_IntInt", LocalArg(Idx), One)));
        Out.push_back(std::move(Loop));
        return true;
    }

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
    Length.Sub->WrittenArgs = ContainerWrites("Array_Length");
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
        if (!AddLocal(Key, Args[0])) { --LoopDepth; return false; }
        FArgIR KeyAt;
        if (!LowerArg(Elem, BP, KeyAt, Err)) { --LoopDepth; return false; }
        Loop.Body->push_back(AssignStmt(Key, Args[0], std::move(KeyAt)));
        RefAlias[Bindings[0]->value("id", std::string())] = RefToLocal(Key, Args[0]);
        const std::string PairTy = TypeOf(*LoopDecl);
        const bool bByRef = !PairTy.empty() && PairTy.back() == '&' && StripTypeKeywords(PairTy) == PairTy;   // `auto& [K, V]`
        /* `auto& [K, V]` names the map's own value: V is `Map[K]`, each read a Map_Find and each store a Map_Add, so
           a write through either name is what the other reads next. A container value stays a copy written back at
           the end of each pass (below): every operation on it through the map would copy it whole. */
        if (bByRef && !IsContainerType(StripTypeKeywords(Args[1])))
            RefAlias[Bindings[1]->value("id", std::string())] =
                { {"kind", "CXXOperatorCallExpr"}, {"type", {{"qualType", Args[1]}}},
                  {"inner", Json::array({ Json{ {"kind", "DeclRefExpr"}, {"referencedDecl", {{"kind", "CXXMethodDecl"}, {"name", "operator[]"}}} },
                                          *RangeExpr, RefToLocal(Key, Args[0]) })} };
        else
        {
            if (!AddLocal(Val, Args[1])) { --LoopDepth; return false; }
            FStmtIR Find = CallStmt("BlueprintMapLibrary", "Map_Find", { Range, LocalArg(Key), LocalArg(Val) });
            if (IsContainerType(StripTypeKeywords(Args[1])))
            {
                /* A nested container value is a wrapper struct: Map_Find fills a wrapper temp, then Val is its Value. */
                FArgIR Call;
                Call.K = FArgIR::Call;
                Call.Sub = std::make_shared<FCallIR>(Find.Call);
                if (!NestedWrapperOut(Args[1], 2, "", AssignStmt(Val, Args[1], FArgIR()), BP, Call, Err)) { --LoopDepth; return false; }
                Find = Call.Sub->Inline->front();
            }
            Loop.Body->push_back(std::move(Find));
            RefAlias[Bindings[1]->value("id", std::string())] = RefToLocal(Val, Args[1]);
            if (bByRef && !OnlyRead(*Body, Bindings[1]->value("id", std::string())))
            {
                FStmtIR Back = CallStmt("BlueprintMapLibrary", "Map_Add", { Range, LocalArg(Key), LocalArg(Val) });
                Loop.Inc->push_back(Back);
                Loop.Trailer = std::make_shared<std::vector<FStmtIR>>(1, Back);
            }
        }
    }
    Json BodyWrap = Kind(*Body) == "CompoundStmt" ? *Body : Json{ {"kind", "CompoundStmt"}, {"inner", Json::array({ *Body })} };
    if (Loop.Trailer) WriteBacks.push_back(Loop.Trailer->front());
    const bool bBodyOk = LowerBody(BodyWrap, BP, *Loop.Body, Locals, Err);
    if (Loop.Trailer) WriteBacks.pop_back();
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
    /* `GetLoc().X`: EX_StructMemberContext steps its base with no result buffer and offsets into the storage the
       base leaves behind. A call leaves none, and a native one writes its return value through that null. */
    if (A.K == FArgIR::Member && A.Base)
        return HoistReadsInArg(*A.Base, BP, Locals, OutPre, Err) && HoistOperand(*A.Base, BP, Locals, OutPre, Err);
    if ((A.K == FArgIR::Field || A.K == FArgIR::InterfaceCtx) && A.Base)
        return HoistReadsInArg(*A.Base, BP, Locals, OutPre, Err);
    if (A.K == FArgIR::Index && A.Base && A.Sub && A.Sub->Args.size() == 1)
    {
        if (!HoistReadsInArg(*A.Base, BP, Locals, OutPre, Err)) return false;
        /* `GetCur()->Items[Swap()]`: E1 is sequenced before E2, so the object holding the array is pinned too. */
        return PinHolder(*A.Base, &A.Sub->Args[0], 1, BP, Locals, OutPre, Err)
            && HoistReadsInArg(A.Sub->Args[0], BP, Locals, OutPre, Err);
    }
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
    if (A.Sub->Target && !PinObject(*A.Sub->Target, A.Sub->Args.data(), A.Sub->Args.size(), BP, Locals, OutPre, Err))
        return false;

    /* Post-order: inner reads hoist before the outer. That way the outer's Addr can reference
       an already-materialised inner temp. */
    if (!HoistCallArgs(*A.Sub, BP, Locals, OutPre, Err)) return false;

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
                            std::vector<FStmtIR>& OutPre, std::string* Err, const FArgIR* Dest,
                            const FPropertyDef* DestProp)
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
    /* Dest: a local of the same layout the value is headed for anyway, which the branches then store into. */
    const bool bIntoDest = Dest && DestProp && PropKey(*DestProp) == PropKey(PD);
    if (!bIntoDest) Locals.push_back(PD);

    FArgIR TmpRef;
    TmpRef.K = FArgIR::Local;
    TmpRef.S = Tmp;
    TmpRef.LetOp = LetOpFor(Type);
    TmpRef.InnerType = Type;
    if (bIntoDest) TmpRef = *Dest;
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
        if (!bCurNoOpt && V.K == FArgIR::Call && V.Sub && IsBranch(V.Sub->Intrinsic))
        {
            /* A branch in a branch stores straight into this one's temp, not a temp of its own that is then copied. */
            if (!HoistBranch(V, BP, Locals, *Into, Err, &TmpRef, &PD)) return false;
            if (V.K == FArgIR::Local && V.S == TmpRef.S) return true;
        }
        else if (!HoistReadsInArg(V, BP, Locals, *Into, Err)) return false;
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
        /* `||` runs R when T is false: as the else of `if (T)`, which costs a jump where `if (!T)` costs a call. */
        const bool bElse = Which != "__AndAlso__" && !bCurNoOpt;
        If.Cond = Which == "__AndAlso__" || bElse ? TmpRef : NotOf(TmpRef, BP);
        if (!Arm(std::move(A.Sub->Args[1]), bElse ? If.Else : If.Then)) return false;
        if (bElse) If.Then = std::make_shared<std::vector<FStmtIR>>();
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

/* C++17 evaluates a call's object before its arguments, but what the arguments hoist (inline bodies, && / ?: arms,
   reads) runs ahead of the whole statement. So an object that could be different by then goes into a temp first. */
bool FCompiler::PinObject(FArgIR& Obj, const FArgIR* After, size_t NumAfter, FBlueprintClass& BP,
                          std::vector<FPropertyDef>& Locals, std::vector<FStmtIR>& OutPre, std::string* Err)
{
    if (std::none_of(After, After + NumAfter, [](const FArgIR& A) { return ContainsRead(A); })) return true;
    if (Obj.K == FArgIR::InterfaceCtx && Obj.Base) return PinObject(*Obj.Base, After, NumAfter, BP, Locals, OutPre, Err);
    if (Obj.K == FArgIR::Self || Obj.K == FArgIR::ObjConst || Obj.K == FArgIR::NullObj) return true;
    if (Obj.K == FArgIR::Local && std::none_of(After, After + NumAfter, [&](const FArgIR& A) { return Mentions(A, Obj.S) > 0; }))
        return true;
    return HoistOperand(Obj, BP, Locals, OutPre, Err, true);
}

/* A field's storage is found through the object holding it, so that object is what gets pinned. */
bool FCompiler::PinHolder(FArgIR& Place, const FArgIR* After, size_t NumAfter, FBlueprintClass& BP,
                          std::vector<FPropertyDef>& Locals, std::vector<FStmtIR>& OutPre, std::string* Err)
{
    FArgIR* P = &Place;
    while ((P->K == FArgIR::Member || P->K == FArgIR::Index) && P->Base) P = P->Base.get();
    return P->K != FArgIR::Field || !P->Base || PinObject(*P->Base, After, NumAfter, BP, Locals, OutPre, Err);
}

/* `GetCur()->Items.Add(Swap())`, `GetCur()->Map[Swap()]`, `GetCur()->OnHit.Broadcast(Swap())`: the container or
   dispatcher is argument 0, and as the call's object it is evaluated before the rest. */
bool FCompiler::HoistCallArgs(FCallIR& C, FBlueprintClass& BP, std::vector<FPropertyDef>& Locals,
                              std::vector<FStmtIR>& OutPre, std::string* Err)
{
    for (size_t I = 0; I < C.Args.size(); ++I)
    {
        if (!HoistReadsInArg(C.Args[I], BP, Locals, OutPre, Err)) return false;
        if (I == 0 && C.bOnArg0 && !PinHolder(C.Args[0], C.Args.data() + 1, C.Args.size() - 1, BP, Locals, OutPre, Err))
            return false;
        /* An rvalue bound to a reference parameter goes into a local first. A script callee (and a container thunk)
           steps it with a null result pointer, which EX_IntConst and the like would write through; a native's
           P_GET_PROPERTY_REF reads the address the argument left, and a call or a cast leaves the address of what ITS
           operands read (execStructMemberContext's rule, one level up). A constant leaves none, so a native reads it
           from its own buffer: it stays as it is there. */
        // ponytail: the local is made after the object is pinned, so an argument that changes the call's object runs
        // first; PinObject would need to see these hoists to fix that, if a mod ever does it.
        if (I < C.RefParms.size() && !C.RefParms[I].empty() && !IsStored(C.Args[I])
            && !(C.bRefsTakeConst && IsVmConstant(C.Args[I])))
        {
            if (C.Args[I].InnerType.empty()) C.Args[I].InnerType = C.RefParms[I];
            if (!HoistOperand(C.Args[I], BP, Locals, OutPre, Err)) return false;
        }
    }
    return true;
}

bool FCompiler::HoistReadsInStmt(FStmtIR& St, FBlueprintClass& BP,
                                 std::vector<FPropertyDef>& Locals,
                                 std::vector<FStmtIR>& OutPre, std::string* Err)
{
    /* A while condition runs every iteration, but hoisted statements land before the loop. So a condition that
       needs them becomes `while (true) { if (!Cond) break; ... }`, and they land inside. */
    if (St.K == FStmtIR::While && !St.bPostTest && ContainsRead(St.Cond))
    {
        FStmtIR Exit;
        Exit.K = FStmtIR::If;
        Exit.bJumpOut = !bCurNoOpt;
        Exit.Cond = bCurNoOpt ? NotOf(std::move(St.Cond), BP) : std::move(St.Cond);
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
    if (St.K == FStmtIR::While && St.bPostTest && ContainsRead(St.Cond))
    {
        /* do/while: the test follows Inc, which `continue` lands on too, so what it needs is computed there. */
        if (!St.Inc) St.Inc = std::make_shared<std::vector<FStmtIR>>();
        if (!HoistReadsInArg(St.Cond, BP, Locals, *St.Inc, Err)) return false;
    }

    if (!HoistReadsInArg(St.Var,   BP, Locals, OutPre, Err)) return false;
    if (!HoistReadsInArg(St.Value, BP, Locals, OutPre, Err)) return false;
    if (!HoistReadsInArg(St.Cond,  BP, Locals, OutPre, Err)) return false;
    if (St.Call.Target && !HoistReadsInArg(*St.Call.Target, BP, Locals, OutPre, Err)) return false;
    if (St.Call.Target && !PinObject(*St.Call.Target, St.Call.Args.data(), St.Call.Args.size(), BP, Locals, OutPre, Err))
        return false;
    if (!HoistCallArgs(St.Call, BP, Locals, OutPre, Err)) return false;
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
        /* `return C ? A : B;` is `if (C) return A; else return B;`: no temp to store the value in first. */
        if (!bCurNoOpt && St.K == FStmtIR::Return && St.bHasValue && St.Value.K == FArgIR::Call && St.Value.Sub
            && St.Value.Sub->Intrinsic == "__Select__" && St.Value.Sub->Args.size() == 3)
        {
            FStmtIR If;
            If.K = FStmtIR::If;
            If.Cond = St.Value.Sub->Args[0];
            FStmtIR Taken = St, Other = St;
            Taken.Value = St.Value.Sub->Args[1];
            Other.Value = St.Value.Sub->Args[2];
            If.Then = std::make_shared<std::vector<FStmtIR>>(1, std::move(Taken));
            If.Else = std::make_shared<std::vector<FStmtIR>>(1, std::move(Other));
            if (!HoistReadsInStmt(If, BP, Locals, Pre, Err)) return false;
            for (FStmtIR& P : Pre) Out.push_back(std::move(P));
            Out.push_back(std::move(If));
            continue;
        }
        /* `X = C ? A : B` (or && / ||) into a local: the branches store into X itself, when nothing in them reads X. */
        const auto Dest = std::find_if(Locals.begin(), Locals.end(), [&](const FPropertyDef& L) { return L.Name == St.Var.S; });
        if (!bCurNoOpt && (St.K == FStmtIR::Assign || (St.K == FStmtIR::Decl && St.bHasValue)) && St.Var.K == FArgIR::Local
            && !St.Var.Base && St.Value.K == FArgIR::Call && St.Value.Sub && IsBranch(St.Value.Sub->Intrinsic)
            && Dest != Locals.end() && Mentions(St.Value, St.Var.S) == 0)
        {
            const FPropertyDef DestProp = *Dest;
            if (!HoistBranch(St.Value, BP, Locals, Pre, Err, &St.Var, &DestProp)) return false;
            for (FStmtIR& P : Pre) Out.push_back(std::move(P));
            if (St.Value.K != FArgIR::Local || St.Value.S != St.Var.S) Out.push_back(std::move(St));
            continue;
        }
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

/* The number a constant expression comes to: literals, enum constants, ConstVars, the constants inlined parameters
   and read-only locals stand for (ParmConst), casts, and arithmetic, comparisons, `&&`, `||` and `?:` over them, each
   step rounded to the type clang gave it so `2 * 50.f` and `1 << 31` come out as C++ has them. `false && X`,
   `true || X` and `?:` need only the side C++ evaluates. False for anything else (a call, sizeof) and for a division
   by zero. */
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
        /* A float goes to bool by comparing with zero, not by truncating (0.5f is true). */
        if (T == "bool") { V.I = V.Num() != 0; V.bFloat = false; return true; }
        V.I = V.bFloat ? int64(V.F) : V.I;
        V.bFloat = false;
        if (T == "int" || T == "int32") V.I = int32(V.I);
        else if (T == "unsigned int" || T == "uint32") V.I = uint32(V.I);
        else if (T == "uint8" || T == "unsigned char") V.I = uint8(V.I);
        else if (T == "int8" || T == "signed char") V.I = int8(V.I);
        else if (T == "short" || T == "int16") V.I = int16(V.I);
        else if (T == "unsigned short" || T == "uint16") V.I = uint16(V.I);
        return true;
    };
    if (K == "IntegerLiteral") { Out = {}; Out.I = int64(std::strtoull(E.value("value", std::string("0")).c_str(), nullptr, 10)); return true; }
    if (K == "CharacterLiteral") { Out = {}; Out.I = CharValue(E); return true; }
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
        if (auto P = ParmConst.find(Id); P != ParmConst.end())
        {
            const FArgIR& A = P->second;
            Out = {};
            if (A.K == FArgIR::Int || A.K == FArgIR::Byte) Out.I = A.I;
            else if (A.K == FArgIR::Int64) Out.I = A.I64;
            else if (A.K == FArgIR::Bool) Out.I = A.B;
            else if (A.K == FArgIR::Float) { Out.bFloat = true; Out.F = A.F; }
            else return false;
            return Fit(Out);
        }
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
    if (K == "ConditionalOperator")
    {
        FConstVal C;
        const Json* Taken = Nth(E, 0) && FoldConst(*Nth(E, 0), C) ? Nth(E, C.Num() != 0 ? 1 : 2) : nullptr;
        return Taken && FoldConst(*Taken, Out) && Fit(Out);
    }
    if (K != "BinaryOperator") return false;
    const std::string Op = E.value("opcode", std::string());
    FConstVal L, R;
    if (Op == "&&" || Op == "||")
    {
        if (!Nth(E, 0) || !FoldConst(*Nth(E, 0), L)) return false;
        const bool bDecided = (L.Num() != 0) == (Op == "||");     // the right side never runs
        if (!bDecided && (!Nth(E, 1) || !FoldConst(*Nth(E, 1), R))) return false;
        Out = {};
        Out.I = bDecided ? Op == "||" : R.Num() != 0;
        return Fit(Out);
    }
    if (!Nth(E, 0) || !Nth(E, 1) || !FoldConst(*Nth(E, 0), L) || !FoldConst(*Nth(E, 1), R)) return false;
    Out = {};
    if (Op == "<" || Op == ">" || Op == "<=" || Op == ">=" || Op == "==" || Op == "!=")
    {
        /* Both sides already have their common type; a uint64 beyond INT64_MAX would compare as negative. */
        const std::string T = TypeOf(*Nth(E, 0));
        if (IsInt64Type(T) && (T.find("unsigned") != std::string::npos || T.find("uint64") != std::string::npos)) return false;
        const int32 Cmp = L.bFloat || R.bFloat ? (L.Num() < R.Num() ? -1 : L.Num() > R.Num() ? 1 : L.Num() == R.Num() ? 0 : 2)
                                               : (L.I < R.I ? -1 : L.I > R.I ? 1 : 0);
        Out.I = Op == "<" ? Cmp == -1 : Op == ">" ? Cmp == 1 : Op == "<=" ? Cmp == -1 || Cmp == 0
              : Op == ">=" ? Cmp == 1 || Cmp == 0 : Op == "==" ? Cmp == 0 : Cmp != 0;
        return Fit(Out);
    }
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

/* The outermost cast below N, among those Strip peels off it, that changes the value it passes on: to bool, float to
   int32 / int64, or an integer to a narrower one (int64 to int32, either to uint8). Only between Blueprint's own
   types, which ConvertArg can convert; a widening or same-size cast keeps every value, so the chain skips it. */
const Json* FCompiler::ValueCastBelow(const Json& N) const
{
    const Json* Leaf = Strip(&N);
    if (Leaf == &N) return nullptr;
    for (const Json* C = First(N); C && C != Leaf; C = First(*C))
    {
        const std::string K = Kind(*C), Cast = C->value("castKind", std::string());
        if (K != "ImplicitCastExpr" && K != "CStyleCastExpr" && K != "CXXStaticCastExpr" && K != "CXXFunctionalCastExpr") continue;
        const Json* From = First(*C);
        if (!From) continue;
        const EStrKind FK = StrKindOf(Canon(TypeOf(*From))), TK = StrKindOf(Canon(TypeOf(*C)));
        const bool bNumber = FK == SK_Int || FK == SK_Int64 || FK == SK_Float || FK == SK_Byte;
        if ((Cast == "FloatingToBoolean" || Cast == "IntegralToBoolean") && TK == SK_Bool && bNumber) return C;
        if (Cast == "FloatingToIntegral" && FK == SK_Float && (TK == SK_Int || TK == SK_Int64)) return C;
        if (Cast == "IntegralCast" && ((TK == SK_Byte && (FK == SK_Int || FK == SK_Int64)) || (TK == SK_Int && FK == SK_Int64)))
            return C;
    }
    return nullptr;
}

/* `!C` with no Not_PreBool call, where there is such a thing: `!!X` is X, and a comparison turns round (`<` and `>=`,
   `==` and `!=`); `<` and the like only over integers, since a NaN is neither `A < B` nor `A >= B`. */
bool FCompiler::FreeNegation(const Json& C, Json& Out) const
{
    const Json* N = Strip(&C);
    if (!N) return false;
    const std::string K = Kind(*N), Op = N->value("opcode", std::string());
    if (K == "UnaryOperator" && Op == "!" && First(*N)) { Out = *First(*N); return true; }
    static const std::map<std::string, std::string> Inverse = {
        { "<", ">=" }, { ">=", "<" }, { ">", "<=" }, { "<=", ">" }, { "==", "!=" }, { "!=", "==" } };
    const auto It = Inverse.find(Op);
    if (K != "BinaryOperator" || It == Inverse.end() || !Nth(*N, 0) || !Nth(*N, 1)) return false;
    auto Ordered = [&](const std::string& T) {
        return Canon(T) != "float" && StripTypeKeywords(T) != "double" && !IsObjectType(T);
    };
    if (Op != "==" && Op != "!=" && (!Ordered(TypeOf(*Nth(*N, 0))) || !Ordered(TypeOf(*Nth(*N, 1))))) return false;
    Out = *N;
    Out["opcode"] = It->second;
    return true;
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

/*
A namespace-scope variable. A Blueprint has no globals, so each one a function uses lives in the default object of a
class generated for it: <Ns>__<Name> in the mod's package, with one member of the variable's name and type whose
default is the initializer. Every class of every source in the mod reads and writes that one object, and two sources
using the same variable write the same package. UE_ASSET_ALL's All is one too: its default lists every UE_ASSET_AT
declared under its namespace whose class is All's element class or derives from it.
*/
bool FCompiler::LowerGlobal(const Json& Decl, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    const auto Scope = VarScope.find(Decl.value("id", std::string()));
    const std::string Ns = Scope == VarScope.end() ? std::string() : Scope->second;
    const Json& T = Decl.contains("type") ? Decl["type"] : Json::object();     // desugared: UeAssets spell `::USoundWave`
    const std::string Qual = Ns + Name(Decl), Type = StripTypeKeywords(T.value("desugaredQualType", T.value("qualType", std::string())));
    if (const FRecord *R = Find(Type), *UObj = Find("UObject"); R && UObj && (R == UObj || IsSubclassOf(*R, *UObj)))
    { *Err = Qual + " is an asset, not a value: point at it with &" + Name(Decl); return false; }
    if (!Globals.count(Qual))
    {
        FRecord G;
        for (size_t At = 0; At < Qual.size(); ++At)
            if (Qual.compare(At, 2, "::") == 0) { G.CppName += "__"; ++At; }
            else G.CppName += Qual[At];
        G.Base = "UObject";
        if (AssetAlls.count(Qual))
        {
            std::string Elem;
            const FRecord* Of = TemplateArg(Type, "TArray", &Elem) && TemplateArg(StripTypeKeywords(Elem), "TSoftObjectPtr", &Elem)
                              ? Find(StripTypeKeywords(Elem)) : nullptr;
            if (!Of) { *Err = Qual + ": UE_ASSET_ALL's All is a TArray<TSoftObjectPtr<a class>>"; return false; }
            Json List = { { "kind", "InitListExpr" }, { "inner", Json::array() } };
            for (const auto& [Key, Path] : AssetPaths)
            {
                if (Key.compare(0, Ns.size(), Ns) != 0 || !NsVarNamed.count(Key)) continue;
                const Json& KT = (*NsVarNamed[Key]).contains("type") ? (*NsVarNamed[Key])["type"] : Json::object();
                const FRecord* R = Find(StripTypeKeywords(KT.value("desugaredQualType", KT.value("qualType", std::string()))));
                if (R && (R == Of || IsSubclassOf(*R, *Of)))
                    List["inner"].push_back(Json{ { "kind", "StringLiteral" }, { "value", "\"" + Path + "\"" } });
            }
            Json& F = GlobalAst[Qual] = Json{ { "kind", "FieldDecl" }, { "name", Name(Decl) }, { "type", Json{ { "qualType", Type } } } };
            if (!List["inner"].empty()) F["inner"] = Json::array({ List });
            G.Fields.push_back(&F);
        }
        else if (!First(Decl) && Decl.value("storageClass", std::string()) == "extern")
        { *Err = Qual + " is declared extern but not defined in this source; a mod's global is defined where it is used"; return false; }
        else G.Fields.push_back(&Decl);
        Globals[Qual] = G;
    }
    const FRecord& G = Globals[Qual];
    const std::string Package = ModPackage + "/" + G.CppName;
    Out = FArgIR();
    Out.K = FArgIR::Field;
    Out.S = Name(Decl);
    Out.Owner = BP.PropertyOwner(Package, G.CppName + "_C");
    Out.LetOp = LetOpFor(TypeOf(Decl));
    Out.Base = std::make_shared<FArgIR>();
    Out.Base->K = FArgIR::ObjConst;
    Out.Base->Owner = BP.Asset(Package, G.CppName + "_C", Package, "Default__" + G.CppName + "_C");
    Out.Base->InnerType = "UObject *";
    return true;
}

bool FCompiler::AssetRef(const Json& N, FBlueprintClass& BP, FIndex* Out)
{
    if (Kind(N) != "UnaryOperator" || N.value("opcode", std::string()) != "&") return false;
    const Json* Ref = Strip(First(N));
    if (!Ref || Kind(*Ref) != "DeclRefExpr" || !Ref->contains("referencedDecl")) return false;
    const Json& D = (*Ref)["referencedDecl"];
    /* The desugared type: UeAssets headers spell theirs `::USoundWave`, since the namespace shadows the class. */
    const Json& T = D.contains("type") ? D["type"] : Json::object();
    const FRecord* R = Find(StripTypeKeywords(T.value("desugaredQualType", T.value("qualType", std::string()))));
    if (Kind(D) != "VarDecl" || !R || R->bIsStruct) return false;

    std::string Package;
    const std::string Var = Name(D);
    const auto Scope = VarScope.find(D.value("id", std::string()));
    const std::string Qual = (Scope == VarScope.end() ? std::string() : Scope->second) + Var;
    if (auto At = AssetPaths.find(Qual); At != AssetPaths.end())
        Package = At->second;
    else if (std::any_of(AssetDecls.begin(), AssetDecls.end(), [&](const Json* A) { return Name(*A) == Var; }))
        Package = PathIn(ModPackage, Qual);
    else return false;

    const std::string Object = SplitAssetPath(Package);
    *Out = BP.Asset(PackageOf(*R), ClassOf(*R), Package, Object);
    return true;
}

bool FCompiler::LowerDefault(const Json& F, FPropertyDef& PD, FBlueprintClass& BP, std::string* Err, const Json* Init,
                             bool bKeepZero)
{
    const Json* const Whole = Init ? Init : First(F);
    Init = Strip(Whole);
    if (!Init) return true;
    std::string K = Kind(*Init);
    if (PD.Type == "StructProperty" && NativeUnwritten(PD.StructName))
    {
        *Err = Name(F) + ": the engine reads a " + PD.StructName + " value in its own binary form, which AssetGen does not "
             "write yet; leave the value out";
        return false;
    }

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
    /* A soft pointer's default is its path, "/Game/Dir/Pkg" meaning Pkg.Pkg as UE_ASSET_AT's does. */
    if (std::string Path; (PD.Type == "SoftObjectProperty" || PD.Type == "SoftClassProperty") && FindLiteral(*Init, Path))
    {
        if (!Path.empty() && Path.find('.', Path.rfind('/') + 1) == std::string::npos) Path += "." + Path.substr(Path.rfind('/') + 1);
        PD.Default.K = !bKeepZero && Path.empty() ? FDefaultValue::None : FDefaultValue::Str;
        PD.Default.S = Path;
        return true;
    }
    const bool bNeg = K == "UnaryOperator" && Init->value("opcode", std::string()) == "-";
    if (bNeg)
    {
        Init = Strip(First(*Init));
        K = Init ? Kind(*Init) : std::string();
    }
    /* A member `{ .Q = 9 }` leaves unwritten (with no default of its own) is ImplicitValueInitExpr: zero. */
    if (!bNeg && (K == "CXXNullPtrLiteralExpr" || K == "ImplicitValueInitExpr" || (K == "CXXConstructExpr" && !First(*Init))))
        return true;

    /* A struct value: `FFloatInterval(1, 5)` or `{1, 5}`, one argument per member in declaration
       order. The members go on the property, each with its own default, and the writer turns them
       into nested tags - or into raw bytes when the struct has a native Serialize. */
    if (!bNeg && PD.Type == "StructProperty"
        && (K == "CXXConstructExpr" || K == "CXXTemporaryObjectExpr" || K == "InitListExpr"))
    {
        const FRecord* SR = Find(StripTypeKeywords(TypeOf(*Init)));
        if (!SR) { *Err = "unknown struct type in an initializer: " + TypeOf(*Init); return false; }
        std::vector<std::string> Names;
        for (const Json* SF : SR->Fields) Names.push_back(Name(*SF));
        const std::vector<const Json*> Args = StructArgs(*Init, SR, Names);
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
            /* Every member is written, so a zero is a value here and not "leave it out". One the braces leave out
               but that has a default of its own (CXXDefaultInitExpr) takes that default, as in C++. */
            const Json* A = Kind(*Args[I]) == "CXXDefaultInitExpr" ? nullptr : Args[I];
            if (!LowerDefault(*SR->Fields[I], MD, BP, Err, A, /*bKeepZero=*/true)) return false;
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
    if (!R.IsNative() && &R == Cur) return BP.ClassIndex();
    return BP.EngineClass(PackageOf(R), ClassOf(R));
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

/* A call result the VM's 64-byte statement buffer cannot take: one with a destructor (a string, a container, a struct
   holding one) or bigger than the buffer. */
bool FCompiler::NeedsResultLocal(const std::string& Type, int32 Depth)
{
    const std::string T = StripTypeKeywords(Type);
    if (T.empty() || T == "void" || Depth > 8) return false;
    if (T == "FString" || T == "FText" || T.compare(0, 7, "TArray<") == 0 || T.compare(0, 5, "TMap<") == 0
        || T.compare(0, 5, "TSet<") == 0) return true;
    int32 Size = 0, Align = 1;
    std::string NoLayout;
    if (LayoutOf(T, &Size, &Align, &NoLayout) && Size > 64) return true;
    if (auto St = Structs.find(T); St != Structs.end())
        for (const auto& Field : St->second.Fields) if (NeedsResultLocal(Field.first, Depth + 1)) return true;
    if (const FRecord* R = Find(T); R && R->bIsStruct)
        for (const Json* F : R->Fields) if (NeedsResultLocal(TypeOf(*F), Depth + 1)) return true;
    return false;
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
        /* Every Add / Find and a loaded default hash the element or key through GetValueTypeHash, which check()s
           CPF_HasGetValueTypeHash (Property.cpp 1517-1522): never on a bool, an FText or a delegate, and on a native
           struct only when its CppStructOps has GetTypeHash. The list is HASHABLE_STRUCTS in invariant_rules/properties.py;
           a UserDefinedStruct always hashes, and a struct of the game's own modules is not judged. */
        if (Tpl[1] != 'A')
        {
            static const std::set<std::string> Hashable = {
                "AdaptorTriangleID", "AssetData", "CachedRigElement", "Color", "CurveTableRowHandle", "DateTime",
                "EdGraphPinReference", "EdgeID", "FontData", "FontOutlineSettings", "GameplayTag", "Guid", "HLODInstancingKey",
                "InputChord", "IntPoint", "IntVector", "Key", "LinearColor", "MovieSceneEvaluationKey",
                "MovieSceneEvaluationOperand", "MovieSceneObjectBindingID", "MovieSceneTrackInstanceInput",
                "NavAgentProperties", "NiagaraAssetVersion", "NiagaraDataSetID", "NiagaraEmitterNameSettingsRef",
                "NiagaraFunctionSignature", "NiagaraID", "NiagaraTypeDefinition", "NiagaraVMExecutableDataId",
                "NiagaraVariableBase", "PolygonGroupID", "PolygonID", "PrimaryAssetId", "PrimaryAssetType", "RigElementKey",
                "RigElementKeyCollection", "SlateFontInfo", "SolverTrailingData", "TimerHandle", "Timespan", "TriangleID",
                "Vector", "Vector2D", "Vector4", "VertexID", "VertexInstanceID", "SoftObjectPath", "SoftClassPath",
                "Vector_NetQuantize", "Vector_NetQuantize10", "Vector_NetQuantize100", "Vector_NetQuantizeNormal" };
            const FPropertyDef& K = Parts[0];
            const FRecord* S = K.Type == "StructProperty" ? Find(StripTypeKeywords(Args[0])) : nullptr;
            static const std::set<std::string> GameModules = { "/Script/FSD", "/Script/FSDEngine", "/Script/FSDRawInput",
                                                              "/Script/FSDAnsel", "/Script/DiscordSDK" };
            const bool bNativeStruct = S && S->IsNative() && S->UePackage.compare(0, 8, "/Script/") == 0
                                    && !GameModules.count(S->UePackage);
            if (K.Type == "BoolProperty" || K.Type == "TextProperty" || K.Type.find("DelegateProperty") != std::string::npos
                || (bNativeStruct && !Hashable.count(S->UeName)))
            { *Err = Where + ": a " + (Tpl[1] == 'S' ? "TSet element" : "TMap key") + " of type " + StripTypeKeywords(Args[0])
                     + " cannot hash, and the engine hashes each one; use a type that does, or a UE_STRUCT holding it"; return false; }
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
        *Out = StructParam(PName, BP.ScriptStruct(PackageOf(*SR), ClassOf(*SR)), ClassOf(*SR), Size, ExtraFlags);
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
    *Out = ObjectParam(PName, BP.EngineClass(PackageOf(*PR), ClassOf(*PR)), ExtraFlags);
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
    const std::string PackageName = PackageOf(R);
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
    const std::string ParentPkg = !Parent ? "/Script/CoreUObject" : PackageOf(*Parent);
    FBlueprintClass BP(P, ClassOf(R), ParentPkg, !Parent ? "Interface" : ClassOf(*Parent), ParentPkg.compare(0, 6, "/Game/") == 0);
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
    if (!SavePackage(P, OutDir, PackageName, Err)) return false;
    RegistryRows.push_back({ PackageName, LeafOf(R.CppName), "BlueprintGeneratedClass" });
    printf("  %-14s -> %s.uasset  (interface, %d functions)\n", R.CppName.c_str(), Shown(PackageName).c_str(),
           int32(R.Methods.size()));
    return true;
}

bool FCompiler::GenerateStruct(const FRecord& R, const std::string& OutDir, std::string* Err)
{
    Cur = &R;
    const std::string PackageName = PackageOf(R);
    FPackage P(PackageName);
    StampIdentity(P, PackageName);
    FBlueprintClass BP(P, ClassOf(R), "", "", false);

    for (const Json* F : R.Fields)
    {
        FPropertyDef PD;
        const std::string Field = IsInternalViewStruct(R.CppName) ? Name(*F)
                                                                  : ModFieldName(PackageName, Name(*F));
        if (!TypeToProperty(TypeOf(*F), Field, 0, "member " + Name(*F), BP, &PD, Err)) return false;
        /* The struct's default instance carries every member's value, so a member no value can be written for is none. */
        if (PD.Type == "StructProperty" && NativeUnwritten(PD.StructName))
        {
            *Err = R.CppName + "::" + Name(*F) + ": the engine reads a " + PD.StructName + " value in its own binary form, "
                   "which AssetGen does not write yet, and a UE_STRUCT's defaults hold every member's";
            return false;
        }
        if (!LowerDefault(*F, PD, BP, Err)) return false;
        PD.PropertyFlags = CPF_Edit | CPF_BlueprintVisible;
        BP.AddVariable(PD);
    }

    const uint32 H = StrCrc32(PackageName);
    const uint32 Guid[4] = { ~H, H * 2654435761u, H ^ 0x9E3779B9u, H };
    /* The editor stub reads the members before FinishStruct consumes them; both share the same Guid,
       so the cooked and uncooked assets carry identical member names and struct identity. */
    if (ApiDir && !IsInternalViewStruct(R.CppName) && !BP.WriteApiStruct(*ApiDir, Guid, Err))
        return false;
    BP.FinishStruct(Guid);
    if (!SavePackage(P, OutDir, PackageName, Err)) return false;
    RegistryRows.push_back({ PackageName, ClassOf(R), "UserDefinedStruct" });
    printf("  %-14s -> %s.uasset  (struct, %d members)\n", R.CppName.c_str(), Shown(PackageName).c_str(),
           int32(R.Fields.size()));
    return true;
}

/* Each wrapper a container needed, into NestedPackage beside the mod's own folder. Two mods that need the same one
   cook identical packages at the same path. A wrapper's own member may need a deeper one. */
bool FCompiler::GenerateEnum(const std::string& Enum, const std::string& OutDir, std::string* Err)
{
    const std::string PackageName = PathIn(ModPackage, Enum), Name = LeafOf(Enum);
    FPackage P(PackageName);
    StampIdentity(P, PackageName);
    FBlueprintClass BP(P, Name, "", "", false);
    if (ApiDir && !BP.WriteApiEnum(*ApiDir, ModEnums[Enum], Err)) return false;
    BP.FinishEnum(ModEnums[Enum]);
    if (!SavePackage(P, OutDir, PackageName, Err)) return false;
    RegistryRows.push_back({ PackageName, Name, "UserDefinedEnum" });
    printf("  %-14s -> %s.uasset  (enum, %d enumerators)\n", Enum.c_str(), Shown(PackageName).c_str(), int32(ModEnums[Enum].size()));
    return true;
}

/* The members a braced initializer of a Rec names, typed and lowered, in order. Measured on ED_Spider_Grunt: the
   semantic form lists the bases first, then every field in order, so a designator is found by position. A member
   the braces leave out is skipped. */
bool FCompiler::BracedMembers(const Json& List, const FRecord& Rec, const std::string& Where, FBlueprintClass& BP,
                              bool bKeepZero, std::vector<FPropertyDef>& Out, std::string* Err)
{
    size_t I = 0;
    if (!Rec.Base.empty())
    {
        const FRecord* B = Find(Rec.Base);
        const Json* Sub = Nth(List, I++);
        if (B && Sub && Kind(*Sub) == "InitListExpr" && !BracedMembers(*Sub, *B, Where, BP, bKeepZero, Out, Err)) return false;
    }
    I += Rec.Interfaces.size();
    for (const Json* F : Rec.Fields)
    {
        const Json* Init = Strip(Nth(List, I++));
        if (!Init || IsUnsetInit(*Init)) continue;
        FPropertyDef PD;
        if (!TypeToProperty(TypeOf(*F), UeNameOf(&Rec, Name(*F)), 0, Where + "." + Name(*F), BP, &PD, Err)) return false;
        if (!LowerDefault(*F, PD, BP, Err, Init, bKeepZero)) return false;
        Out.push_back(PD);
    }
    return true;
}

/* Only a field the braces name is written: the rest stay the CDO's. */
bool FCompiler::GenerateAsset(const Json& Var, const std::string& OutDir, std::string* Err)
{
    const FRecord* R = Find(StripTypeKeywords(TypeOf(Var)));
    if (!R || R->bIsStruct || (R->UeName.empty() && !R->IsGenerated())) return true;    // a plain C++ aggregate
    Cur = nullptr;

    const auto Scope = VarScope.find(Var.value("id", std::string()));
    const std::string AssetName = Name(Var);
    const std::string PackageName = PathIn(ModPackage, (Scope == VarScope.end() ? std::string() : Scope->second) + AssetName);
    FPackage P(PackageName);
    StampIdentity(P, PackageName);
    FBlueprintClass BP(P, AssetName, "", "", false);

    std::vector<FPropertyDef> Set;
    if (!BracedMembers(*BracedInit(Var), *R, AssetName, BP, false, Set, Err)) return false;
    for (const FPropertyDef& PD : Set) BP.AddVariable(PD);

    const std::string ClassPkg = PackageOf(*R), ClassName = ClassOf(*R);
    /* The CDO's import first, as MSVC evaluates call arguments (right to left); clang goes left to right. */
    const FIndex Cdo = BP.ClassDefaultObject(ClassPkg, ClassName);
    BP.FinishAsset(BP.EngineClass(ClassPkg, ClassName), Cdo);
    if (!SavePackage(P, OutDir, PackageName, Err)) return false;
    RegistryRows.push_back({ PackageName, AssetName, ClassName });
    printf("  %-14s -> %s.uasset  (asset, a %s)\n", AssetName.c_str(), Shown(PackageName).c_str(), R->CppName.c_str());
    return true;
}

/*
S38 - UE_ASSET_EDIT(Asset) { .Member = value, ... }: the members the braces name, written into Asset's own package as
the game cooked it. The target is a UE_ASSET_AT; a mod's own asset takes its values where it is declared.
*/
bool FCompiler::GenerateEdit(const std::string& Key, const Json& Var, std::string* Err)
{
    const auto Of = EditTargets.find(Key);
    std::function<const Json*(const Json&)> RefIn = [&](const Json& N) -> const Json* {
        if (Kind(N) == "DeclRefExpr" && N.contains("referencedDecl")) return &N;
        const Json* Found = nullptr;
        ForEach(N, [&](const Json& C) { if (!Found) Found = RefIn(C); });
        return Found;
    };
    const Json* Ref = Of == EditTargets.end() ? nullptr : RefIn(*Of->second);
    if (!Ref || !BracedInit(Var)) { *Err = "UE_ASSET_EDIT: write it as `UE_ASSET_EDIT(Asset) { .Member = value };`"; return false; }
    const Json& D = (*Ref)["referencedDecl"];
    const auto Scope = VarScope.find(D.value("id", std::string()));
    const std::string Qual = (Scope == VarScope.end() ? std::string() : Scope->second) + Name(D);
    const std::string Where = "UE_ASSET_EDIT(" + Qual + ")";
    const auto At = AssetPaths.find(Qual);
    if (At == AssetPaths.end())
    { *Err = Where + ": the target is a UE_ASSET_AT (every asset in UeAssets/ is one); a mod's own asset takes its values where it is declared"; return false; }
    const Json& T = D.contains("type") ? D["type"] : Json::object();
    const FRecord* R = Find(StripTypeKeywords(T.value("desugaredQualType", T.value("qualType", std::string()))));
    if (!R || R->bIsStruct) { *Err = Where + ": cannot tell the asset's class"; return false; }
    Cur = nullptr;

    std::string Package = At->second;
    const std::string Object = SplitAssetPath(Package);
    FPackage Scratch(Package);
    FBlueprintClass BP(Scratch, Object, "", "", false);
    std::vector<FPropertyDef> Defs;
    /* An explicit zero is written: the asset's own value is what it replaces, not the class default. */
    if (!BracedMembers(*BracedInit(Var), *R, Qual, BP, /*bKeepZero=*/true, Defs, Err)) return false;
    if (Defs.empty()) { *Err = Where + ": the braces name no member"; return false; }
    std::vector<FEditDef> Edits;
    for (FPropertyDef& D : Defs) Edits.push_back({ { std::move(D) }, {} });
    return ApplyEdit(Package, Object, std::move(Edits), Scratch, Where, Err);
}

/*
S38 - UE_PATCH: a class that edits the game Blueprint it derives from, in that Blueprint's own package. Its UE_DEFAULTS
assignments become tags of the Blueprint's default object; a member of any class above it can be named. `Comp->Field`
edits a component, in the export the engine builds that component from:
  - a native class's component: the default subobject under the default object, named as UeApi's `__UeSubobject` says
    (not always the member's name: ASpiderEnemy's `temperature` is the subobject Temperature);
  - a Blueprint's own SCS component: its template, `<Var>_GEN_VARIABLE` in the class;
  - a parent Blueprint's SCS component this one overrides: the override record's template, also `<Var>_GEN_VARIABLE`
    in the class. One it does not override yet has no such export, and adding the record is not built.
None of them is instanced from cooked data (bCookBlueprintComponentTemplateData is off in DRG, and false by default),
so the tags are all the engine reads. A method replaces the Blueprint's function of the same name (TransplantFunctions).
A member or an interface of the patch's own is refused rather than silently dropped.
*/
bool FCompiler::GeneratePatch(const FRecord& R, std::string* Err)
{
    const std::string Where = R.CppName + " (UE_PATCH)";
    const FRecord* B = R.Base.empty() ? nullptr : Find(R.Base);
    if (!B || !B->IsNative() || PackageOf(*B).compare(0, 6, "/Game/") != 0)
    {
        *Err = Where + ": a patch derives from the game Blueprint it edits (a class in a /Game package); a native class's "
               "defaults come from its C++ constructor, not from a package";
        return false;
    }
    /* Every one the patch declares: Collect leaves out the implicit ones. */
    const bool bMethods = !R.Methods.empty();
    if (!R.Fields.empty() || !R.Interfaces.empty())
    { *Err = Where + ": a patch edits its parent's defaults and replaces its functions; a member or an interface of its own is not built yet"; return false; }
    const Json* Body = nullptr;
    if (R.Defaults) ForEach(*R.Defaults, [&](const Json& C) { if (Kind(C) == "CompoundStmt") Body = &C; });
    if (!Body && !bMethods)
    {
        *Err = Where + ": nothing to edit - a patch's defaults go in UE_DEFAULTS { Field = value; }, and its methods replace "
               "the Blueprint's functions of the same name";
        return false;
    }
    /* The defaults first: the functions' names are written against the table the tags leave. */
    if (Body && !PatchDefaults(*B, *Body, Where, Err)) return false;
    return !bMethods || Generate(R, std::string(), Err);
}

/* The patch's UE_DEFAULTS, as tags of the Blueprint Bp's own package. */
bool FCompiler::PatchDefaults(const FRecord& Bp, const Json& Body, const std::string& Where, std::string* Err)
{
    const FRecord* B = &Bp;
    Cur = nullptr;

    const std::string Package = PackageOf(*B), Class = ClassOf(*B), Cdo = "Default__" + Class;
    const FRecord* Engine = B;      // the nearest native ancestor: its UeApi names each default subobject
    while (Engine && PackageOf(*Engine).compare(0, 8, "/Script/") != 0) Engine = Engine->Base.empty() ? nullptr : Find(Engine->Base);
    FPackage Scratch(Package);
    FBlueprintClass BP(Scratch, Class, "", "", false);
    std::map<std::string, std::vector<FEditDef>> Objects;          // an export, as ApplyEdit names it -> its assignments
    auto DeclarerOf = [&](const Json& M) {
        const auto Owner = FieldOwner.find(M.value("referencedMemberDecl", std::string()));
        return Owner == FieldOwner.end() ? nullptr : Find(Owner->second);
    };
    bool bOk = true;
    ForEach(Body, [&](const Json& S) {
        if (!bOk) return;
        const Json *Lhs = nullptr, *Rhs = nullptr, *Through = nullptr;
        std::vector<const Json*> Steps;
        if (!DefaultPath(S, Lhs, Rhs, Through, Steps))
        {
            *Err = Where + ": every statement is `Field = value;` or `Component->Field = value;`, or assigns part of one: "
                   "`Field.Member = value;`, `Field[2] = value;`";
            bOk = false;
            return;
        }
        std::string Object = Cdo;
        if (Through)
        {
            const std::string Comp = Name(*Through);
            const FRecord* CR = DeclarerOf(*Through);
            if (!CR) { *Err = Where + ": cannot tell which class declares " + Comp; bOk = false; return; }
            if (PackageOf(*CR).compare(0, 6, "/Game/") == 0)
            {
                if (!CR->ScsNodes.count(Comp))
                { *Err = Where + ": " + Comp + " is not one of " + CR->UeName + "'s components (UeApi gives it no SCS node)"; bOk = false; return; }
                Object = Class + ":" + UeNameOf(CR, Comp) + "_GEN_VARIABLE";
            }
            else
            {
                const auto Sub = Engine ? Engine->Subobjects.find(Comp) : std::map<std::string, std::string>::const_iterator();
                if (!Engine || Sub == Engine->Subobjects.end())
                {
                    *Err = Where + ": UeApi does not say which default subobject " + Comp + " is - regenerate it with genueapi, "
                           "which reads that off the object dump";
                    bOk = false;
                    return;
                }
                Object = Cdo + ":" + Sub->second.substr(0, Sub->second.find(' '));
            }
        }
        const FRecord* DR = DeclarerOf(*Lhs);
        if (!DR) { *Err = Where + ": cannot tell which class declares " + Name(*Lhs); bOk = false; return; }
        FEditDef Edit;
        bOk = EditAssignment(*Lhs, *DR, Steps, Rhs, Where, BP, Edit, Err);
        if (bOk) Objects[Object].push_back(std::move(Edit));
    });
    if (!bOk) return false;
    if (Objects.empty()) { *Err = Where + ": UE_DEFAULTS assigns nothing"; return false; }
    for (const auto& [Object, Edits] : Objects)
        if (!ApplyEdit(Package, Object, Edits, Scratch, Where, Err)) return false;
    return true;
}

/* S38: one assignment of an edit, `Root<Steps> = Rhs`: the member Root names (Declarer's), each step's def down the
   path, and the value lowered against the last. Zero is a real value here: it replaces the game's own, which need not
   be zero. */
bool FCompiler::EditAssignment(const Json& Root, const FRecord& Declarer, const std::vector<const Json*>& Steps,
                               const Json* Rhs, const std::string& Where, FBlueprintClass& BP, FEditDef& Out, std::string* Err)
{
    Out.Chain.emplace_back();
    if (!TypeToProperty(TypeOf(Root), UeNameOf(&Declarer, Name(Root)), 0, Where, BP, &Out.Chain.back(), Err)) return false;
    std::string Path = Name(Root);
    for (const Json* Step : Steps)
    {
        FConstVal I;
        const Json* Index = Kind(*Step) == "MemberExpr" ? nullptr : Nth(*Step, 2);
        Path += Index ? "[" + (FoldConst(*Index, I) ? std::to_string(I.I) : std::string("...")) + "]" : "." + Name(*Step);
        FPropertyDef Reached;
        if (!EditStep(*Step, Out.Chain.back(), Where + ": " + Path, BP, Out.Path.emplace_back(), Reached, Err)) return false;
        Out.Chain.push_back(std::move(Reached));
    }
    if (!LowerDefault(Steps.empty() ? Root : *Steps.back(), Out.Chain.back(), BP, Err, Rhs, /*bKeepZero=*/true)) return false;
    if (Out.Chain.back().Default.K == FDefaultValue::None) { *Err = Where + ": " + Path + " needs a literal value"; return false; }
    return true;
}

/*
S38 - UE_ASSET_EDITS { Asset.Member = value; Asset.Array[1].Field = value; ... }: assignments to assets the game holds
(UE_ASSET_AT), each written into its asset's package as UE_ASSET_EDIT's braces are; a path changes only the part of the
member's value it names (ApplyEdit). Statements run in order, each asset's in one pass.
*/
bool FCompiler::GenerateAssetEdits(const Json& Block, std::string* Err)
{
    const std::string Where = "UE_ASSET_EDITS";
    const Json* Body = nullptr;
    ForEach(Block, [&](const Json& C) { if (Kind(C) == "CompoundStmt") Body = &C; });
    if (!Body) { *Err = Where + ": write it as `UE_ASSET_EDITS { Asset.Member = value; }`"; return false; }
    Cur = nullptr;
    struct FTarget
    {
        std::string Package, Object;
        std::unique_ptr<FPackage> Scratch;
        std::unique_ptr<FBlueprintClass> BP;
        std::vector<FEditDef> Edits;
    };
    std::map<std::string, FTarget> Targets;         // the asset's qualified name -> its package and assignments
    std::vector<std::string> Order;
    bool bOk = true;
    ForEach(*Body, [&](const Json& S) {
        if (!bOk) return;
        const Json *Asset = nullptr, *Root = nullptr, *Rhs = nullptr;
        std::vector<const Json*> Steps;
        if (!AssetPath(S, Asset, Root, Rhs, Steps) || !Asset->contains("referencedDecl"))
        {
            *Err = Where + ": every statement assigns an asset's member, or part of one: `Asset.Member = value;`, "
                   "`Asset.Member[2].Field = value;`";
            bOk = false;
            return;
        }
        const Json& D = (*Asset)["referencedDecl"];
        const auto Scope = VarScope.find(D.value("id", std::string()));
        const std::string Qual = (Scope == VarScope.end() ? std::string() : Scope->second) + Name(D);
        const auto At = AssetPaths.find(Qual);
        if (At == AssetPaths.end())
        {
            *Err = Where + ": " + Qual + " is not a UE_ASSET_AT (every asset in UeAssets/ is one); a mod's own asset "
                   "takes its values where it is declared";
            bOk = false;
            return;
        }
        FTarget& T = Targets[Qual];
        if (!T.Scratch)
        {
            T.Package = At->second;
            T.Object = SplitAssetPath(T.Package);
            T.Scratch = std::make_unique<FPackage>(T.Package);
            T.BP = std::make_unique<FBlueprintClass>(*T.Scratch, T.Object, "", "", false);
            Order.push_back(Qual);
        }
        const auto Owner = FieldOwner.find(Root->value("referencedMemberDecl", std::string()));
        const FRecord* DR = Owner == FieldOwner.end() ? nullptr : Find(Owner->second);
        if (!DR) { *Err = Where + ": cannot tell which class declares " + Name(*Root); bOk = false; return; }
        bOk = EditAssignment(*Root, *DR, Steps, Rhs, Where + " " + Qual, *T.BP, T.Edits.emplace_back(), Err);
    });
    if (!bOk) return false;
    if (Order.empty()) { *Err = Where + ": the block assigns nothing"; return false; }
    for (const std::string& Qual : Order)
    {
        FTarget& T = Targets[Qual];
        if (!ApplyEdit(T.Package, T.Object, std::move(T.Edits), *T.Scratch, Where + " " + Qual, Err)) return false;
    }
    return true;
}

/* S38: one step of a UE_DEFAULTS path into the value Parent is the def of: a `.Member` MemberExpr, or a TArray's
   `[i]` call. Out says where the step lands in the cooked bytes, Reached is the def of what it reaches. */
bool FCompiler::EditStep(const Json& Step, const FPropertyDef& Parent, const std::string& Where, FBlueprintClass& BP,
                         FValueStep& Out, FPropertyDef& Reached, std::string* Err)
{
    if (Kind(Step) == "MemberExpr")
    {
        const auto Owner = FieldOwner.find(Step.value("referencedMemberDecl", std::string()));
        const FRecord* SR = Owner == FieldOwner.end() ? nullptr : Find(Owner->second);
        if (!SR || Parent.Type != "StructProperty") { *Err = Where + ": cannot tell which struct declares " + Name(Step); return false; }
        Out.Member = UeNameOf(SR, Name(Step));
        if (!TypeToProperty(TypeOf(Step), Out.Member, 0, Where, BP, &Reached, Err)) return false;
        const int32 Native = NativeStructSize(Parent.StructName);
        if (!Native) return true;
        /* A native struct's bytes are its members' in declaration order, each of a fixed size. */
        int32 At = 0;
        for (const Json* F : SR->Fields)
        {
            FPropertyDef M;
            if (!TypeToProperty(TypeOf(*F), UeNameOf(SR, Name(*F)), 0, Where, BP, &M, Err)) return false;
            const int32 Size = FixedValueSize(M);
            if (!Size) { *Err = Where + ": " + SR->CppName + "::" + Name(*F) + " is not a fixed size, so its bytes cannot be found"; return false; }
            if (Name(*F) == Name(Step)) { Out.Offset = At; Out.Size = Size; }
            At += Size;
        }
        if (!SR->Base.empty() || Out.Offset < 0 || At > Native)
        { *Err = Where + ": UeApi's " + SR->CppName + " is not laid out the way the engine writes it"; return false; }
        Out.StructSize = Native;
        return true;
    }
    if (Parent.Type != "ArrayProperty" || !Parent.Inner)
    {
        *Err = Where + ": only a TArray's element can be assigned on its own; assign the whole "
             + std::string(Parent.Type == "MapProperty" ? "map" : Parent.Type == "SetProperty" ? "set" : "value");
        return false;
    }
    FConstVal V;
    const Json* Index = Nth(Step, 2);
    if (!Index || !FoldConst(*Index, V) || V.bFloat || V.I < 0 || V.I > 0x7FFFFFFF)
    { *Err = Where + ": an element's index is a constant, 0 or more"; return false; }
    Out.Element = int32(V.I);
    Reached = *Parent.Inner;
    Out.bStructElements = Reached.Type == "StructProperty";
    Out.ElementSize = FixedValueSize(Reached);
    if (!Out.ElementSize)
    {
        if (Reached.Type == "StrProperty") Out.ElementKind = FValueStep::String;
        else if (Reached.Type == "SoftObjectProperty" || Reached.Type == "SoftClassProperty") Out.ElementKind = FValueStep::SoftPath;
        else if (Reached.Type == "StructProperty") Out.ElementKind = FValueStep::Tags;
        else { *Err = Where + ": an element of a TArray of " + Reached.Type + " cannot be assigned on its own yet; assign the whole array"; return false; }
    }
    return true;
}

/* The game's Package, read from GameDir the first time an edit names it; SaveEdits writes it once every edit is in. */
FCompiler::FEdited* FCompiler::LoadEdited(const std::string& Package, const std::string& Where, std::string* Err)
{
    if (auto Slot = Edited.find(Lower(Package)); Slot != Edited.end()) return &Slot->second;
    if (GameDir.empty())
    {
        *Err = Where + ": an edit starts from the game's own package: pass the folder /Game is in (assetgen compile --game "
               "<extracted pak>/FSD/Content; bpbuild: game_content in mods.yaml)";
        return nullptr;
    }
    if (Package.compare(0, 6, "/Game/") != 0) { *Err = Where + ": " + Package + " is not a /Game package"; return nullptr; }
    FEdited E;
    E.Package = Package;
    const std::string Base = (std::filesystem::u8path(GameDir) / std::filesystem::u8path(Package.substr(6))).u8string();
    for (const char* Ext : { ".uasset", ".umap" })
        if (std::filesystem::exists(std::filesystem::u8path(Base + Ext))) { E.Ext = Ext; break; }
    if (E.Ext.empty()) { *Err = Where + ": " + Package + " is not in " + GameDir; return nullptr; }
    std::string LoadErr;
    if (!E.P.Load(Base + E.Ext, &LoadErr)) { *Err = Where + ": " + Package + ": " + LoadErr; return nullptr; }
    return &Edited.emplace(Lower(Package), std::move(E)).first->second;
}

/* From's import Index as an FPackageIndex of P, the cooked package named Package. An object inside P is its export: a
   patch's function names its own class, other functions of it, its default object. Anything else is an import of P,
   found or appended after its outers. 0, with Err set, for an object of P that P does not hold. */
static int32 ImportInto(FCookedPackage& P, const std::string& Package, const FPackage& From, int32 Index,
                        std::map<int32, int32>& Moved, std::string* Err)
{
    if (Index >= 0) return 0;
    if (const auto It = Moved.find(Index); It != Moved.end()) return It->second;
    std::vector<const FImport*> Chain;                              // the object first, its package last
    for (int32 I = Index; I < 0; I = From.ImportAt(FIndex{ I })->Outer.V) Chain.push_back(From.ImportAt(FIndex{ I }));
    int32 Out = 0;
    if (Lower(Chain.back()->ObjectName) == Lower(Package))
    {
        if (Chain.size() == 1) { *Err = "a reference to " + Package + " itself"; return 0; }
        for (size_t K = Chain.size() - 1; K-- > 0;)
        {
            const int32 E = P.FindExport(Chain[K]->ObjectName, Out);
            if (E < 0) { *Err = Package + " holds no " + Chain[K]->ObjectName; return 0; }
            Out = E + 1;
        }
    }
    else
    {
        const FImport* Im = Chain.front();
        const int32 Outer = Im->Outer.V ? ImportInto(P, Package, From, Im->Outer.V, Moved, Err) : 0;
        if (Im->Outer.V && !Outer) return 0;
        Out = P.Import(Im->ClassPackage, Im->ClassName, Outer, Im->ObjectName);
    }
    return Moved[Index] = Out;
}

/*
Edits, lowered against From (a scratch package, whose imports their objects are), into the tags of Object in the game's
Package: read from GameDir the first time, saved by SaveEdits once every edit is in. A member's tag replaces the one of
its name or is appended; an assignment to part of a member's value (a path) changes that part of the tag's value and
keeps the rest (SetTagPath). An object a tag points at becomes an import of the package, found or appended, and one the
object is created before being serialized - the edge the cook gives such a reference; a user-defined struct or enum a
tag names too. Everything else is the game's, byte for byte.
*/
bool FCompiler::ApplyEdit(const std::string& Package, const std::string& Object, std::vector<FEditDef> Edits,
                          const FPackage& From, const std::string& Where, std::string* Err)
{
    FEdited* Ed = LoadEdited(Package, Where, Err);
    if (!Ed) return false;
    FCookedPackage& P = Ed->P;
    /* Object is a path of names from the package down, `:`-separated: `Default__X_C:HealthComponent`. */
    int32 Export = -1;
    for (size_t From = 0, Colon = 0; Colon != std::string::npos; From = Colon + 1)
    {
        Colon = Object.find(':', From);
        Export = P.FindExport(Object.substr(From, Colon == std::string::npos ? std::string::npos : Colon - From), Export + 1);
        if (Export < 0) break;
    }
    if (Export < 0)
    {
        *Err = Where + ": " + Package + " holds no " + Object;
        if (Object.size() > 13 && Object.compare(Object.size() - 13, 13, "_GEN_VARIABLE") == 0)
            *Err += " - the class does not override that inherited component. Patch the Blueprint that declares it instead "
                    "(every child that does not override it changes too); adding an override record here is not built yet";
        return false;
    }

    /* From's imports, made again in P. From has no exports - every object a default can name is an import (AssetRef). */
    std::map<int32, int32> Moved;
    std::string MoveErr;
    auto Move = [&](int32 Index) { return ImportInto(P, Package, From, Index, Moved, &MoveErr); };
    /* A deep copy on the way: members and elements are shared_ptrs a type's other properties may hold too. */
    std::vector<int32> Deps;
    std::function<void(FPropertyDef&)> MoveDef;
    std::function<void(FDefaultValue&)> MoveValue = [&](FDefaultValue& V) {
        if (V.K == FDefaultValue::Obj && V.Object.V != 0) Deps.push_back(V.Object.V = Move(V.Object.V));
        for (FDefaultValue& Item : V.Items) MoveValue(Item);
        if (V.Members)
        {
            V.Members = std::make_shared<std::vector<FPropertyDef>>(*V.Members);
            for (FPropertyDef& M : *V.Members) MoveDef(M);
        }
    };
    MoveDef = [&](FPropertyDef& D) {
        if (D.Members)
        {
            D.Members = std::make_shared<std::vector<FPropertyDef>>(*D.Members);
            for (FPropertyDef& M : *D.Members) MoveDef(M);
        }
        for (std::shared_ptr<FPropertyDef>* Sub : { &D.Inner, &D.Value })
            if (*Sub) { *Sub = std::make_shared<FPropertyDef>(**Sub); MoveDef(**Sub); }
        MoveValue(D.Default);
    };
    for (FEditDef& E : Edits)
        for (FPropertyDef& D : E.Chain)
        {
            MoveDef(D);
            const FImport* Named = From.ImportAt(D.Extra);
            if (Named && (Named->ClassName == "UserDefinedStruct" || Named->ClassName == "UserDefinedEnum"))
                Deps.push_back(Move(D.Extra.V));
        }
    if (!MoveErr.empty()) { *Err = Where + ": " + MoveErr; return false; }

    /* Each assignment's bytes, written against P's names (a name P lacks is appended), then read back as P's own.
       Fresh[K] is the tag holding only the path below step K, for each K it can be one (every step on is a member of a
       struct written as tags): Fresh[0] the member's own tag, whole when there is no path. Leaf is the value's bytes
       when the path ends in a native struct or at an element. */
    std::vector<std::string> Texts;
    for (const FCookedName& N : P.Names) Texts.push_back(N.Text);
    FPackage Sink(Package);
    Sink.SeedNames(Texts);
    auto Bytes = [&](const auto& Write) { FArc Ar(&Sink); Write(Ar); return Ar.B; };
    std::vector<std::vector<std::vector<uint8>>> Fresh(Edits.size());
    std::vector<std::vector<uint8>> Leaf(Edits.size());
    for (size_t I = 0; I < Edits.size(); ++I)
    {
        const FEditDef& E = Edits[I];
        const size_t N = E.Path.size();
        Fresh[I].resize(N + 1);
        FPropertyDef Tree = E.Chain[N];
        for (size_t K = N + 1; K-- > 0;)
        {
            if (K < N)
            {
                if (!E.Path[K].IsTaggedMember()) break;
                FPropertyDef Up = E.Chain[K];
                Up.Members = std::make_shared<std::vector<FPropertyDef>>(1, Tree);
                Up.Default = FDefaultValue{};
                Up.Default.K = FDefaultValue::Struct;
                Tree = std::move(Up);
            }
            if (K == 0 || E.Path[K - 1].IsTaggedMember())
                Fresh[I][K] = Bytes([&](FArc& Ar) { WriteDefaultTag(Ar, Tree); TagEnd(Ar); });
        }
        if (N && !E.Path.back().IsTaggedMember()) Leaf[I] = Bytes([&](FArc& Ar) { WriteDefaultValue(Ar, E.Chain[N]); });
    }
    for (size_t I = P.Names.size(); I < Sink.NameTable().size(); ++I) P.Names.push_back({ Sink.NameTable()[I] });

    /* In statement order, so a later assignment lands on what an earlier one wrote. */
    for (size_t I = 0; I < Edits.size(); ++I)
    {
        if (Edits[I].Path.empty())
        {
            std::vector<FTag> Tags;
            size_t End = 0;
            if (!ReadTags(P, Fresh[I][0], End, Tags) || End != Fresh[I][0].size() || Tags.size() != 1)
            { *Err = Where + ": internal: the edit's tag does not read back"; return false; }
            if (!SetTags(P, Export, Tags, Err)) { *Err = Where + ": " + *Err; return false; }
        }
        else if (!SetTagPath(P, Export, Edits[I].Chain[0].Name, Edits[I].Path, Fresh[I], Leaf[I], Err))
        { *Err = Where + ": " + *Err; return false; }
    }
    for (int32 Dep : Deps) P.CreateBeforeSerialize(Export, Dep);
    Ed->Objects.push_back(Object + " (" + std::to_string(Edits.size()) + (Edits.size() == 1 ? " assignment)" : " assignments)"));
    return true;
}

/*
S38 - a patch's methods, compiled by Generate as a scratch child class of the Blueprint B (each an override of B's
function), written over B's own functions in B's package. The export row stays, so the class's function list, its
FuncMap and every caller reach the same object. So do the function's SuperStruct and FunctionFlags: its place in the
class chain, and what callers and the replication layer expect of it. The rest is the new function's: its properties
(the parameters, then its locals) and its script. The event-graph link goes, since the body no longer jumps into the
ubergraph. The payload is written with the cooked package's names, a missing one appended, and every object index
remapped: the scratch class, its default object and its functions to B's; an object inside B's package to its export;
anything else to an import of it, found or appended. A replaced function that a body calls as `Parent::Method()` keeps
the game's body in a copy of its export, which that call reaches instead (a UFunction links itself as it loads,
Class.cpp:1881, so the copy needs only its row, its dependencies and its place in the class's Children and FuncMap).
*/
bool FCompiler::TransplantFunctions(const FRecord& R, const FRecord& B, const FPackage& Scratch, FIndex ScratchClass,
                                    std::string* Err)
{
    const std::string Where = R.CppName + " (UE_PATCH)";
    const std::string Package = PackageOf(B), Class = ClassOf(B);
    FEdited* Ed = LoadEdited(Package, Where, Err);
    if (!Ed) return false;
    FCookedPackage& P = Ed->P;
    const int32 ClassExport = P.FindExport(Class);
    if (ClassExport < 0) { *Err = Where + ": " + Package + " holds no " + Class; return false; }
    const std::vector<FExport>& Rows = Scratch.ExportRows();
    auto IsFunction = [&](const FExport& F) {       // not the SCS root an actor class gets beside them
        const FImport* Of = Scratch.ImportAt(F.ClassIndex);
        return F.OuterIndex.V == ScratchClass.V && Of && Of->ObjectName == "Function";
    };

    /* What a method would override (Generate wrote none into the scratch), and whether that is an object of B's own. */
    auto SuperOf = [&](const FExport& F) {
        const auto It = PatchSupers.find(R.CppName + "::" + F.ObjectName);
        return It != PatchSupers.end() ? It->second : FIndex{};
    };
    auto PackageOfImport = [&](FIndex I) {
        while (Scratch.ImportAt(I)->Outer.V < 0) I = Scratch.ImportAt(I)->Outer;
        return Lower(Scratch.ImportAt(I)->ObjectName);
    };
    auto InB = [&](FIndex I) { return I.V < 0 && PackageOfImport(I) == Lower(Package); };
    /* The scratch names objects of its own package (its class, a function of it) as imports too, as every compile
       does (a dependency on a method it calls by name); here they are B's, found by name once the added rows exist. */
    const std::string Own = Lower(PackageOf(R)), ScratchName = Rows[size_t(ScratchClass.V - 1)].ObjectName;
    auto IsOwn = [&](FIndex I) { return I.V < 0 && PackageOfImport(I) == Own; };
    auto OwnInB = [&](FIndex I) -> int32 {
        const FImport* Im = Scratch.ImportAt(I);
        const FImport* Outer = Scratch.ImportAt(Im->Outer);
        if (Outer->Outer.V == 0)                                    // in the package itself: the class, its default
        {
            if (Im->ObjectName == ScratchName) return ClassExport + 1;
            if (Im->ObjectName == "Default__" + ScratchName) return P.FindExport("Default__" + Class) + 1;
        }
        else if (Outer->ObjectName == ScratchName && Scratch.ImportAt(Outer->Outer)->Outer.V == 0)
            return P.FindExport(Im->ObjectName, ClassExport + 1) + 1;  // a function of the class
        return 0;
    };

    /* A method replaces B's function of its name. Where B has none it is added to B: a new function, or an override of
       one B inherits (from a parent Blueprint, or native). */
    std::set<std::string> Added;            // lowercased
    for (const FExport& F : Rows)
    {
        if (!IsFunction(F)) continue;
        const int32 Fn = P.FindExport(F.ObjectName, ClassExport + 1);
        if (Fn >= 0 && P.ClassNameOf(P.Exports[size_t(Fn)].Class) == "Function") continue;
        if (Fn >= 0)
        {
            *Err = Where + "::" + F.ObjectName + ": " + Class + "'s " + F.ObjectName + " is a "
                   + P.ClassNameOf(P.Exports[size_t(Fn)].Class) + ", not a function";
            return false;
        }
        if (InB(SuperOf(F)))
        {
            *Err = Where + "::" + F.ObjectName + ": " + Class + " has no function of that name of its own, though its "
                   "declaration has one: the declaration is not the game's";
            return false;
        }
        Added.insert(Lower(F.ObjectName));
    }

    /* Pass one: the imports the functions name. Each is made P's before the writer seeds its names from P, since an
       import adds its names to P's table. An added function's row and dependencies are written too, not kept. */
    std::map<int32, int32> Moved;
    std::set<int32> InBodies;               // the imports the payloads name, as against only their dependencies
    std::string Bad;
    {
        std::set<int32> Named;
        FPackage Probe(Package);
        Probe.RemapIndex = [&](FIndex V) { if (V.V < 0) InBodies.insert(V.V); return V; };
        for (const FExport& F : Rows)
            if (IsFunction(F))
            {
                FArc Ar(&Probe);
                F.Serialize(Ar);
                for (int32 Dep : F.CreateBeforeSer) if (Dep < 0) Named.insert(Dep);
                if (!Added.count(Lower(F.ObjectName))) continue;
                for (const std::vector<int32>* List : { &F.SerBeforeSer, &F.SerBeforeCreate, &F.CreateBeforeCreate })
                    for (int32 Dep : *List) if (Dep < 0) Named.insert(Dep);
                for (const FIndex I : { F.ClassIndex, F.TemplateIndex, SuperOf(F) }) if (I.V < 0) Named.insert(I.V);
            }
        Named.insert(InBodies.begin(), InBodies.end());
        for (int32 I : Named)
            if (!IsOwn(FIndex{ I }) && !ImportInto(P, Package, Scratch, I, Moved, &Bad)) { *Err = Where + ": " + Bad; return false; }
    }

    /* `Parent::Method()` names a function of B's that the patch replaces: it calls the game's body, kept as a copy
       beside it (<Method>__Vanilla) and listed in the class as any function of it is. The copy overrides nothing. A
       dependency alone is no call: a scratch function depends on the function it would override. */
    std::map<int32, int32> Kept;            // a replaced function's FPackageIndex -> its copy's
    for (const int32 From : InBodies)
    {
        if (IsOwn(FIndex{ From })) continue;
        const int32 To = Moved.at(From);
        if (To <= 0 || Kept.count(To)) continue;
        const std::string Name = P.NameOf(P.Exports[size_t(To - 1)].ObjectName);
        if (P.Exports[size_t(To - 1)].Outer != ClassExport + 1 || P.ClassNameOf(P.Exports[size_t(To - 1)].Class) != "Function"
            || std::none_of(Rows.begin(), Rows.end(), [&](const FExport& F) { return IsFunction(F) && Lower(F.ObjectName) == Lower(Name); }))
            continue;
        FFunctionLayout L;
        if (!ReadFunctionLayout(P, P.Exports[size_t(To - 1)].Payload, L))
        { *Err = Where + "::" + Name + ": the game's function does not read as a cooked UFunction"; return false; }
        const std::string CopyName = Name + "__Vanilla";
        if (P.FindExport(CopyName, ClassExport + 1) >= 0)
        { *Err = Where + ": " + Class + " already holds a " + CopyName + " (patched before?)"; return false; }
        const FNameRef CopyRef = P.NameRef(CopyName);
        const int32 Copy = P.CopyExport(To - 1, CopyRef);
        FCookedExport& C = P.Exports[size_t(Copy - 1)];
        std::memset(C.Payload.data() + L.Super, 0, 4);         // SuperStruct
        C.Super = 0;
        /* Nothing overrides the copy, and Parent:: calls it by pointer, which the editor does only for a final function.
           An RPC's copy is a plain function. With FUNC_Net and no super it would be a net field of the class's own
           (Class.cpp:4189), shifting the RPC indices against the game's, and a call to it would be routed again
           (CallFunction's call space). Parent:: means the body, run where the RPC already arrived. */
        uint32 CopyFlags = L.FunctionFlags | FUNC_Final;
        if (L.FunctionFlags & FUNC_Net)
        {
            CopyFlags &= ~uint32(FUNC_Net | FUNC_NetReliable | FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast | FUNC_NetValidate);
            C.Payload.erase(C.Payload.begin() + std::ptrdiff_t(L.Flags + 4), C.Payload.begin() + std::ptrdiff_t(L.Flags + 6));   // RepOffset
        }
        std::memcpy(C.Payload.data() + L.Flags, &CopyFlags, 4);
        if (!AddClassFunction(P, ClassExport, Copy, CopyRef, &Bad)) { *Err = Where + ": " + Bad; return false; }
        Kept[To] = Copy;
        Ed->Objects.push_back(Class + "::" + CopyName + " (the game's " + Name + ", kept)");
    }

    /* An added function's row, before pass two: a method calling it by index finds it there. Its payload and its
       dependencies are pass two's. */
    for (const FExport& F : Rows)
    {
        if (!IsFunction(F) || !Added.count(Lower(F.ObjectName))) continue;
        FCookedExport E;
        E.Class = Moved.at(F.ClassIndex.V);
        E.Template = Moved.at(F.TemplateIndex.V);
        E.Outer = ClassExport + 1;
        const FIndex Super = SuperOf(F);
        E.Super = Super.V < 0 ? Moved.at(Super.V) : 0;
        E.ObjectName = P.NameRef(F.ObjectName);
        E.ObjectFlags = F.ObjectFlags;
        P.Exports.push_back(std::move(E));
        if (!AddClassFunction(P, ClassExport, int32(P.Exports.size()), P.Exports.back().ObjectName, &Bad))
        { *Err = Where + ": " + Bad; return false; }
        Ed->Objects.push_back(Class + "::" + F.ObjectName + (Super.V < 0 ? " (added, an override)" : " (added)"));
    }

    /* Pass two, for real. */
    int32 Replacing = 0;                    // the cooked function being written, as an FPackageIndex
    FPackage Sink(Package);
    {
        std::vector<std::string> Texts;
        for (const FCookedName& N : P.Names) Texts.push_back(N.Text);
        Sink.SeedNames(Texts);
    }
    Sink.RemapIndex = [&](FIndex V) -> FIndex {
        if (IsOwn(V))
        {
            if (Scratch.ImportAt(V)->Outer.V == 0) return Null();      // the package itself: a dependency, at most
            const int32 E = OwnInB(V);
            if (E <= 0 && Bad.empty()) Bad = "names " + Scratch.ImportAt(V)->ObjectName + " of its own, which " + Class + " does not hold";
            return FIndex{ E };
        }
        if (V.V < 0)
        {
            const int32 To = Moved.at(V.V);
            const auto K = Kept.find(To);
            return FIndex{ K != Kept.end() ? K->second : To };
        }
        if (V.V == 0) return V;
        /* The scratch class is B's class, its default object B's, a function of it B's function of that name. */
        const FExport& X = Rows[size_t(V.V - 1)];
        std::string Name = X.ObjectName;
        int32 Outer = 0;
        if (V.V == ScratchClass.V) Name = Class;
        else if (X.OuterIndex.V == 0 && X.ObjectName.compare(0, 9, "Default__") == 0) Name = "Default__" + Class;
        else if (IsFunction(X)) Outer = ClassExport + 1;
        else { if (Bad.empty()) Bad = "uses " + X.ObjectName + ", which a patch does not cook"; return Null(); }
        const int32 E = P.FindExport(Name, Outer);
        if (E < 0 && Bad.empty()) Bad = Package + " holds no " + Name;
        return FIndex{ E + 1 };
    };

    for (const FExport& F : Rows)
    {
        if (!IsFunction(F)) continue;
        const int32 Fn = P.FindExport(F.ObjectName, ClassExport + 1);
        const bool bAdded = Added.count(Lower(F.ObjectName)) != 0;
        FFunctionLayout Old, New;
        if (!bAdded && !ReadFunctionLayout(P, P.Exports[size_t(Fn)].Payload, Old))
        { *Err = Where + "::" + F.ObjectName + ": the game's function does not read as a cooked UFunction"; return false; }

        Replacing = Fn + 1;
        FArc Ar(&Sink);
        F.Serialize(Ar);
        if (bAdded)                         // its own run, at the end: the four phases in order
        {
            FCookedExport& E = P.Exports[size_t(Fn)];
            E.FirstExportDependency = int32(P.PreloadDependencies.size());
            int32* const Counts[] = { &E.SerBeforeSer, &E.CreateBeforeSer, &E.SerBeforeCreate, &E.CreateBeforeCreate };
            const std::vector<int32>* const Lists[] = { &F.SerBeforeSer, &F.CreateBeforeSer, &F.SerBeforeCreate, &F.CreateBeforeCreate };
            for (size_t K = 0; K < 4; ++K)
                for (int32 Dep : *Lists[K])
                    if (const FIndex To = Sink.RemapIndex(FIndex{ Dep }); To.V != 0) { P.PreloadDependencies.push_back(To.V); ++*Counts[K]; }
        }
        else
            for (int32 Dep : F.CreateBeforeSer)
                if (const FIndex To = Sink.RemapIndex(FIndex{ Dep }); To.V != 0 && To.V != Replacing) P.CreateBeforeSerialize(Fn, To.V);
        if (!Bad.empty()) { *Err = Where + "::" + F.ObjectName + ": " + Bad; return false; }
        for (size_t I = P.Names.size(); I < Sink.NameTable().size(); ++I) P.Names.push_back({ Sink.NameTable()[I] });
        if (!ReadFunctionLayout(P, Ar.B, New))
        { *Err = Where + "::" + F.ObjectName + ": internal: the compiled function does not read back"; return false; }
        if (bAdded)                         // all of it the method's, and the function it overrides, if any
        {
            std::vector<uint8> Out = Ar.B;
            std::memcpy(Out.data() + New.Super, &P.Exports[size_t(Fn)].Super, 4);
            P.Exports[size_t(Fn)].Payload = std::move(Out);
            continue;
        }

        /* The parameters stay the game's, byte for byte: every caller was cooked against them, and UeApi's `T&` cannot
           tell a Blueprint output pin (Parm | OutParm) from an in-out reference (+ ReferenceParm). So the method's must
           be the same list: in order, the same property class, name, size and direction. Only the locals are ours. */
        const std::vector<uint8>& Vanilla = P.Exports[size_t(Fn)].Payload;
        using FField = FFunctionLayout::FField;
        auto ParmsOf = [](const FFunctionLayout& L, bool bParm) {
            std::vector<const FField*> Out;
            for (const FField& Fd : L.Fields) if (((Fd.PropertyFlags & CPF_Parm) != 0) == bParm) Out.push_back(&Fd);
            return Out;
        };
        const std::vector<const FField*> Theirs = ParmsOf(Old, true), Ours = ParmsOf(New, true), Locals = ParmsOf(New, false);
        auto Listed = [&](const std::vector<const FField*>& List) {
            std::string S;
            for (const FField* Fd : List)
                S += (S.empty() ? "" : ", ") + P.NameOf(Fd->Type) + " " + P.NameOf(Fd->Name)
                     + (Fd->PropertyFlags & CPF_ReturnParm ? " (return)" : Fd->PropertyFlags & CPF_OutParm ? " (out)" : "");
            return "(" + S + ")";
        };
        constexpr uint64 kDirection = CPF_Parm | CPF_OutParm | CPF_ReturnParm;
        bool bSame = Theirs.size() == Ours.size();
        for (size_t I = 0; bSame && I < Theirs.size(); ++I)
            bSame = Lower(P.NameOf(Theirs[I]->Type)) == Lower(P.NameOf(Ours[I]->Type))
                    && Lower(P.NameOf(Theirs[I]->Name)) == Lower(P.NameOf(Ours[I]->Name))
                    && Theirs[I]->ElementSize == Ours[I]->ElementSize
                    && (Theirs[I]->PropertyFlags & kDirection) == (Ours[I]->PropertyFlags & kDirection);
        if (!bSame)
        {
            *Err = Where + "::" + F.ObjectName + ": its parameters " + Listed(Ours) + " are not the game function's "
                   + Listed(Theirs) + "; a replacement takes the same ones, in the same order";
            return false;
        }

        std::vector<uint8> Out(Ar.B.begin(), Ar.B.begin() + std::ptrdiff_t(New.Properties));
        std::memcpy(Out.data() + New.Super, Vanilla.data() + Old.Super, 4);                  // SuperStruct
        const int32 Count = int32(Theirs.size() + Locals.size());
        Out.insert(Out.end(), reinterpret_cast<const uint8*>(&Count), reinterpret_cast<const uint8*>(&Count) + 4);
        for (const FField* Parm : Theirs) Out.insert(Out.end(), Vanilla.begin() + std::ptrdiff_t(Parm->Begin), Vanilla.begin() + std::ptrdiff_t(Parm->End));
        for (const FField* Local : Locals) Out.insert(Out.end(), Ar.B.begin() + std::ptrdiff_t(Local->Begin), Ar.B.begin() + std::ptrdiff_t(Local->End));
        Out.insert(Out.end(), Ar.B.begin() + std::ptrdiff_t(New.Script), Ar.B.begin() + std::ptrdiff_t(New.Flags));      // the script
        const size_t FlagBytes = 4 + ((Old.FunctionFlags & FUNC_Net) ? 2 : 0);             // FunctionFlags, RepOffset
        Out.insert(Out.end(), Vanilla.begin() + std::ptrdiff_t(Old.Flags), Vanilla.begin() + std::ptrdiff_t(Old.Flags + FlagBytes));
        Out.resize(Out.size() + 8, 0);                                                      // no event-graph link
        P.Exports[size_t(Fn)].Payload = std::move(Out);
        Ed->Objects.push_back(Class + "::" + F.ObjectName + " (replaced)");
    }
    return true;
}

/* The edited packages, each at its own /Game path under the Content folder OutDir is in: the mod pak's copy is the one
   the game loads. */
bool FCompiler::SaveEdits(const std::string& OutDir, std::string* Err)
{
    for (auto& [Key, E] : Edited)
    {
        const std::filesystem::path File = FileOf(OutDir, E.Package);
        std::error_code Ec;
        std::filesystem::create_directories(File.parent_path(), Ec);
        if (!E.P.Save(File.u8string() + E.Ext, Err)) return false;
        std::string Objects;
        for (const std::string& O : E.Objects) Objects += (Objects.empty() ? "" : ", ") + O;
        printf("  %-14s -> %s%s  (the game's, edited: %s)\n", "edit", Shown(E.Package).c_str(), E.Ext.c_str(), Objects.c_str());
    }
    return true;
}

bool FCompiler::GenerateNestedWrappers(const std::string& OutDir, std::string* Err)
{
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
            if (!SavePackage(P, OutDir, PackageName, Err)) return false;
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

    const std::string PackageName = PackageOf(R);
    FPackage P(PackageName);
    StampIdentity(P, PackageName);

    /* Parent-is-Blueprint (/Game) decides the CDO's create-before-serialize edge onto it (measured on
       BP_ThornsComponent). */
    const std::string ParentPkg = PackageOf(*B);
    FBlueprintClass BP(P, ClassOf(R), ParentPkg, ClassOf(*B), ParentPkg.compare(0, 6, "/Game/") == 0);

    std::vector<std::string> Ancestry;
    for (const FRecord* A = &R; A; A = A->Base.empty() ? nullptr : Find(A->Base))
        if (!A->UeName.empty()) Ancestry.push_back(A->UeName);

    const bool bIsActor = std::find(Ancestry.begin(), Ancestry.end(), "Actor") != Ancestry.end();
    BP.SetIsActor(bIsActor);
    /* Abstract: the nearest declaration of some method along the class chain is `= 0`. SpawnActor and CreateWidget
       refuse the class, as they do one the editor marks Generate Abstract Class. An interface's `= 0` does not count,
       since an implementer that leaves it out gets a stub, and clang's own isAbstract never reaches here (FAstSax). */
    if (!CheckMemberNames(R, Err)) return false;
    bool bAbstract = false;
    std::set<std::string> Nearest;
    for (const FRecord* A = &R; A; A = A->Base.empty() ? nullptr : Find(A->Base))
        for (const auto& [Method, Decl] : A->Methods)
            if (Nearest.insert(Method).second && Decl->value("pure", false)) bAbstract = true;
    BP.SetClassFlags(ClassFlagsFor(Ancestry) | (bAbstract ? uint32(CLASS_Abstract) : 0u));

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
    if (R.Defaults && !R.bIsPatch)      // a patch's defaults are tags of the game's own package (GeneratePatch)
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
                const Json *Lhs = nullptr, *Rhs = nullptr, *Owner = nullptr;
                if (!DefaultAssignment(S, Lhs, Rhs, Owner))
                { *Err = Where + ": every statement is `Field = value;` or `Component->Field = value;`"; bOk = false; return; }

                /* `Comp->Field` reaches through a component; a bare `Field` targets this class's
                   own CDO. Which of the three destinations a statement means is decided by who
                   DECLARES the member it names, so no Super:: spelling is needed. */
                const bool bThroughComponent = Owner != nullptr;
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
                /* How the component was made decides how the actor keeps it: Instance or UserConstructionScript on a
                   template skips AddOwnedComponent (ActorComponent.cpp 282-287) and the name lookups a native
                   subobject answers (ActorComponent.cpp 1912; ActorConstruction.cpp 737). The engine sets it. */
                if (bThroughComponent && Name(*Lhs) == "CreationMethod")
                { *Err = Where + ": " + CompName + "->CreationMethod is set by the engine when it makes the component; drop it"; bOk = false; return; }

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

    /* The first scene component is the actor's root, and the engine puts it at the spawn transform: it never reads the
       root's RelativeLocation or RelativeRotation, and reads its RelativeScale3D for a C++ SpawnActor but not for
       Blueprint's Spawn Actor node (USCS_Node::ExecuteNodeOnActor, bIsDefaultTransform). So a plain SceneComponent root
       hands its transform to the components attached to it, composed as FTransform::Multiply composes a child with its
       parent - the rotations multiply (root first), the scales multiply, and a child's offset grows with the root's
       scale, turns with its rotation and adds its offset - and keeps none of it. A root that draws something itself
       would need its own, so that is only warned about. ponytail: a negative scale takes FTransform's matrix path
       and an absolute child ignores its parent; neither is special-cased here. */
    {
        auto IsScene = [&](const FRecord* C) {
            for (; C; C = C->Base.empty() ? nullptr : Find(C->Base)) if (C->UeName == "SceneComponent") return true;
            return false;
        };
        std::vector<std::pair<std::string, const FRecord*>> Scene;     // this class's scene components, the root first
        for (const Json* F : R.Fields)
        {
            const size_t Star = TypeOf(*F).find('*');
            const FRecord* CR = Star == std::string::npos ? nullptr : Find(StripTypeKeywords(TypeOf(*F).substr(0, Star)));
            if (R.Components.count(Name(*F)) && IsScene(CR)) Scene.emplace_back(Name(*F), CR);
        }
        /* A component's vector default: X, Y, Z (Or each, when it has none), and the def to write another like it. */
        struct FVec { std::array<double, 3> V; std::optional<FPropertyDef> Def; };
        auto Read = [](const std::vector<FPropertyDef>& Defs, const char* Prop, double Or) {
            FVec Out{ { Or, Or, Or }, std::nullopt };
            for (const FPropertyDef& D : Defs)
                if (D.Name == Prop) { Out.Def = D; for (int32 I = 0; I < 3; ++I) Out.V[I] = (*D.Members)[I].Default.F; }
            return Out;
        };
        auto Drop = [](std::vector<FPropertyDef>& Defs, const std::string& Prop) {
            Defs.erase(std::remove_if(Defs.begin(), Defs.end(), [&](const FPropertyDef& D) { return D.Name == Prop; }), Defs.end());
        };
        auto Write = [&](std::vector<FPropertyDef>& Defs, FPropertyDef Like, const std::array<double, 3>& V) {
            Drop(Defs, Like.Name);
            Like.Members = std::make_shared<std::vector<FPropertyDef>>(*Like.Members);     // its own, not the root's
            for (int32 I = 0; I < 3; ++I) (*Like.Members)[I].Default.F = V[I];
            Defs.push_back(Like);
        };
        /* FRotator::Quaternion, FQuat::operator* (B turns first), FQuat::RotateVector and FQuat::Rotator, as UE 4.27
           writes them. A rotator is Pitch, Yaw, Roll in degrees; a quaternion X, Y, Z, W. */
        using FQ = std::array<double, 4>;
        const double Pi = 3.14159265358979323846;
        auto ToQuat = [&](const std::array<double, 3>& Rot) {
            double S[3], C[3];
            for (int32 I = 0; I < 3; ++I) { S[I] = std::sin(std::fmod(Rot[I], 360.0) * Pi / 360); C[I] = std::cos(std::fmod(Rot[I], 360.0) * Pi / 360); }
            const double SP = S[0], SY = S[1], SR = S[2], CP = C[0], CY = C[1], CR = C[2];
            return FQ{ CR * SP * SY - SR * CP * CY, -CR * SP * CY - SR * CP * SY, CR * CP * SY - SR * SP * CY, CR * CP * CY + SR * SP * SY };
        };
        auto Mul = [](const FQ& A, const FQ& B) {
            return FQ{ A[3] * B[0] + A[0] * B[3] + A[1] * B[2] - A[2] * B[1], A[3] * B[1] - A[0] * B[2] + A[1] * B[3] + A[2] * B[0],
                       A[3] * B[2] + A[0] * B[1] - A[1] * B[0] + A[2] * B[3], A[3] * B[3] - A[0] * B[0] - A[1] * B[1] - A[2] * B[2] };
        };
        auto Cross = [](const std::array<double, 3>& A, const std::array<double, 3>& B) {
            return std::array<double, 3>{ A[1] * B[2] - A[2] * B[1], A[2] * B[0] - A[0] * B[2], A[0] * B[1] - A[1] * B[0] };
        };
        auto Rotate = [&](const FQ& Q, const std::array<double, 3>& V) {
            std::array<double, 3> T = Cross({ Q[0], Q[1], Q[2] }, V), Out;
            for (double& X : T) X *= 2;
            const std::array<double, 3> QT = Cross({ Q[0], Q[1], Q[2] }, T);
            for (int32 I = 0; I < 3; ++I) Out[I] = V[I] + Q[3] * T[I] + QT[I];
            return Out;
        };
        auto ToRotator = [&](const FQ& Q) {
            const double X = Q[0], Y = Q[1], Z = Q[2], W = Q[3], Test = Z * X - W * Y, Deg = 180 / Pi;
            const double Yaw = std::atan2(2 * (W * Z + X * Y), 1 - 2 * (Y * Y + Z * Z)) * Deg;
            auto Normalize = [](double A) { A = std::fmod(A, 360.0); if (A < 0) A += 360; return A > 180 ? A - 360 : A; };
            std::array<double, 3> Out;
            if (Test < -0.4999995) Out = { -90, Yaw, Normalize(-Yaw - 2 * std::atan2(X, W) * Deg) };
            else if (Test > 0.4999995) Out = { 90, Yaw, Normalize(Yaw - 2 * std::atan2(X, W) * Deg) };
            else Out = { std::asin(2 * Test) * Deg, Yaw, std::atan2(-2 * (W * X + Y * Z), 1 - 2 * (X * X + Y * Y)) * Deg };
            for (double& A : Out) A += 0.0;     // no -0 in the cooked float
            return Out;
        };
        if (!Scene.empty())
        {
            std::vector<FPropertyDef>& RootDefs = ComponentDefaults[Scene[0].first];
            const FVec Lr = Read(RootDefs, "RelativeLocation", 0), Rr = Read(RootDefs, "RelativeRotation", 0),
                       Sr = Read(RootDefs, "RelativeScale3D", 1);
            const bool bPlain = Scene[0].second->UeName == "SceneComponent";
            std::string Lost;
            auto Lose = [&](const FVec& V, const char* Prop) { if (!bPlain && V.Def) Lost += (Lost.empty() ? "" : ", ") + std::string(Prop); };
            Lose(Lr, "RelativeLocation");
            Lose(Rr, "RelativeRotation");
            Lose(Sr, "RelativeScale3D");
            if (!Lost.empty())
                printf("  warning: %s::UE_DEFAULTS: %s is the actor's root, which the engine puts at the spawn transform, so its "
                       "%s is not applied. A USceneComponent root passes its transform on to the components attached "
                       "to it.\n", R.CppName.c_str(), Scene[0].first.c_str(), Lost.c_str());
            if (bPlain && (Lr.Def || Rr.Def || Sr.Def))
            {
                const FQ Qr = ToQuat(Rr.V);
                for (size_t C = 1; C < Scene.size(); ++C)
                {
                    std::vector<FPropertyDef>& Defs = ComponentDefaults[Scene[C].first];
                    const FVec Lc = Read(Defs, "RelativeLocation", 0), Rc = Read(Defs, "RelativeRotation", 0),
                               Sc = Read(Defs, "RelativeScale3D", 1);
                    std::array<double, 3> L, S;
                    for (int32 I = 0; I < 3; ++I) { L[I] = Sr.V[I] * Lc.V[I]; S[I] = Sr.V[I] * Sc.V[I]; }
                    L = Rotate(Qr, L);
                    for (int32 I = 0; I < 3; ++I) L[I] += Lr.V[I];
                    if (Lc.Def || Lr.Def) Write(Defs, Lc.Def ? *Lc.Def : *Lr.Def, L);
                    if (Rc.Def || Rr.Def) Write(Defs, Rc.Def ? *Rc.Def : *Rr.Def, ToRotator(Mul(Qr, ToQuat(Rc.V))));
                    if (Sc.Def || Sr.Def) Write(Defs, Sc.Def ? *Sc.Def : *Sr.Def, S);
                }
                Drop(RootDefs, "RelativeLocation");
                Drop(RootDefs, "RelativeRotation");
                Drop(RootDefs, "RelativeScale3D");
            }
        }
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
        if (auto Cat = Decl.Categories.find(Name(*F)); Cat != Decl.Categories.end()) BP.ApiCategory[PD.Name] = Cat->second;
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
                /* A level's BSP: its Serialize reads a UModel, and PostLoad checks one is there. */
                if (A->UeName == "ModelComponent")
                { *Err = R.CppName + "::" + FieldName + ": a UModelComponent belongs to a level's BSP and cannot be a component template"; return false; }
            }
            if (!bIsComponent) { *Err = R.CppName + "::" + FieldName + ": " + CR->CppName + " is not a UActorComponent"; return false; }
            /* The variable stays an ordinary ObjectProperty: ExecuteNodeOnActor finds it by name and
               assigns the instance it built from the archetype. */
            BP.AddComponent(FieldName, BP.EngineClass(CR->UePackage, CR->UeName),
                            BP.ClassDefaultObject(CR->UePackage, CR->UeName), bIsScene,
                            ComponentDefaults[FieldName], NativeTail(CR));
            ComponentDefaults.erase(FieldName);
        }
        if (auto Rep = Decl.Replicated.find(FieldName); Rep != Decl.Replicated.end())
        {
            /* Measured on BP_LiftPod.IsLaunchEnabled: the editor's flags plus CPF_Net, and CPF_RepNotify with the
               function's name when it has one. */
            const std::string Notify = Rep->second.substr(0, Rep->second.find(':'));
            const std::string Cond = Rep->second.substr(Rep->second.find(':') + 1);
            if (const std::string Why = HoldsMapOrSet(PD) ? "a TMap or TSet" : Unreplicable(*F); !Why.empty())
            { *Err = R.CppName + "::" + FieldName + ": " + Why + " does not replicate"; return false; }
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
                /* RepLayout calls it through ProcessEvent with no room for a result; an inline method is no UFunction
                   at all, so a client's RepLayout would never find it and the OnRep would never run there. */
                if ((*NotifyFn)["type"].value("qualType", std::string()).rfind("void", 0) != 0)
                { *Err = R.CppName + "::" + FieldName + ": its RepNotify " + Notify + " must return void"; return false; }
                if (IsInlineMethod(R, Notify))
                { *Err = R.CppName + "::" + FieldName + ": its RepNotify " + Notify + " is inline, so no function of the class: drop inline"; return false; }
                PD.PropertyFlags |= CPF_RepNotify;
                PD.RepNotify = Notify;
            }
            static const char* const Conditions[] = { "None", "InitialOnly", "OwnerOnly", "SkipOwner", "SimulatedOnly",
                "AutonomousOnly", "SimulatedOrPhysics", "InitialOrOwner", "Custom", "ReplayOrOwner", "ReplayOnly",
                "SimulatedOnlyNoReplay", "SimulatedOrPhysicsNoReplay", "SkipReplay", "", "Never" };   // 14 is unused: COND_Never is 15 (CoreNetTypes.h 26)
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
    /* The override is an export of the parent CDO's subobject's own name and exact class, or FLinkerLoad::CreateExport
       makes a new component beside it. Neither is the member's: ACharacter's CapsuleComponent is CollisionCylinder, and
       a subclass can swap the class (APlayerCharacter's CharMoveComp). The nearest engine class's CDO says which. */
    const FRecord* EngineParent = Find(R.Base);
    while (EngineParent && EngineParent->UePackage.compare(0, 8, "/Script/") != 0) EngineParent = Find(EngineParent->Base);
    for (const auto& Entry : SubobjectDefaults)
    {
        const std::string* Sub = nullptr;
        if (EngineParent)
            if (auto It = EngineParent->Subobjects.find(Entry.first); It != EngineParent->Subobjects.end()) Sub = &It->second;
        const size_t Space = Sub ? Sub->find(' ') : std::string::npos;
        const size_t Dot = Sub ? Sub->rfind('.') : std::string::npos;
        if (Space == std::string::npos || Dot == std::string::npos || Dot < Space)
        {
            *Err = R.CppName + "::UE_DEFAULTS: UeApi does not say which default subobject " + Entry.first + " is on "
                   + (EngineParent ? EngineParent->UeName : R.Base) + " - regenerate it with genueapi, which reads that off the object dump";
            return false;
        }
        BP.AddSubobjectOverride(Sub->substr(0, Space), UeNameOf(Entry.second.Owner, Entry.first),
                                BP.EngineClass(Sub->substr(Space + 1, Dot - Space - 1), Sub->substr(Dot + 1)),
                                Entry.second.Defaults, NativeTail(Find("U" + Sub->substr(Dot + 1))));
    }
    for (const auto& Entry : ComponentOverrides)
    {
        /* The parent's own archetype, imported as a subobject of its class, is the record's template:
           the tags this class writes on top of it are exactly the overridden values. */
        const FRecord& Owner = *Entry.second.Owner;
        const FRecord* CR = ComponentClassOf(Owner, Entry.first);
        if (!CR || !CR->IsNative())
        { *Err = R.CppName + "::UE_DEFAULTS: cannot resolve the class of " + Entry.first; return false; }

        const std::string OwnerPkg = PackageOf(Owner), OwnerCls = ClassOf(Owner);
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
                                OwnerClass, NodeGuid, Entry.second.Defaults, NativeTail(CR));
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
        /* clang refuses an override of a final method, not one of the same name with other parameters, which a Blueprint
           would still take for its override: calls by name would reach it, calls bound to the final one would not. */
        for (const FRecord* A = R.Base.empty() ? nullptr : Find(R.Base); A; A = A->Base.empty() ? nullptr : Find(A->Base))
            if (A->FinalMethods.count(Fn.Name))
            {
                *Err = R.CppName + "::" + Fn.Name + ": " + A->CppName + "::" + Fn.Name + " is final, so no subclass may have a "
                       "function of that name; rename this one";
                return false;
            }
        /* `= 0` with no body anywhere is an empty function, like an interface's stub below: a subclass's version needs
           it as its super, and a call by name on an object without one would not find a function (a Fatal). */
        if ((Fn.Body = BodyOf(R, Fn.Name, Fn.Def)) || Fn.Decl->value("pure", false)) Methods.push_back(Fn);
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

    /* The functions UserConstructionScript reaches through calls to the class's own: a spawn in one returns None
       there as in the script itself (LowerCall). */
    UcsReached.clear();
    WarnedUcsSpawn.clear();
    for (std::vector<std::string> Todo = { "UserConstructionScript" }; !Todo.empty();)
    {
        const std::string Fn = Todo.back();
        Todo.pop_back();
        auto M = std::find_if(Methods.begin(), Methods.end(), [&](const FMethod& F) { return F.Name == Fn; });
        if (M == Methods.end() || !M->Body || !UcsReached.insert(Fn).second) continue;
        std::function<void(const Json&)> Scan = [&](const Json& N) {
            if (Kind(N) == "MemberExpr") Todo.push_back(N.value("name", std::string()));
            else if (Kind(N) == "DeclRefExpr" && N.contains("referencedDecl")) Todo.push_back(Name(N["referencedDecl"]));
            ForEach(N, Scan);
        };
        Scan(*M->Body);
    }

    /* A method that makes a latent call becomes a segment of ExecuteUbergraph_<Class> and keeps its name as a stub
       event that jumps in, as the editor compiles an event graph. Any class can: every BPGC object gets a persistent
       frame (FObjectInitializer::PostConstructInit), and UWorld::Tick resumes every object's actions
       (ProcessLatentActions(nullptr), LevelTick.cpp:1539), not only an actor's. What the call needs is a world, and
       these find their own; any other object has one only through its Outer (UObject::GetWorld, Obj.cpp:846). The
       editor hides Delay there for that reason alone (EdGraphSchema_K2.cpp:846, ImplementsGetWorld). */
    const bool bHasOwnWorld = std::any_of(Ancestry.begin(), Ancestry.end(), [](const std::string& A) {
        return A == "Actor" || A == "ActorComponent" || A == "UserWidget" || A == "GameInstance" || A == "Subsystem"; });
    /* Only an actor or a component replicates: UObject's GetLifetimeReplicatedProps lists nothing of a Blueprint's and
       its GetFunctionCallspace runs every RPC locally. An interface declares them for the classes implementing it. */
    if (!R.bIsInterface && std::none_of(Ancestry.begin(), Ancestry.end(), [](const std::string& A) {
            return A == "Actor" || A == "ActorComponent"; }))
    {
        std::string What = R.Replicated.empty() ? std::string() : "the replicated variable " + R.Replicated.begin()->first;
        for (const FMethod& Fn : Methods)
            if (What.empty() && (NetFlagsOf(*Fn.Decl) | NetFlagsOf(*Fn.Def))) What = "the RPC " + Fn.Name;
        if (!What.empty())
        { *Err = R.CppName + ": " + What + " does nothing, since only an actor or an actor component replicates"; return false; }
    }
    /* The engine routes a call by the called UFunction's flags (CallFunction, GetFunctionCallspace); an inline method is
       none, its body copied into each caller, so its marker would go nowhere. */
    for (const Json* M : R.AllMethods)
        if ((M->value("inline", false) || IsInlineMethod(R, Name(*M))) && (NetFlagsOf(*M) | AccessFlagsOf(*M)))
        { *Err = R.CppName + "::" + Name(*M) + ": an RPC, authority-only or cosmetic marker on an inline method does nothing, "
                 "since no call to it is routed; drop inline"; return false; }
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
        /* UFunction::NumParms is a uint8 and ParmsSize a uint16 (Class.h 1800-1805): past either, Link wraps them and
           ProcessEvent copies the wrong range (Class.cpp 5638-5651, ScriptCore.cpp 1952-1958). */
        if (Params.size() > 255)
        { *Err = R.CppName + "::" + Fn.Name + ": " + std::to_string(Params.size()) + " parameters, the return value included; a function takes at most 255"; return false; }
        {
            int64 ParmsEnd = 0;
            auto Add = [&](std::string T) {
                T = StripTypeKeywords(T);
                while (!T.empty() && (T.back() == '&' || T.back() == ' ')) T.pop_back();
                int32 Size = 0, Align = 1;
                std::string NoLayout;
                if (T.empty() || T == "void" || !LayoutOf(StripTypeKeywords(T), &Size, &Align, &NoLayout)) return;
                ParmsEnd = (ParmsEnd + Align - 1) / std::max(Align, 1) * std::max(Align, 1) + Size;
            };
            ForEach(M, [&](const Json& C) { if (Kind(C) == "ParmVarDecl") Add(TypeOf(C)); });
            Add(RetType);
            if (ParmsEnd > 65535)
            { *Err = R.CppName + "::" + Fn.Name + ": its parameters take " + std::to_string(ParmsEnd) + " bytes; a function's parameter block holds at most 65535"; return false; }
        }

        std::vector<FStmtIR> Stmts;
        std::vector<FPropertyDef> Locals;
        ReadScratchAdded = false;
        ReadTmpCounter = 0;
        RefAddr.clear();
        RefAlias.clear();
        RefPlace.clear();
        LocalRename.clear();
        ParmConst.clear();
        CurLocals = &Locals;
        LoopDepth = 0;
        SwitchDepth = 0;
        WriteBacks.clear();
        ReEntered = 0;
        GotoLabels.clear();
        bBodyHasGoto = Fn.Body && HasGoto(*Fn.Body);
        bFnHasGoto = bBodyHasGoto;
        KeepLoaded.clear();
        CurFnName = Fn.Name;
        CurFnDef = Fn.Def;
        bCurNet = (NetFlagsOf(Decl) | NetFlagsOf(M)) != 0;
        bCurNoOpt = IsNoOptDecl(Decl) || IsNoOptDecl(M);
        WarnedRefParms.clear();
        bMadeLatentCall = false;
        StaticLocal.clear();
        LatentCount = 0;
        Completions.clear();
        LatentRefusal = IsStaticDecl(Decl) ? "a static function has no object whose ubergraph frame could keep its locals"
                      : (!RetType.empty() && RetType != "void") || HasOutParm(Params)
                          ? "a function that resumes later returns nothing and takes no reference parameters"
                      : "";
        if (Fn.Body && !LowerBody(*Fn.Body, BP, Stmts, Locals, Err))
        {
            *Err = R.CppName + "::" + Fn.Name + ": " + *Err;
            return false;
        }
        /* A stub returns the default. A script caller's destination is the return parameter itself (ScriptCore.cpp
           ProcessScriptFunction: RetVal->PropAddr = RESULT_PARAM), so Return Nothing would leave its old value there. A
           local is zeroed and constructed on every call, the value the editor's unlinked result pin gives. */
        if (!Fn.Body && !RetType.empty() && RetType != "void")
        {
            FPropertyDef PD;
            if (!TypeToProperty(RetType, "__Default", 0, "return type on " + Fn.Name, BP, &PD, Err)) return false;
            PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
            Locals.push_back(PD);
            FStmtIR Ret;
            Ret.K = FStmtIR::Return;
            Ret.bHasValue = true;
            Ret.Value.K = FArgIR::Local;
            Ret.Value.S = PD.Name;
            Stmts.push_back(std::move(Ret));
        }
        if (!StaticLocal.empty() && !bMadeLatentCall)
        {
            *Err = R.CppName + "::" + Fn.Name + ": static " + StaticLocal + " lives in the ubergraph's frame, which only a "
                   "function that makes a latent call runs in; make " + StaticLocal + " a member";
            return false;
        }
        if (!HoistReadsInList(Stmts, BP, Locals, Err))
        {
            *Err = R.CppName + "::" + Fn.Name + ": " + *Err;
            return false;
        }
        if (!bCurNoOpt)
        {
            PruneConstBranches(Stmts);
            /* An inlined body's label re-enters the caller's statements too, and these passes see only While loops. */
            if (!bFnHasGoto && !bMadeLatentCall)
            {
                FlattenBlocks(Stmts);
                DropOverwritten(Stmts);
                ForwardSingleUse(Stmts, Stmts);
            }
            DropUnusedPure(Stmts);
            DropUnusedLocals(Stmts, Locals);
            if (!bFnHasGoto && !bMadeLatentCall)
            {
                ThreadBranches(Stmts, Stmts, Locals);
                CoalesceTemps(Stmts, Locals, BP);
            }
        }
        if (bMadeLatentCall && !PlaceActivations(Stmts, Err))
        {
            *Err = R.CppName + "::" + Fn.Name + ": " + *Err;
            return false;
        }
        for (const auto& [Struct, Keep] : KeepLoaded)
        {
            auto Typed = [&](const FPropertyDef& P) { return P.Type == "StructProperty" && P.Extra.V == Struct; };
            if (std::none_of(Params.begin(), Params.end(), Typed) && std::none_of(Locals.begin(), Locals.end(), Typed))
                Locals.push_back(Keep);
        }
        /* The VM constructs a local only under FUNC_HasDefaults (UFunction::InitializeDerivedMembers sets
           FirstPropertyToInit, Class.cpp 5653-5660; ScriptCore.cpp 909-916); without it the frame stays memzeroed, which
           is no FText, FTransform, FHitResult, TMap or defaulted struct. The editor sets it for any local without
           CPF_ZeroConstructor (KismetCompiler.cpp 2328-2335): the native structs below have it (Property.cpp 32-373,
           ZERO_NATIVE_STRUCTS in invariant_rules/functions.py); a Blueprint struct is taken to need it, its defaults unread. */
        static const std::set<std::string> ZeroStructs = {
            "Vector", "IntPoint", "IntVector", "Vector2D", "Vector4", "Plane", "Rotator", "Box", "Box2D", "Matrix",
            "BoxSphereBounds", "LinearColor", "Color", "TwoVectors", "Guid", "RandomStream", "DateTime", "Timespan",
            "SoftObjectPath", "SoftClassPath", "PrimaryAssetType", "PrimaryAssetId", "Margin" };
        const bool bLocalsNeedCtor = std::any_of(Locals.begin(), Locals.end(), [](const FPropertyDef& L) {
            return L.Type == "TextProperty" || L.Type == "SetProperty" || L.Type == "MapProperty" || L.Type == "SoftObjectProperty"
                || L.Type == "SoftClassProperty" || L.Type == "LazyObjectProperty" || L.Type == "FieldPathProperty"
                || (L.Type == "StructProperty" && !ZeroStructs.count(L.StructName)); });
        /* Locals follow ReturnValue in ChildProperties; the engine tells them apart by CPF_Parm. */
        for (const FPropertyDef& L : Locals) Params.push_back(L);

        const bool bEndsWithReturn = NeverFallsThrough(Stmts, true);
        const bool bScratchNeeded = ReadScratchAdded;
        const FIndex DerefStruct = bScratchNeeded
            ? BP.ScriptStruct(ModPackage + "/FDeref", "FDeref")
            : FIndex{};

        /* An override or interface implementation takes these of its parent's flags (KismetCompiler.cpp
           PrecompileFunction and the event stubs); measured on BP_SentryGun_MoveMarker. A static left
           FUNC_Event would be treated by the loader as an overridable entry point. */
        uint32 Inherited = 0;
        const FIndex Super = FindEvent(BP, R.CppName, Fn.Name, &Inherited);
        const auto [Owner, Replaced] = ReplacedDecl(R, Fn.Name);
        /* Only a Blueprint event is looked up by name; C++ and the calls bound to a native function (EX_FinalFunction)
           keep running it, so a function of the same name replaces it for some callers only. Events.json lists every
           event of the engine and the game; another mod's class, from its UE_CLASS header, is not in it. */
        if (Super.V != 0 && !(Inherited & FUNC_BlueprintEvent) && Owner && Owner->UePackage.compare(0, 8, "/Script/") == 0)
        { *Err = R.CppName + "::" + Fn.Name + ": " + Owner->CppName + "::" + Fn.Name + " is native and no Blueprint event, so "
                 "no function replaces it: C++ and the calls bound to it still run it; rename " + Fn.Name; return false; }
        /* A caller lays the parameters out for the function it names (ProcessEvent, an interface's Execute_, a received
           RPC), and ProcessEvent copies them into this one's frame at this one's offsets (ScriptCore.cpp:1958-2016). */
        if (Replaced && Replaced != &Decl && !R.bIsPatch && SignatureOf(*Replaced) != SignatureOf(Decl))     // a patch checks its own
        { *Err = R.CppName + "::" + Fn.Name + " is " + TypeOf(Decl) + ", and the " + Owner->CppName + "::" + Fn.Name
                 + " it replaces is " + TypeOf(*Replaced) + ": callers pass that one's parameters; declare the same"; return false; }
        uint32 Flags = Inherited ? Inherited & kOverrideInherits
                     : IsStaticDecl(Decl) ? uint32(FUNC_Static | FUNC_BlueprintCallable | FUNC_Public | FUNC_Final)
                     : kPlainMethodFlags;
        /* `final`, the class or the method: no subclass has a version of its own, and calls are bound to this one
           (LowerCall). Without BlueprintEvent the editor offers no override either (CanKismetOverrideFunction). */
        if (!Inherited && !IsStaticDecl(Decl) && (R.bFinal || R.FinalMethods.count(Fn.Name)))
            Flags = (Flags & ~uint32(FUNC_BlueprintEvent)) | FUNC_Final;
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
        if (bLocalsNeedCtor && !bMadeLatentCall) Flags |= FUNC_HasDefaults;     // a waiting one's locals are the ubergraph's
        if (const uint32 Net = NetFlagsOf(Decl) | NetFlagsOf(M))
        {
            /* Measured on BP_LiftPod: Server_ButtonPressedAnim is Net | NetServer, Multi_ButtonPressedAnim
               Net | NetMulticast; reliable adds NetReliable. A net function returns nothing (a reference parameter arrives as a copy),
               and an override keeps its parent's net flags (UClass::SetUpRuntimeReplicationData checks). */
            if ((Net & (FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast)) == 0)
            { *Err = R.CppName + "::" + Fn.Name + ": UE_RELIABLE needs UE_SERVER, UE_CLIENT or UE_MULTICAST"; return false; }
            /* The sender routes by one direction (Multicast, then Client, then Server: AActor::GetFunctionCallspace), the
               receiver accepts by its own flags, so two directions send one way and are dropped the other. */
            if ((Net & FUNC_NetMulticast) && (Net & (FUNC_NetServer | FUNC_NetClient)))
            { *Err = R.CppName + "::" + Fn.Name + ": an RPC goes one way: UE_MULTICAST with UE_SERVER or UE_CLIENT"; return false; }
            /* A static function's callspace comes from GetGlobalFunctionCallspace, which never answers Remote: it is never sent. */
            if (IsStaticDecl(Decl))
            { *Err = R.CppName + "::" + Fn.Name + ": a static function cannot be an RPC, it is never sent"; return false; }
            if (Super.V != 0) { *Err = R.CppName + "::" + Fn.Name + ": an override takes its parent's replication; drop the RPC marker"; return false; }
            if (!RetType.empty() && RetType != "void") { *Err = R.CppName + "::" + Fn.Name + ": an RPC returns void"; return false; }
            if (std::any_of(Params.begin(), Params.end(), [](const FPropertyDef& P) { return HoldsMapOrSet(P); }))
            { *Err = R.CppName + "::" + Fn.Name + ": an RPC parameter cannot be a TMap or TSet, which do not replicate"; return false; }
            std::string Unsent;
            ForEach(Decl, [&](const Json& C) {
                if (Kind(C) == "ParmVarDecl" && Unsent.empty())
                    if (const std::string Why = Unreplicable(C); !Why.empty()) Unsent = Name(C) + " is or holds " + Why;
            });
            if (!Unsent.empty())
            { *Err = R.CppName + "::" + Fn.Name + ": an RPC parameter cannot hold what does not replicate: " + Unsent; return false; }
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

        if (auto Cat = R.Categories.find(Fn.Name); Cat != R.Categories.end()) BP.ApiCategory[UeNameOf(&R, Fn.Name)] = Cat->second;
        /* A patch's method is written over the function it overrides (TransplantFunctions), which keeps its own super:
           naming it here would make the function its own parent. One B lacks is added with this super. */
        if (R.bIsPatch) PatchSupers[R.CppName + "::" + UeNameOf(&R, Fn.Name)] = Super;
        BP.AddFunction(UeNameOf(&R, Fn.Name), R.bIsPatch ? Null() : Super, Params,
                       [Stmts, bEndsWithReturn, bScratchNeeded, DerefStruct, bOpt = !bCurNoOpt](FScript& S, FIndex SelfExp) {
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
            if (bOpt) S.ThreadJumps();
            S.EndOfScript();
        }, Flags);
    }

    if (!Segments.empty() && R.bIsPatch)
    {
        *Err = R.CppName + " (UE_PATCH)::" + Segments.front().Name + ": a patched function that waits (a latent call) needs "
               "an ubergraph of its own in the game's class; not built yet";
        return false;
    }
    if (!Segments.empty())
    {
        const std::string UberName = "ExecuteUbergraph_" + R.CppName;    // CheckMemberNames refuses a member named so

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
    if (R.bIsPatch) return TransplantFunctions(R, *B, P, BP.ClassIndex(), Err);
    if (!SavePackage(P, OutDir, PackageName, Err)) return false;
    if (ApiDir && !BP.WriteApi(*ApiDir, Err))
    {
        /* A class with nothing callable is not a build failure; the mod simply has no API surface. */
        printf("  %-14s -> no API asset: %s\n", R.CppName.c_str(), Err->c_str());
        Err->clear();
    }
    RegistryRows.push_back({ PackageName, ClassOf(R), "BlueprintGeneratedClass" });
    printf("  %-14s -> %s.uasset  (%s %s)\n", R.CppName.c_str(), Shown(PackageName).c_str(),
           B->IsNative() ? "extends" : "extends BP", R.Base.c_str());
    return true;
}

/* Builds the DOM of clang's AST dump as it streams in, without what nothing reads: source locations (all but a
   DeclRefExpr's range begin offset and token length, and its end's in a macro, see NamedQualifier), mangled names, a record's definitionData and
   a few flags are most of the dump, and building them was most of a compile. A key read later must not be in key(). */
class FAstSax : public nlohmann::json_sax<Json>
{
public:
    explicit FAstSax(Json& InRoot) : Root(InRoot) {}
    bool null() override { return Value(nullptr); }
    bool boolean(bool V) override { return Value(V); }
    bool number_integer(number_integer_t V) override { return Value(V); }
    bool number_unsigned(number_unsigned_t V) override { return Value(V); }
    bool number_float(number_float_t V, const string_t&) override { return Value(V); }
    bool string(string_t& V) override
    {
        if (bKindNext && !Skipped) DeclRef.back() = V == "DeclRefExpr";   // clang writes "kind" before "range"
        return Value(std::move(V));
    }
    bool binary(binary_t& V) override { return Value(std::move(V)); }
    bool start_object(size_t) override { return Open(Json::value_t::object); }
    bool start_array(size_t) override { return Open(Json::value_t::array); }
    bool end_object() override { return Close(); }
    bool end_array() override { return Close(); }
    bool key(string_t& K) override
    {
        if (Skipped) return true;
        /* A spellingLoc only gets here inside a DeclRefExpr's range (every other loc and range is skipped whole); the
           range's end is kept only then, for a qualifier written in a macro (NamedQualifier). */
        const bool bMacroEnd = K == "end" && Stack.back()->is_object() && Stack.back()->contains("begin")
                            && (*Stack.back())["begin"].contains("spellingLoc");
        /* A variable's use flags stay: Run refuses a used UE_ASSET_AT that cannot load. A method's isImplicit stays: the
           operator= clang declares up front in a class with a virtual is no Blueprint function. "kind" comes before them. */
        bSkipNext = K == "loc" || (K == "end" && !bMacroEnd) || K == "file" || K == "line" || K == "col" || K == "includedFrom"
                 || K == "expansionLoc" || K == "isMacroArgExpansion" || K == "mangledName" || K == "definitionData"
                 || (K == "isImplicit" && Stack.back()->value("kind", std::string()) != "CXXMethodDecl")
                 || ((K == "isUsed" || K == "isReferenced") && Stack.back()->value("kind", std::string()) != "VarDecl")
                 || K == "typeAliasDeclId" || (K == "range" && !DeclRef.back());
        bKindNext = K == "kind";
        if (!bSkipNext) Slot = &(*Stack.back())[std::move(K)];
        return true;
    }
    bool parse_error(size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }

private:
    Json& Root;
    std::vector<Json*> Stack;       // the open objects and arrays being filled
    std::vector<bool> DeclRef;      // per open object or array: a DeclRefExpr, whose range is kept
    Json* Slot = nullptr;           // the object member the last key named
    int32 Skipped = 0;              // depth inside a dropped object or array
    bool bSkipNext = false;         // the next value is a dropped key's
    bool bKindNext = false;         // ... or "kind"'s

    Json* Place(Json&& V)
    {
        if (Stack.empty()) return &(Root = std::move(V));
        if (Stack.back()->is_array()) { Stack.back()->push_back(std::move(V)); return &Stack.back()->back(); }
        return &(*Slot = std::move(V));
    }
    bool Value(Json&& V)
    {
        if (!Skipped && !bSkipNext) Place(std::move(V));
        bSkipNext = bKindNext = false;
        return true;
    }
    bool Open(Json::value_t T)
    {
        if (Skipped || bSkipNext) ++Skipped;
        else { Stack.push_back(Place(Json(T))); DeclRef.push_back(false); }
        bSkipNext = bKindNext = false;
        return true;
    }
    bool Close()
    {
        if (Skipped) --Skipped;
        else { Stack.pop_back(); DeclRef.pop_back(); }
        return true;
    }
};

/* Parses clang's AST dump at Path. */
bool ParseAst(const std::string& Path, Json* Out, std::string* Err)
{
    std::vector<char> Buf(1 << 20);
    std::ifstream In;
    In.rdbuf()->pubsetbuf(Buf.data(), Buf.size());
    In.open(Path, std::ios::binary);
    if (!In || In.peek() == std::ifstream::traits_type::eof()) { *Err = "clang produced no AST at " + Path; return false; }
    FAstSax Sax(*Out);
    if (!Json::sax_parse(In, &Sax)) { *Err = "could not parse clang's AST dump"; return false; }
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
                          Row.value("fn", std::string()), ExtraArgs(Row), RefArgs(Row) });
    for (const Json& Row : OpsDoc)
        Ops.push_back({ Row.value("op", std::string()), Row.value("lhs", std::string()), Row.value("rhs", std::string()),
                        Row.value("ret", std::string()), Row.value("package", std::string()),
                        Row.value("class", std::string()), Row.value("fn", std::string()), ExtraArgs(Row), RefArgs(Row) });
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
                    const std::string& OutDir, const std::optional<std::string>& InApiDir, std::string* Err)
{
    ApiDir = InApiDir;
    /* In %TEMP%, not OutDir: bpbuild paks OutDir's whole tree. Named per process: bpbuild and the tests compile the
       same sources, and at once they overwrote each other's. Removed however Run returns: a dump is hundreds of MB,
       no later run overwrites it, and every refusal the tests expect is a failed compile. */
    std::error_code TmpEc;
    /* Absolute: a bare "Mod.cpp" has an empty parent, and NamedQualifier cannot list "". */
    SourceDir = std::filesystem::absolute(SourcePath, TmpEc).parent_path().string();
    const std::string AstPath = (std::filesystem::temp_directory_path(TmpEc)
                                 / (std::filesystem::path(SourcePath).stem().string() + "." + std::to_string(ProcessId())
                                    + ".assetgen-ast.json")).string();
    struct FRemoveAst { const std::string& Path; ~FRemoveAst() { remove(Path.c_str()); } } RemoveAst{ AstPath };
    /* Both the UeApi dir and its parent are include paths, so "FSD.h" and "UeApi/FSD.h" both resolve. Absolute
       first: a relative "UeApi" has an empty parent, and -I"" swallows the next argument. */
    const std::string Parent = std::filesystem::absolute(IncludeDir, TmpEc).parent_path().string();
    /* -Wno-string-plus-int: `"lit" + N` is a Concat_StrStr here, not pointer arithmetic. */
    std::string Cmd = "clang++ -std=c++20 -Wno-string-plus-int -fsyntax-only -Xclang -ast-dump=json";
#ifndef _WIN32
    /* Parse with the game's ABI, not the host's: on x86-64 Linux size_t is `unsigned long`, so sizeof has no
       Kismet conversion. The msvc target finds no C++ headers here and the SDK needs only <initializer_list>,
       so hand clang a stand-in (it checks only the two-pointer layout). */
    const std::filesystem::path ShimDir = std::filesystem::temp_directory_path(TmpEc) / "assetgen-include";
    std::filesystem::create_directories(ShimDir, TmpEc);
    std::ofstream(ShimDir / "initializer_list", std::ios::binary | std::ios::trunc)
        << "#pragma once\n"
           "namespace std {\n"
           "template <class E> class initializer_list {\n"
           "    const E* First = nullptr;\n"
           "    const E* Last = nullptr;\n"
           "public:\n"
           "    constexpr initializer_list() noexcept = default;\n"
           "    constexpr const E* begin() const noexcept { return First; }\n"
           "    constexpr const E* end() const noexcept { return Last; }\n"
           "    constexpr decltype(sizeof 0) size() const noexcept { return Last - First; }\n"
           "};\n"
           "}\n";
    Cmd += " --target=x86_64-pc-windows-msvc -isystem \"" + ShimDir.string() + "\"";
#endif
    Cmd += " \"" + SourcePath + "\" -I\"" + IncludeDir + "\" -I\"" + Parent + "\" > \"" + AstPath + "\"";
#ifdef _WIN32
    /* cmd /c strips the first and last quote of a line that starts with one, so wrap it in a spare pair. */
    if (system(("\"" + Cmd + "\"").c_str()) != 0)
#else
    if (system(Cmd.c_str()) != 0)
#endif
    {
        *Err = "clang rejected " + SourcePath + " (diagnostics above)";
        return false;
    }
    if (!ParseAst(AstPath, &Doc, Err)) return false;

    if (!LoadTables(IncludeDir, Err)) return false;
    if (!Collect(Err)) return false;
    /* Inline free functions anywhere in the translation unit, a template's instantiations included: a call names
       the instantiation's own FunctionDecl. Class bodies are skipped; their methods are the records'. */
    std::function<void(const Json&, bool)> IndexConsts = [&](const Json& N, bool bInClass) {
        const std::string K = Kind(N);
        if (K == "VarDecl" && N.contains("init") && N.contains("id")
            && (N.value("constexpr", false) || TypeOf(N).compare(0, 6, "const ") == 0))
            ConstVars[N.value("id", std::string())] = &N;
        else if (K == "VarDecl" && N.contains("id") && bInClass)
            MutableStatics[N.value("id", std::string())] = &N;
        if (K == "TranslationUnitDecl" || K == "NamespaceDecl" || K == "CXXRecordDecl" || K == "LinkageSpecDecl")
            ForEach(N, [&](const Json& C) { IndexConsts(C, K == "CXXRecordDecl"); });
    };
    IndexConsts(Doc, false);
    std::function<void(Json&)> IndexInlines = [&](Json& N) {
        if (!N.is_object()) return;
        const std::string K = Kind(N);
        if (K == "CXXRecordDecl")
        {
            /* Class bodies are the records', except member templates: no UFunction per instantiation, so each
               one a call names is inlined. The dependent pattern sits beside them and no call names it. */
            ForEach(N, [&](const Json& C) {
                if (Kind(C) == "CXXRecordDecl") IndexInlines(const_cast<Json&>(C));
                if (Kind(C) != "FunctionTemplateDecl") return;
                for (Json& M : const_cast<Json&>(C)["inner"])
                {
                    bool bBody = false;
                    ForEach(M, [&](const Json& B) { bBody = bBody || Kind(B) == "CompoundStmt"; });
                    if (Kind(M) == "CXXMethodDecl" && bBody)
                    {
                        NormalizePointers(M);
                        MemberTemplates[M.value("id", std::string())] = &M;
                    }
                }
            });
            return;
        }
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
        for (const auto& M : Entry.second.Inlines) NormalizePointers(const_cast<Json&>(*M.second));
    }

    /* A namespace is a folder (PathIn), so `Game::<the mod's own path>::X` is X's package, and a package name is
       case-blind. */
    std::map<std::string, std::string> Cooked;
    for (const auto& [Cpp, R] : Records)
        if (R.IsGenerated())
            if (const auto [At, bNew] = Cooked.emplace(Lower(PackageOf(R)), Cpp); !bNew)
            { *Err = Cpp + " and " + At->second + " would both be cooked as " + PackageOf(R); return false; }

    /* A UE_ASSET_AT whose class this mod cooks: the asset is an instance of the copy its own mod cooked, so the
       import's class check fails in game and the reference loads as null. Pinning the class to its owner with
       UE_CLASS makes every other mod import it instead. */
    for (const auto& [Qual, Path] : AssetPaths)
    {
        const auto D = NsVarNamed.find(Qual);
        if (D == NsVarNamed.end() || !(D->second->value("isUsed", false) || D->second->value("isReferenced", false))) continue;
        if (std::any_of(AssetDecls.begin(), AssetDecls.end(), [&](const Json* A) { return Name(*A) == LeafOf(Qual); })) continue;
        const Json& T = D->second->contains("type") ? (*D->second)["type"] : Json::object();
        const FRecord* R = Find(StripTypeKeywords(T.value("desugaredQualType", T.value("qualType", std::string()))));
        if (!R || R->bIsStruct || !R->IsGenerated()) continue;
        const std::string Leaf = LeafOf(R->CppName);
        *Err = Qual + " at " + Path + " is a " + R->CppName + ", which this mod cooks its own copy of, so it would load as null: "
               "name the class's owner where it is declared, e.g. UE_CLASS(\"" + Path.substr(0, Path.rfind('/') + 1) + Leaf
             + "\", \"" + Leaf + "_C\")";
        return false;
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
            /* Remove every generated asset. */
            for (const auto& Other : Records)
                if (Other.second.IsGenerated())
                    for (const char* Ext : { ".uasset", ".uexp" })
                        remove((FileOf(OutDir, PackageOf(Other.second)).string() + Ext).c_str());
            return false;
        }
        ++Generated;
    }
    /* The patches here, not after the edits: lowering a patch's methods discovers what the loops below cook, as the
       classes' lowering does (a global's class, the deref, a slot struct). */
    for (const auto& Entry : Records)
        if (Entry.second.bIsPatch && !GeneratePatch(Entry.second, Err)) return false;
    /* After the loops, not in them: lowering is what discovers the deref, and the mod's own
       FDeref (if it declared one) was cooked above as an ordinary record. */
    if (bSynthDeref)
    {
        if (!GenerateStruct(SynthDeref, OutDir, Err)) return false;
        ++Generated;
    }
    for (const auto& Entry : SlotStructs)       // the TMap walks lowering made; see MapSlotView
    {
        if (!GenerateStruct(Entry.second, OutDir, Err)) return false;
        ++Generated;
    }
    for (const auto& Entry : Globals)       // what the lowering above found used; see LowerGlobal
    {
        if (!Generate(Entry.second, OutDir, Err)) return false;
        ++Generated;
    }
    for (const auto& Entry : ModEnums)
    {
        if (!GenerateEnum(Entry.first, OutDir, Err)) return false;
        ++Generated;
    }
    for (const Json* Var : AssetDecls)
        if (!GenerateAsset(*Var, OutDir, Err)) return false;
    for (const auto& [Key, Var] : Edits)
        if (!GenerateEdit(Key, *Var, Err)) return false;
    for (const Json* Block : AssetEditBlocks)
        if (!GenerateAssetEdits(*Block, Err)) return false;
    if (!SaveEdits(OutDir, Err)) return false;
    if (Generated == 0 && RegistryRows.empty() && Edited.empty())
    { *Err = "the source declares no UE_STRUCT, UE_ENUM, class deriving from a UE class, asset or edit"; return false; }
    if (!GenerateNestedWrappers(OutDir, Err)) return false;

    /* A cooked package carries no registry data; without the bake the classes are invisible to it. A pak keeps its
       one registry beside its Content folder, FSD/AssetRegistry.bin, as the game's own pak and every editor-cooked
       mod pak do. So an OutDir of <root>/Content/<package path> (bpbuild's) puts it in <root>, merged with what other
       compiles into the same pak put there; any other OutDir gets its own.
       A compile with no rows (an edit-only one) writes none: a cooked game loads exactly one registry,
       ProjectDir()/AssetRegistry.bin (AssetRegistry.cpp:198), and an empty one in a pak mounted at startup would
       replace the game's. A registry another compile already put there is left as it is. */
    if (RegistryRows.empty())
    {
        printf("  %-14s -> none (no assets of its own)\n", "registry");
        remove(AstPath.c_str());
        return true;
    }
    std::string RegistryDir = OutDir;
    while (!RegistryDir.empty() && (RegistryDir.back() == '/' || RegistryDir.back() == '\\')) RegistryDir.pop_back();
    for (char& C : RegistryDir) if (C == '\\') C = '/';
    const std::string Tail = ModPackage.compare(0, 6, "/Game/") == 0 ? "/Content/" + ModPackage.substr(6) : std::string();
    if (!Tail.empty() && RegistryDir.size() > Tail.size()
        && Lower(RegistryDir.substr(RegistryDir.size() - Tail.size())) == Lower(Tail))
        RegistryDir.resize(RegistryDir.size() - Tail.size());
    else
        RegistryDir = OutDir;
    if (!MergeAssetRegistry(RegistryRows, RegistryDir + "/AssetRegistry.bin", Err)) return false;
    printf("  %-14s -> %s/AssetRegistry.bin  (%d asset%s)\n", "registry", RegistryDir == OutDir ? "." : RegistryDir.c_str(),
           int32(RegistryRows.size()), RegistryRows.size() == 1 ? "" : "s");
    return true;
}
}   // namespace

bool CompileToAssets(const std::string& SourcePath, const std::string& IncludeDir,
                     const std::string& OutDir, const std::optional<std::string>& ApiDir, const std::string& GameDir,
                     std::string* Err)
{
    /* Never freed: the process ends right after, and tearing the AST down node by node takes longer than exiting. */
    FCompiler* C = new FCompiler;
    C->GameDir = GameDir;
    return C->Run(SourcePath, IncludeDir, OutDir, ApiDir, Err);
}

}   // namespace Uasset
