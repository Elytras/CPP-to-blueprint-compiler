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

std::string TypeOf(const Json& N)
{
    auto It = N.find("type");
    return It == N.end() ? std::string() : It->value("qualType", std::string());
}

/* Stripping CXXConstructExpr drops any ctor args after the first; safe only because the
   Types.h wrappers take a single literal. */
const Json* Strip(const Json* N)
{
    while (N)
    {
        const std::string K = Kind(*N);
        if (K != "ImplicitCastExpr" && K != "CStyleCastExpr" && K != "ParenExpr"
            && K != "ConstantExpr" && K != "ExprWithCleanups"
            && K != "CXXBindTemporaryExpr" && K != "MaterializeTemporaryExpr"
            && K != "CXXConstructExpr" && K != "CXXFunctionalCastExpr"
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

bool IsStaticDecl(const Json& Decl) { return Decl.value("storageClass", std::string()) == "static"; }

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
    bool bIsLocal = false;      // UePackage == ModPackage/CppName: cooked here, published at its /Game path
    bool bIsStruct = false;     // UE_STRUCT: cooked as a UserDefinedStruct asset

    bool IsNative() const { return !UePackage.empty() && !bIsLocal; }
    bool IsGenerated() const { return !IsNative() && (bIsStruct || !Base.empty()); }
};

struct FCallIR;

struct FArgIR
{
    enum EKind { Self, Int, Int64, Float, Bool, Str, Name, Text, Field, Local, LocalOut, Member, Call, NullObj } K = Self;
    int32 I = 0;
    int64 I64 = 0;
    float F = 0.0f;
    bool B = false;
    bool bWide = false;     // Str: emit EX_UnicodeStringConst
    std::string S;          // Str: the literal; Field/Local/LocalOut/Member: the property name
    FIndex Owner;           // Field: declaring class; Member: the struct (locals are owned by the function, resolved at emit)
    EExprToken LetOp = EX_Let;
    std::string InnerType;          // clang type of the expression this was lowered from
    std::shared_ptr<FCallIR> Sub;
    std::shared_ptr<FArgIR> Base;   // Member: the struct-valued expression
};

enum EStrKind { SK_None, SK_Str, SK_Name, SK_Text, SK_Int, SK_Int64, SK_Float, SK_Bool, SK_Byte, SK_Object };

EStrKind KindByName(const std::string& N)
{
    static const std::map<std::string, EStrKind> M = {
        { "Str", SK_Str }, { "Name", SK_Name }, { "Text", SK_Text }, { "Int", SK_Int }, { "Int64", SK_Int64 },
        { "Float", SK_Float }, { "Bool", SK_Bool }, { "Byte", SK_Byte }, { "Object", SK_Object } };
    auto It = M.find(N);
    return It == M.end() ? SK_None : It->second;
}

const char* TypeNameOf(EStrKind K)
{
    switch (K)
    {
    case SK_Str: return "FString"; case SK_Name: return "FName"; case SK_Text: return "FText";
    case SK_Int: return "int"; case SK_Int64: return "int64"; case SK_Float: return "float";
    case SK_Bool: return "bool"; case SK_Byte: return "uint8"; case SK_Object: return "UObject *";
    default: return "";
    }
}

/* One row of UeApi/Conv.json: a Kismet Conv_XToY and the constants for its formatting parameters. */
struct FConv
{
    EStrKind From = SK_None, To = SK_None;
    std::string Package, Class, Fn;
    std::vector<Json> Extra;
};

struct FCallIR
{
    FIndex Fn;                          // empty for intrinsics
    std::string Intrinsic;              // __NAME__ compiler intrinsic
    FIndex Extra;
    FIndex Extra2;
    bool bScript = false;               // callee is Blueprint bytecode
    bool bInstance = false;             // non-static method: needs the context object, not the class CDO
    FIndex Context;                     // CDO a static call runs against; null = self
    std::string VirtualName;            // a generated class's own instance method: EX_VirtualFunction resolves it by name at run time
    std::shared_ptr<FArgIR> Target;     // the object an instance call runs against; null = self
    std::vector<FArgIR> Args;
};

/* EX_CallMath calls UFunction::Func with the CALLER's frame, which is only correct for a native;
   a bytecode callee must go through EX_FinalFunction (UFunction::Invoke builds its own frame).
   EX_CallMath also runs on the function's outer-class CDO and ignores EX_Context, so it is only
   right for a static: an instance native on it would run against e.g. Default__FSDGameState. */
void EmitCallOp(FScript& S, const FCallIR& Call)
{
    if (!Call.VirtualName.empty()) S.VirtualFunction(Call.VirtualName);
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
    bool bAssignLocal = false;
    bool bAssignOutParm = false;
};

/* SelfExp: the enclosing function's export index, FFieldPath owner of its params and locals. */
bool EmitArgs(FScript& S, const std::vector<FArgIR>& Args, FIndex SelfExp, std::string* Err);

bool EmitArg(FScript& S, const FArgIR& A, FIndex SelfExp, std::string* Err)
{
    switch (A.K)
    {
    case FArgIR::Self:    S.Self(); return true;
    case FArgIR::NullObj: S.NoObject(); return true;
    case FArgIR::Int:   S.IntConst(A.I); return true;
    case FArgIR::Int64: S.Int64Const(A.I64); return true;
    case FArgIR::Float: S.FloatConst(A.F); return true;
    case FArgIR::Bool:  A.B ? S.True() : S.False(); return true;
    case FArgIR::Name:  S.NameConst(A.S); return true;
    case FArgIR::Text:  S.TextConst(A.S, A.bWide); return true;
    case FArgIR::Str:
        /* EX_StringConst is Latin-1, so anything non-ASCII goes out as UTF-16. */
        if (A.bWide || !IsAscii(A.S)) S.UnicodeStringConst(Utf8To16(A.S));
        else S.StringConst(A.S);
        return true;
    case FArgIR::Field: S.InstanceVariable(A.S, A.Owner); return true;
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
        if (A.Sub->Intrinsic == "__AddrOf__" || A.Sub->Intrinsic == "__NameIndex__")
        {
            /* StructMember with a donor field at Offset_Internal=0 copies ElementSize bytes straight out
               of the argument's own storage: ScreenMessageString.Key (8) / IntPoint.X (4 = ComparisonIndex). */
            const std::string InnerField = A.Sub->Intrinsic == "__AddrOf__" ? "Key" : "X";
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
            const char* ViewField =
                A.Sub->Intrinsic == "__DerefReadI64__"  ? "NameHashes"     :
                A.Sub->Intrinsic == "__DerefReadI32__"  ? "Mapping"        :
                A.Sub->Intrinsic == "__DerefReadF__"    ? "Data"           :
                A.Sub->Intrinsic == "__DerefReadStr__"  ? "AssetScanPaths" :
                A.Sub->Intrinsic == "__DerefReadText__" ? "Data"           :
                                                         "Kilobyte";
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
                    O.StructMember("NameHashes", View,
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
             const std::string& OutDir, std::string* Err);

private:
    bool Collect(std::string* Err);
    bool Generate(const FRecord& R, const std::string& OutDir, std::string* Err);
    bool GenerateStruct(const FRecord& R, const std::string& OutDir, std::string* Err);
    bool TypeToProperty(const std::string& QualType, const std::string& PName, uint64 ExtraFlags,
                        const std::string& Where, FBlueprintClass& BP, FPropertyDef* Out, std::string* Err);
    bool LayoutOf(const std::string& QualType, int32* Size, int32* Align, std::string* Err);
    bool StructLayout(const FRecord& R, int32* Size, int32* Align, std::string* Err);
    bool LowerBody(const Json& Body, FBlueprintClass& BP, std::vector<FStmtIR>& Out,
                   std::vector<FPropertyDef>& Locals, std::string* Err);
    bool LowerCall(const Json& CallExprNode, FBlueprintClass& BP, FCallIR& Out, std::string* Err);
    bool LowerArg(const Json& ArgNode, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    bool LowerArgRaw(const Json& N, const std::string& OuterType, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    bool ConvertArg(EStrKind To, FBlueprintClass& BP, FArgIR& Arg, std::string* Err);
    bool LowerField(const Json& MemberNode, FBlueprintClass& BP, FArgIR& Out, std::string* Err);

    /* Post-pass over the IR that turns every `__Read*__(Addr)` sub-expression into a pair of
       statements hoisted to the enclosing statement level:
           __DerefScratch__.Data = <Addr>
           __DerefTmpN__         = ArrayGetByRef(view.<field>, 0)   [via __DerefRead*__ intrinsic]
       The original sub-expression is replaced with LocalVariable(__DerefTmpN__). One
       __DerefScratch__ FDeref local is added per function on first use; the prologue seeds
       its Num=1 so ArrayGetByRef's bounds check passes. */
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

    FIndex FindEvent(FBlueprintClass& BP, const std::string& FromRecord, const std::string& Method);

    /* Records are keyed by qualified name; Bare holds only leaf names exactly one class claims,
       so an ambiguous bare name fails instead of picking the last class collected. */
    const FRecord* Find(const std::string& CppName) const
    {
        auto It = Records.find(CppName);
        if (It != Records.end()) return &It->second;
        auto B = Bare.find(CppName);
        if (B == Bare.end()) return nullptr;
        It = Records.find(B->second);
        return It == Records.end() ? nullptr : &It->second;
    }

    Json Doc;
    std::vector<FConv> Convs;
    const FConv* FindConv(EStrKind From, EStrKind To) const;
    void ApplyConv(const FConv& C, FBlueprintClass& BP, FArgIR& Arg);
    std::string ModPackage;
    std::map<std::string, FRecord> Records;
    std::map<std::string, std::string> MethodOwner;   // clang decl id -> owning record
    std::map<std::string, std::string> FieldOwner;    // clang decl id -> declaring record
    std::map<std::string, std::string> Bare;          // unambiguous leaf name -> qualified name
    const FRecord* Cur = nullptr;                     // record Generate is working on
    std::set<std::string> CurrentOutParms;            // T& parm names of the function being lowered
    std::string CurrentWco;                           // its WorldContext* parm when it is a static, else empty
    std::vector<FRegistryAsset> RegistryRows;

    /* Per-function state reset in Generate: whether this function needs the FDeref scratch
       local (and its Num=1 prologue) and the counter that names each hoisted temp. */
    bool ReadScratchAdded = false;
    int32 ReadTmpCounter = 0;
};

void EmitStmts(const std::vector<FStmtIR>& Stmts, FScript& S, FIndex SelfExp)
{
    for (const FStmtIR& St : Stmts)
    {
        switch (St.K)
        {
        case FStmtIR::Assign:
        {
            /* Let's PropertyChain owner: the declaring class for a Field, the function for a local. */
            const bool bLocalDest = St.Var.K == FArgIR::Local || St.Var.K == FArgIR::LocalOut;
            const FIndex VarOwner = bLocalDest ? SelfExp : St.Var.Owner;
            S.Let(St.Var.LetOp, St.Var.S, VarOwner,
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
            if (St.Then) EmitStmts(*St.Then, S, SelfExp);
            if (St.Else && !St.Else->empty())
            {
                const int32 EndPatch = S.Jump(0);
                S.PatchJumpTarget(NotPatch, S.MemorySize());
                EmitStmts(*St.Else, S, SelfExp);
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
            const int32 Head = S.MemorySize();
            const int32 ExitPatch = S.JumpIfNot(0,
                [St, SelfExp](FScript& C) { EmitArg(C, St.Cond, SelfExp, nullptr); });
            if (St.Body) EmitStmts(*St.Body, S, SelfExp);
            S.Jump(Head);
            S.PatchJumpTarget(ExitPatch, S.MemorySize());
            break;
        }
        }
    }
}

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
        if (Kind(N) == "VarDecl" && Name(N) == "UeModPackage") FindLiteral(N, ModPackage);
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
        auto Bases = N.find("bases");
        if (Bases != N.end() && !Bases->empty())
            R.Base = (*Bases)[0]["type"].value("qualType", std::string());

        ForEach(N, [&](const Json& C) {
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
            else if (Kind(C) == "VarDecl" && Name(C) == "UeStructMeta")
            {
                R.bIsStruct = true;
            }
            else if (Kind(C) == "CXXMethodDecl" && C.contains("name"))
            {
                /* genueapi's overload without the world context shares the name; the longer one is the UFunction. */
                const Json*& Slot = R.Methods[Name(C)];
                if (!Slot || ParmNames(C).size() > ParmNames(*Slot).size()) Slot = &C;
                MethodOwner[C.value("id", std::string())] = R.CppName;
            }
            else if (Kind(C) == "FieldDecl" && C.contains("name"))
            {
                R.Fields.push_back(&C);
                FieldOwner[C.value("id", std::string())] = R.CppName;
            }
        });
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

    for (auto& It : Records)
    {
        FRecord& R = It.second;
        if (R.UePackage.empty()) continue;
        if (R.UePackage != ModPackage + "/" + R.CppName) continue;
        if (R.UeName != R.CppName + "_C")
        {
            *Err = "UE_CLASS on " + R.CppName + " says \"" + R.UeName
                 + "\", but cooking it here requires \"" + R.CppName + "_C\"";
            return false;
        }
        R.bIsLocal = true;
    }
    return true;
}

FIndex FCompiler::FindEvent(FBlueprintClass& BP, const std::string& FromRecord, const std::string& Method)
{
    for (const FRecord* R = Find(FromRecord); R; R = Find(R->Base))
    {
        if (R->IsNative() && R->Methods.count(Method))
            return BP.EngineFunction(R->UePackage, R->UeName, Method);
        if (R->Base.empty()) break;
    }
    return Null();      // not an override
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

EExprToken LetOpFor(const std::string& QualType)
{
    if (QualType == "bool") return EX_LetBool;
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
    if (Op == "&") return "And_" + Flavour;
    if (Op == "|") return "Or_" + Flavour;
    if (Op == "^") return "Xor_" + Flavour;
    if (Op == "==") return "EqualEqual_" + Flavour;
    if (Op == "!=") return "NotEqual_" + Flavour;
    if (Op == "<") return "Less_" + Flavour;
    if (Op == ">") return "Greater_" + Flavour;
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
    case FArgIR::Self:
    case FArgIR::NullObj: return SK_Object;
    default: return StrKindOf(InnerType);
    }
}

void WrapInCall(FArgIR& Arg, FIndex Fn)
{
    FArgIR Inner = Arg;
    Arg = FArgIR();
    Arg.K = FArgIR::Call;
    Arg.Sub = std::make_shared<FCallIR>();
    Arg.Sub->Fn = Fn;
    Arg.Sub->Args.push_back(Inner);
}

const FConv* FCompiler::FindConv(EStrKind From, EStrKind To) const
{
    for (const FConv& C : Convs)
        if (C.From == From && C.To == To) return &C;
    return nullptr;
}

void FCompiler::ApplyConv(const FConv& C, FBlueprintClass& BP, FArgIR& Arg)
{
    WrapInCall(Arg, BP.EngineFunction(C.Package, C.Class, C.Fn));
    for (const Json& E : C.Extra)
    {
        FArgIR X;
        if (E.is_boolean()) { X.K = FArgIR::Bool; X.B = E.get<bool>(); }
        else                { X.K = FArgIR::Int;  X.I = E.get<int32>(); }
        Arg.Sub->Args.push_back(X);
    }
    Arg.InnerType = TypeNameOf(C.To);
}

/* Converts Arg (of kind KindOfLowered) to the slot's kind: a literal folds, else a Conv_XToY from
   UeApi/Conv.json, else two of them through FString. */
bool FCompiler::ConvertArg(EStrKind To, FBlueprintClass& BP, FArgIR& Arg, std::string* Err)
{
    const EStrKind From = KindOfLowered(Arg, Arg.InnerType);
    if (To == From || From == SK_None || To == SK_None || To == SK_Object) return true;

    if (Arg.K == FArgIR::Str && !Arg.bWide && To == SK_Name) { Arg.K = FArgIR::Name; return true; }
    if (Arg.K == FArgIR::Str && To == SK_Text) { Arg.K = FArgIR::Text; return true; }
    if (Arg.K == FArgIR::Int && To == SK_Int64) { Arg.K = FArgIR::Int64; Arg.I64 = Arg.I; return true; }
    if (Arg.K == FArgIR::Int && To == SK_Float) { Arg.K = FArgIR::Float; Arg.F = float(Arg.I); return true; }
    if (Arg.K == FArgIR::Int && To == SK_Bool)  { Arg.K = FArgIR::Bool;  Arg.B = Arg.I != 0; return true; }

    if (const FConv* Direct = FindConv(From, To)) { ApplyConv(*Direct, BP, Arg); return true; }
    const FConv* In  = FindConv(From, SK_Str);
    const FConv* Out = FindConv(SK_Str, To);
    if (In && Out) { ApplyConv(*In, BP, Arg); ApplyConv(*Out, BP, Arg); return true; }
    *Err = std::string("no Kismet conversion from ") + Arg.InnerType + " to " + TypeNameOf(To);
    return false;
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
        Out.S = Name(MemberNode);
        Out.Owner = BP.ScriptStruct(ModPackage + "/" + R->CppName, R->CppName);
        Out.LetOp = LetOpFor(TypeOf(MemberNode));
        Out.Base = std::make_shared<FArgIR>();
        return LowerArg(*ObjRaw, BP, *Out.Base, Err);
    }

    const Json* Obj = Strip(ObjRaw);
    const std::string ObjKind = Obj ? Kind(*Obj) : "<none>";
    if (ObjKind != "CXXThisExpr")
    {
        *Err = "TODO: a property is only reachable on `this`, not on " + ObjKind;
        return false;
    }
    if (!R->IsNative() && R != Cur)
    {
        *Err = "TODO: a property declared on another generated class is not reachable yet: "
             + Name(MemberNode);
        return false;
    }

    Out.K = FArgIR::Field;
    Out.S = Name(MemberNode);
    Out.Owner = R->IsNative() ? BP.PropertyOwner(R->UePackage, R->UeName) : BP.ClassIndex();
    Out.LetOp = LetOpFor(TypeOf(MemberNode));
    return true;
}

/* The outer (pre-Strip) type is the slot the value lands in; a string-kind mismatch against the
   value's own type becomes a Kismet conversion, so `FName n = Str + Count` just works. */
bool FCompiler::LowerArg(const Json& ArgNode, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    const std::string OuterType = TypeOf(ArgNode);
    const Json* N = Strip(&ArgNode);
    if (!N) { *Err = "empty argument expression"; return false; }
    if (!LowerArgRaw(*N, OuterType, BP, Out, Err)) return false;
    Out.InnerType = TypeOf(*N);
    return ConvertArg(StrKindOf(OuterType), BP, Out, Err);
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
            return LowerArg(*Obj, BP, Out, Err) && ConvertArg(StrKindOf(TypeOf(*N)), BP, Out, Err);
        }
        const Json* Obj = Callee ? Strip(First(*Callee)) : nullptr;
        if (!Obj) { *Err = "member call with no object"; return false; }
        Out.K = FArgIR::Call;
        Out.Sub = std::make_shared<FCallIR>();
        if (!LowerCall(*N, BP, *Out.Sub, Err)) return false;
        if (Kind(*Obj) == "CXXThisExpr") return true;
        Out.Sub->Target = std::make_shared<FArgIR>();
        return LowerArg(*Obj, BP, *Out.Sub->Target, Err);
    }
    if (K == "CXXOperatorCallExpr")
    {
        const Json* Callee = Strip(First(*N));
        const std::string OpName = (Callee && Callee->contains("referencedDecl"))
            ? (*Callee)["referencedDecl"].value("name", std::string()) : std::string();
        const Json* Lhs = Nth(*N, 1);
        const Json* Rhs = Nth(*N, 2);
        if (OpName != "operator+" || !Lhs || !Rhs || StrKindOf(TypeOf(*N)) != SK_Str)
        { *Err = "TODO: unimplemented operator overload " + OpName + " yielding " + TypeOf(*N); return false; }

        /* String `+`: both sides to FString, then Concat_StrStr. */
        Out.K = FArgIR::Call;
        Out.Sub = std::make_shared<FCallIR>();
        Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetStringLibrary", "Concat_StrStr");
        for (const Json* Side : { Lhs, Rhs })
        {
            FArgIR A;
            if (!LowerArg(*Side, BP, A, Err) || !ConvertArg(SK_Str, BP, A, Err)) return false;
            Out.Sub->Args.push_back(A);
        }
        return true;
    }
    if (K == "IntegerLiteral") { Out.K = FArgIR::Int; Out.I = int32(std::stoll(N->value("value", std::string("0")))); return true; }
    if (K == "FloatingLiteral") { Out.K = FArgIR::Float; Out.F = std::stof(N->value("value", std::string("0"))); return true; }
    if (K == "CXXBoolLiteralExpr") { Out.K = FArgIR::Bool; Out.B = N->value("value", false); return true; }
    if (K == "DeclRefExpr")
    {
        /* A T& out-parm needs EX_LocalOutVariable so writes reach the caller's storage. */
        const Json& Ref = (*N)["referencedDecl"];
        const std::string RefKind = Ref.value("kind", std::string());
        if (RefKind != "ParmVarDecl" && RefKind != "VarDecl")
        {
            *Err = "TODO: DeclRefExpr to " + RefKind;
            return false;
        }
        const std::string RefName = Ref.value("name", std::string());
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

        Out.K = FArgIR::Call;
        Out.Sub = std::make_shared<FCallIR>();
        return LowerCall(*N, BP, *Out.Sub, Err);
    }
    if (K == "UnaryOperator")
    {
        const std::string Op = N->value("opcode", std::string());
        if (Op != "!") { *Err = "TODO: unimplemented unary operator " + Op; return false; }
        const Json* Operand = Nth(*N, 0);
        if (!Operand) { *Err = "unary `!` with a missing operand"; return false; }

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
        /* Flavour is read from the UNSTRIPPED sides: clang's promotion cast carries the common type. */
        const std::string LhsTy = TypeOf(*LhsRaw), RhsTy = TypeOf(*RhsRaw);
        std::string Flavour;
        if (IsObjectType(LhsTy) || IsObjectType(RhsTy))      Flavour = "ObjectObject";
        else if (IsInt64Type(LhsTy) || IsInt64Type(RhsTy))   Flavour = "Int64Int64";
        else                                                 Flavour = "IntInt";
        const std::string MathFn = MathFuncFor(Op, Flavour);
        if (MathFn.empty())
        { *Err = "TODO: unimplemented binary operator " + Op + " on " + Flavour; return false; }

        Out.K = FArgIR::Call;
        Out.Sub = std::make_shared<FCallIR>();
        Out.Sub->Fn = BP.EngineFunction("/Script/Engine", "KismetMathLibrary", MathFn);
        Out.Sub->bScript = false;

        FArgIR LA, RA;
        if (!LowerArg(*LhsRaw, BP, LA, Err)) return false;
        if (!LowerArg(*RhsRaw, BP, RA, Err)) return false;
        Out.Sub->Args.push_back(LA);
        Out.Sub->Args.push_back(RA);
        return true;
    }

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
        auto Owner = MethodOwner.find(DeclId);
        if (Owner == MethodOwner.end()) { *Err = "call to an unknown function: " + MethodName; return false; }
        const FRecord* R = Find(Owner->second);
        if (!R) { *Err = "call to a function on an unknown class: " + Owner->second; return false; }

        auto Decl = R->Methods.find(MethodName);
        const bool bStatic = Decl != R->Methods.end() && IsStaticDecl(*Decl->second);
        if (Decl != R->Methods.end()) FullDecl = Decl->second;

        if (!R->IsNative() && !bStatic) Out.VirtualName = MethodName;

        const std::string CalleePackage = R->IsNative() ? R->UePackage
                                                        : ModPackage + "/" + R->CppName;
        const std::string CalleeName    = R->IsNative() ? R->UeName
                                                        : R->CppName + "_C";
        Out.Fn = BP.EngineFunction(CalleePackage, CalleeName, MethodName);
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
    for (size_t I = 0; I < Parms.size(); ++I)
    {
        if (!IsWcoName(Parms[I])) continue;
        FArgIR Wco;
        if (!CurrentWco.empty()) { Wco.K = FArgIR::Local; Wco.S = CurrentWco; }
        if (Out.Args.size() + 1 == Parms.size()) Out.Args.insert(Out.Args.begin() + I, Wco);
        else if (I < Defaulted.size() && Defaulted[I]) Out.Args[I] = Wco;
        break;
    }
    return true;
}

bool FCompiler::LowerBody(const Json& Body, FBlueprintClass& BP, std::vector<FStmtIR>& Out,
                          std::vector<FPropertyDef>& Locals, std::string* Err)
{
    bool bOk = true;
    ForEach(Body, [&](const Json& Raw) {
        if (!bOk) return;
        const Json* S = Strip(&Raw);
        if (!S) return;

        FStmtIR St;
        const std::string K = Kind(*S);
        if (K == "DeclStmt")
        {
            /* clang groups comma-declared vars under one DeclStmt. */
            bool bAny = false;
            ForEach(*S, [&](const Json& D) {
                if (!bOk || Kind(D) != "VarDecl") return;
                bAny = true;
                const std::string VarName = Name(D);
                FPropertyDef PD;
                if (!TypeToProperty(TypeOf(D), VarName, 0, "local " + VarName, BP, &PD, Err))
                { bOk = false; return; }
                PD.PropertyFlags &= ~uint64(CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
                Locals.push_back(PD);

                FStmtIR Ds;
                Ds.K = FStmtIR::Decl;
                Ds.Var.K = FArgIR::Local;
                Ds.Var.S = VarName;
                Ds.Var.LetOp = LetOpFor(TypeOf(D));
                const Json* Init = Strip(First(D));
                /* `FStats S;` carries an implicit argless CXXConstructExpr: no initialiser. */
                if (Init && Kind(*Init) == "CXXConstructExpr" && !First(*Init)) Init = nullptr;
                if (Init)
                {
                    Ds.bHasValue = true;
                    if (!LowerArg(*First(D), BP, Ds.Value, Err)) { bOk = false; return; }
                }
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
        }
        else if (K == "CallExpr")
        {
            bOk = LowerCall(*S, BP, St.Call, Err);
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

            const std::string LK = Kind(*Lhs);
            if (LK == "MemberExpr")
            {
                St.K = FStmtIR::Assign;
                bOk = LowerField(*Lhs, BP, St.Var, Err) && LowerArg(*Rhs, BP, St.Value, Err);
            }
            else if (LK == "DeclRefExpr")
            {
                const Json& Ref = (*Lhs)["referencedDecl"];
                const std::string RefKind = Ref.value("kind", std::string());
                if (RefKind != "ParmVarDecl" && RefKind != "VarDecl")
                { *Err = "TODO: assignment to a DeclRefExpr of kind " + RefKind; bOk = false; return; }
                const std::string RefName = Ref.value("name", std::string());
                const bool bOut = CurrentOutParms.count(RefName) != 0;
                St.K = FStmtIR::Assign;
                St.Var.K = bOut ? FArgIR::LocalOut : FArgIR::Local;
                St.Var.S = RefName;
                St.Var.LetOp = LetOpFor(TypeOf(*Lhs));
                St.bAssignLocal = !bOut;
                St.bAssignOutParm = bOut;
                bOk = LowerArg(*Rhs, BP, St.Value, Err);
            }
            else
            {
                *Err = "TODO: assignment to " + LK + ", not a property or local";
                bOk = false;
                return;
            }
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
                *Err = "TODO: `if` with an init-statement / condition-variable is not supported";
                bOk = false;
                return;
            }
            const Json* Cond = Nth(*S, 0);
            const Json* Then = Nth(*S, 1);
            const Json* Else = Nth(*S, 2);
            if (!Cond || !Then) { *Err = "`if` with a missing condition or then-branch"; bOk = false; return; }

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
        else if (K == "WhileStmt")
        {
            /* WhileStmt inner is [cond, body]. */
            const Json* Cond = Nth(*S, 0);
            const Json* Body = Nth(*S, 1);
            if (!Cond || !Body) { *Err = "`while` with a missing condition or body"; bOk = false; return; }

            St.K = FStmtIR::While;
            if (!LowerArg(*Cond, BP, St.Cond, Err)) { bOk = false; return; }
            St.Body = std::make_shared<std::vector<FStmtIR>>();
            if (Kind(*Body) == "CompoundStmt")
            {
                if (!LowerBody(*Body, BP, *St.Body, Locals, Err)) { bOk = false; return; }
            }
            else
            {
                Json Wrap = { {"kind", "CompoundStmt"}, {"inner", Json::array({*Body})} };
                if (!LowerBody(Wrap, BP, *St.Body, Locals, Err)) { bOk = false; return; }
            }
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

            Json BodyArr = Json::array();
            if (Kind(*Body) == "CompoundStmt")
            {
                auto It = Body->find("inner");
                if (It != Body->end()) for (const Json& C : *It) BodyArr.push_back(C);
            }
            else BodyArr.push_back(*Body);
            if (Inc) BodyArr.push_back(*Inc);

            Json WrapBody = { {"kind", "CompoundStmt"}, {"inner", BodyArr} };
            if (!LowerBody(WrapBody, BP, *St.Body, Locals, Err)) { bOk = false; return; }
        }
        else
        {
            *Err = "TODO: unimplemented statement " + K;
            bOk = false;
            return;
        }
        if (bOk) Out.push_back(St);
    });
    return bOk;
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
    if (A.K == FArgIR::Member && A.Base)
        return HoistReadsInArg(*A.Base, BP, Locals, OutPre, Err);
    if (A.K != FArgIR::Call || !A.Sub) return true;

    /* Post-order: inner reads hoist before the outer. That way the outer's Addr can reference
       an already-materialised inner temp. */
    for (FArgIR& CA : A.Sub->Args)
        if (!HoistReadsInArg(CA, BP, Locals, OutPre, Err)) return false;

    if (const FReadViewSpec* V = FindReadView(A.Sub->Intrinsic))
        return HoistReadCall(A, *V, BP, Locals, OutPre, Err);
    if (A.Sub->Intrinsic == "__RefAt__")
        return HoistRefAt(A, BP, Locals, OutPre, Err);
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

    const FIndex View = BP.ScriptStruct("/Script/Engine", "MaterialCachedParameterEntry");
    A = FArgIR{};
    A.K = FArgIR::Call;
    A.S = Scratch;
    A.Sub = std::make_shared<FCallIR>();
    A.Sub->Intrinsic = "__RefAtInline__";
    A.Sub->Extra = View;
    return true;
}

bool ContainsRead(const FArgIR& A)
{
    if (A.K == FArgIR::Member && A.Base) return ContainsRead(*A.Base);
    if (A.K != FArgIR::Call || !A.Sub) return false;
    if (FindReadView(A.Sub->Intrinsic)) return true;
    for (const FArgIR& CA : A.Sub->Args) if (ContainsRead(CA)) return true;
    return false;
}

bool FCompiler::HoistReadsInStmt(FStmtIR& St, FBlueprintClass& BP,
                                 std::vector<FPropertyDef>& Locals,
                                 std::vector<FStmtIR>& OutPre, std::string* Err)
{
    /* A while cond runs every iteration but our pre-stmts land outside the loop, so a stale
       scratch would serve every check but the first. Force the user to lift it into the body. */
    if (St.K == FStmtIR::While && ContainsRead(St.Cond))
    {
        *Err = "TODO: `__Read*__` in a `while` condition would only be re-primed once; "
               "extract the read into the loop body";
        return false;
    }

    if (St.Then) if (!HoistReadsInList(*St.Then, BP, Locals, Err)) return false;
    if (St.Else) if (!HoistReadsInList(*St.Else, BP, Locals, Err)) return false;
    if (St.Body) if (!HoistReadsInList(*St.Body, BP, Locals, Err)) return false;

    if (!HoistReadsInArg(St.Var,   BP, Locals, OutPre, Err)) return false;
    if (!HoistReadsInArg(St.Value, BP, Locals, OutPre, Err)) return false;
    if (!HoistReadsInArg(St.Cond,  BP, Locals, OutPre, Err)) return false;
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

/* A member initializer becomes PD.Default, which the CDO / struct default instance writes. It must
   be a literal the member's own type can hold (optionally negated), nullptr, or an argless ctor. */
bool LowerDefault(const Json& F, FPropertyDef& PD, std::string* Err)
{
    const Json* Init = Strip(First(F));
    if (!Init) return true;
    std::string K = Kind(*Init);
    const bool bNeg = K == "UnaryOperator" && Init->value("opcode", std::string()) == "-";
    if (bNeg)
    {
        Init = Strip(First(*Init));
        K = Init ? Kind(*Init) : std::string();
    }
    if (!bNeg && (K == "CXXNullPtrLiteralExpr" || (K == "CXXConstructExpr" && !First(*Init)))) return true;

    FDefaultValue& D = PD.Default;
    const std::string& T = PD.Type;
    const bool bNumber = K == "IntegerLiteral" || K == "FloatingLiteral" || K == "CXXBoolLiteralExpr";
    if (bNumber && (T == "IntProperty" || T == "Int64Property" || T == "ByteProperty"
                    || T == "FloatProperty" || T == "BoolProperty"))
    {
        const std::string V = K == "CXXBoolLiteralExpr" ? std::string() : Init->value("value", std::string("0"));
        double Num = K == "CXXBoolLiteralExpr" ? (Init->value("value", false) ? 1.0 : 0.0)
                   : K == "FloatingLiteral"    ? std::strtod(V.c_str(), nullptr)
                                               : double(std::strtoull(V.c_str(), nullptr, 10));
        int64 Int = K == "IntegerLiteral" ? int64(std::strtoull(V.c_str(), nullptr, 10)) : int64(Num);
        if (bNeg) { Num = -Num; Int = -Int; }
        if (T == "FloatProperty") { D.K = Num != 0.0 ? FDefaultValue::Float : FDefaultValue::None; D.F = Num; }
        else if (T == "BoolProperty") { D.K = Num != 0.0 ? FDefaultValue::Bool : FDefaultValue::None; D.I = 1; }
        else { D.K = Int != 0 ? FDefaultValue::Int : FDefaultValue::None; D.I = Int; }
        return true;
    }
    if (!bNeg && K == "StringLiteral" && (T == "StrProperty" || T == "NameProperty" || T == "TextProperty"))
    {
        D.S = Unquote(Init->value("value", std::string()));
        D.K = D.S.empty() ? FDefaultValue::None : FDefaultValue::Str;
        return true;
    }
    *Err = "TODO: an initializer must be a literal of the member's own type: " + Name(F);
    return false;
}

bool FCompiler::TypeToProperty(const std::string& QualType, const std::string& PName, uint64 ExtraFlags,
                               const std::string& Where, FBlueprintClass& BP, FPropertyDef* Out, std::string* Err)
{
    const std::string Type = StripTypeKeywords(QualType);
    if (Type == "float") { *Out = FloatParam(PName, ExtraFlags); return true; }
    if (Type == "int" || Type == "int32") { *Out = IntParam(PName, ExtraFlags); return true; }
    if (Type == "int64" || Type == "long long") { *Out = Int64Param(PName, ExtraFlags); return true; }
    if (Type == "bool") { *Out = BoolParam(PName, ExtraFlags); return true; }
    if (Type == "uint8" || Type == "unsigned char") { *Out = ByteParam(PName, ExtraFlags); return true; }
    if (Type == "char *" || Type == "char*" || Type == "FString")
    { *Out = StringParam(PName, ExtraFlags); return true; }
    if (Type == "FName") { *Out = NameParam(PName, ExtraFlags); return true; }
    if (Type == "FText") { *Out = TextParam(PName, ExtraFlags); return true; }

    /* TArray<T>. Peel the inner type and recurse; the Inner keeps its own ElementSize while
       the outer is sizeof(FScriptArray) = 16. */
    if (Type.compare(0, 7, "TArray<") == 0 && !Type.empty() && Type.back() == '>')
    {
        const std::string InnerQual = Type.substr(7, Type.size() - 8);
        FPropertyDef InnerPD;
        if (!TypeToProperty(InnerQual, PName, 0, Where + " (TArray inner)", BP, &InnerPD, Err))
            return false;
        *Out = ArrayParam(PName, std::move(InnerPD), ExtraFlags);
        return true;
    }

    if (const FRecord* SR = Find(Type); SR && SR->bIsStruct)
    {
        int32 Size = 0, Align = 0;
        if (!StructLayout(*SR, &Size, &Align, Err)) return false;
        *Out = StructParam(PName, BP.ScriptStruct(ModPackage + "/" + SR->CppName, SR->CppName),
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
    if (!PR || PR->bIsStruct) { *Err = "TODO: unimplemented " + Where + ": " + QualType; return false; }
    const FIndex ClassImp = PR->IsNative()
        ? BP.EngineClass(PR->UePackage, PR->UeName)
        : BP.EngineClass(ModPackage + "/" + PR->CppName, PR->CppName + "_C");
    *Out = ObjectParam(PName, ClassImp, ExtraFlags);
    return true;
}

bool FCompiler::LayoutOf(const std::string& QualType, int32* Size, int32* Align, std::string* Err)
{
    const std::string T = StripTypeKeywords(QualType);
    if (T == "float" || T == "int" || T == "int32") { *Size = 4; *Align = 4; return true; }
    if (T == "int64" || T == "long long") { *Size = 8; *Align = 8; return true; }
    if (T == "bool" || T == "uint8" || T == "unsigned char") { *Size = 1; *Align = 1; return true; }
    if (T == "FName") { *Size = 8; *Align = 4; return true; }
    if (T == "FString" || T == "char *" || T == "char*") { *Size = 16; *Align = 8; return true; }
    if (T == "FText") { *Size = 24; *Align = 8; return true; }
    if (T.compare(0, 7, "TArray<") == 0) { *Size = 16; *Align = 8; return true; }
    if (!T.empty() && T.back() == '*') { *Size = 8; *Align = 8; return true; }
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
        if (!TypeToProperty(TypeOf(*F), Name(*F), 0, "member " + Name(*F), BP, &PD, Err)) return false;
        if (!LowerDefault(*F, PD, Err)) return false;
        PD.PropertyFlags = CPF_Edit | CPF_BlueprintVisible;
        BP.AddVariable(PD);
    }

    const uint32 H = StrCrc32(PackageName);
    const uint32 Guid[4] = { ~H, H * 2654435761u, H ^ 0x9E3779B9u, H };
    BP.FinishStruct(Guid);
    if (!P.Save(OutDir + "/" + R.CppName, Err)) return false;
    RegistryRows.push_back({ PackageName, R.CppName, "UserDefinedStruct" });
    printf("  %-14s -> %s.uasset  (struct, %d members)\n", R.CppName.c_str(), R.CppName.c_str(),
           int32(R.Fields.size()));
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

    for (const Json* F : R.Fields)
    {
        const std::string FieldName = Name(*F);
        FPropertyDef PD;
        std::string PErr;
        if (!TypeToProperty(TypeOf(*F), FieldName, 0, "property " + FieldName, BP, &PD, &PErr))
        { *Err = PErr; return false; }
        if (!LowerDefault(*F, PD, Err)) return false;

        /* CPF_Parm would make it part of the call frame; CPF_BlueprintReadOnly would forbid assignment. */
        PD.PropertyFlags = (PD.PropertyFlags & ~uint64(CPF_Parm | CPF_BlueprintReadOnly))
                         | CPF_Edit | CPF_BlueprintVisible | CPF_DisableEditOnInstance;
        BP.AddVariable(PD);
    }

    for (const auto& Entry : R.Methods)
    {
        /* The OOL definition carries body/parms; only the in-class decl carries storageClass. */
        const Json& Decl = *Entry.second;
        auto DefIt = R.MethodDefs.find(Entry.first);
        const Json& M = DefIt != R.MethodDefs.end() ? *DefIt->second : Decl;
        const Json* Body = nullptr;
        ForEach(M, [&](const Json& C) { if (Kind(C) == "CompoundStmt") Body = &C; });
        if (!Body) continue;

        std::vector<FPropertyDef> Params;
        CurrentOutParms.clear();
        CurrentWco.clear();
        bool bOk = true;
        ForEach(M, [&](const Json& C) {
            if (Kind(C) != "ParmVarDecl" || !bOk) return;
            std::string Type = C["type"].value("qualType", std::string());
            const std::string PName = Name(C);
            /* Every T& parm is treated as an out-parm. */
            bool bOutParm = false;
            while (!Type.empty() && (Type.back() == '&' || Type.back() == ' ' || Type.back() == '\t'))
            {
                if (Type.back() == '&') bOutParm = true;
                Type.pop_back();
            }
            const uint64 ExtraFlags = bOutParm ? (CPF_OutParm | CPF_ReferenceParm) : 0;

            FPropertyDef PD;
            std::string PErr;
            if (!TypeToProperty(Type, PName, ExtraFlags,
                                "parameter " + PName + " on " + Entry.first, BP, &PD, &PErr))
            { *Err = PErr; bOk = false; return; }
            Params.push_back(PD);
            if (bOutParm) CurrentOutParms.insert(PName);
            if (IsStaticDecl(Decl) && CurrentWco.empty() && IsWcoName(PName)) CurrentWco = PName;
        });
        if (!bOk) return false;

        /* UE finds the return property by the exact name "ReturnValue". */
        const std::string FnQual = M["type"].value("qualType", std::string());
        std::string RetType;
        {
            const size_t LParen = FnQual.find('(');
            RetType = LParen == std::string::npos ? FnQual : FnQual.substr(0, LParen);
            while (!RetType.empty() && (RetType.back() == ' ' || RetType.back() == '\t'))
                RetType.pop_back();
        }
        if (!RetType.empty() && RetType != "void")
        {
            FPropertyDef PD;
            std::string PErr;
            if (!TypeToProperty(RetType, "ReturnValue", CPF_ReturnParm | CPF_OutParm,
                                "return type on " + Entry.first, BP, &PD, &PErr))
            { *Err = PErr; return false; }
            Params.push_back(PD);
        }

        std::vector<FStmtIR> Stmts;
        std::vector<FPropertyDef> Locals;
        ReadScratchAdded = false;
        ReadTmpCounter = 0;
        if (!LowerBody(*Body, BP, Stmts, Locals, Err))
        {
            *Err = R.CppName + "::" + Entry.first + ": " + *Err;
            return false;
        }
        if (!HoistReadsInList(Stmts, BP, Locals, Err))
        {
            *Err = R.CppName + "::" + Entry.first + ": " + *Err;
            return false;
        }
        /*
        TODO (user-raised 2026-09-16, assetgen optimizer): an optimizer pass over Stmts, here,
        where lowering and the read hoist are done and nothing is emitted yet. UE_PURE marks the
        calls it may rewrite:
        - drop a statement that only calls a pure function, if its arguments make no impure call;
        - evaluate a pure call repeated with the same arguments once, into a temp local, if no
          impure call and no write to an argument sits between the two (StringTest's
          MakeKey(Caption, Count)); a pure getter may read state an impure call changes.
        Nothing enforces BlueprintPure, and the dump marks functions pure that are not: a fresh
        object per call (FSDJsonObject::CreateJSONObject), the wall clock (Now, UtcNow),
        randomness (RandomInteger; RandomIntegerFromStream advances the stream's mutable Seed
        through a const&). The pass treats a list of those as impure, or it merges two different
        values into one and drops draws that move a random sequence.
        The inlining / copy-propagation / constant-folding passes in TODO.md can share this
        slot. Needs an FCallIR::bPure set in LowerCall beside bStatic and, for engine calls,
        genueapi.py emitting UE_PURE: it reads only _classes.hpp, and the flags are in the
        comment above each body in _functions.cpp.
        */
        /* Locals follow ReturnValue in ChildProperties; the engine tells them apart by CPF_Parm. */
        for (const FPropertyDef& L : Locals) Params.push_back(L);

        const bool bEndsWithReturn = !Stmts.empty() && Stmts.back().K == FStmtIR::Return;
        const bool bScratchNeeded = ReadScratchAdded;
        const FIndex DerefStruct = bScratchNeeded
            ? BP.ScriptStruct(ModPackage + "/FDeref", "FDeref")
            : FIndex{};

        /* A static left FUNC_Event would be treated by the loader as an overridable entry point. */
        uint32 Flags = IsStaticDecl(Decl)
            ? uint32(FUNC_Static | FUNC_BlueprintCallable | FUNC_Public | FUNC_Final)
            : FFunctionDef().FunctionFlags;
        /* All 7229 BlueprintPure functions in the DRG dump are BlueprintCallable too. */
        if (IsPureDecl(M)) Flags |= FUNC_BlueprintPure | FUNC_BlueprintCallable;

        BP.AddFunction(Entry.first, FindEvent(BP, R.CppName, Entry.first), Params,
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

    BP.Finish();
    if (!P.Save(OutDir + "/" + R.CppName, Err)) return false;
    RegistryRows.push_back({ PackageName, R.CppName + "_C", "BlueprintGeneratedClass" });
    printf("  %-14s -> %s.uasset  (%s %s)\n", R.CppName.c_str(), R.CppName.c_str(),
           B->IsNative() ? "extends" : "extends BP", R.Base.c_str());
    return true;
}

bool FCompiler::Run(const std::string& SourcePath, const std::string& IncludeDir,
                    const std::string& OutDir, std::string* Err)
{
    const std::string AstPath = OutDir + "/ast.json";
    /* Both the UeApi dir and its parent are include paths, so "FSD.h" and "UeApi/FSD.h" both resolve. */
    const std::string Parent = std::filesystem::path(IncludeDir).parent_path().string();
    const std::string Cmd = "clang++ -std=c++17 -fsyntax-only -Xclang -ast-dump=json"
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

    const std::string ConvText = ReadText(IncludeDir + "/Conv.json");
    const Json ConvDoc = Json::parse(ConvText, nullptr, false);
    if (!ConvDoc.is_array()) { *Err = "missing or invalid " + IncludeDir + "/Conv.json (run genueapi.py)"; return false; }
    for (const Json& Row : ConvDoc)
    {
        FConv C;
        C.From = KindByName(Row.value("from", std::string()));
        C.To = KindByName(Row.value("to", std::string()));
        C.Package = Row.value("package", std::string());
        C.Class = Row.value("class", std::string());
        C.Fn = Row.value("fn", std::string());
        for (const Json& E : Row["extra"]) C.Extra.push_back(E);
        Convs.push_back(C);
    }

    if (!Collect(Err)) return false;

    int32 Generated = 0;
    for (const auto& Entry : Records)
    {
        const FRecord& R = Entry.second;
        if (!R.IsGenerated()) continue;
        if (!(R.bIsStruct ? GenerateStruct(R, OutDir, Err) : Generate(R, OutDir, Err)))
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
    if (Generated == 0) { *Err = "the source declares no UE_STRUCT and no class deriving from a UE class"; return false; }

    /* A cooked package carries no registry data; without the bake the classes are invisible to it. */
    if (!SaveAssetRegistry(RegistryRows, OutDir + "/AssetRegistry.bin", Err)) return false;
    printf("  %-14s -> AssetRegistry.bin  (%d asset%s)\n", "registry",
           int32(RegistryRows.size()), RegistryRows.size() == 1 ? "" : "s");

    remove(AstPath.c_str());        // kept only on failure
    return true;
}
}   // namespace

bool CompileToAssets(const std::string& SourcePath, const std::string& IncludeDir,
                     const std::string& OutDir, std::string* Err)
{
    FCompiler C;
    return C.Run(SourcePath, IncludeDir, OutDir, Err);
}

}   // namespace Uasset
