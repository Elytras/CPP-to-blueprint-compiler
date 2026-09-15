/*
Cpp.cpp — clang's AST, walked into packages.

Two passes per function body, and the split matters. The first resolves every call to an import
and lowers the body into a tiny IR; the second replays that IR as bytecode from inside the
export's serialize closure. They cannot be one pass, because imports have to be added before
the import table is written and the closure runs after — resolving a function name lazily would
append a row to a table that is already on disk.

The IR is also where the subset is enforced. By the time a closure exists, every construct in
the body has already been accepted or rejected by name, so emission cannot fail and there is no
half-written asset to explain.
*/
#include "Cpp.h"

#include <cstdio>
#include <cstdlib>
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

/* Casts, parens and temporaries carry no meaning here; the operand underneath does.

   CXXConstructExpr is stripped too - a Types.h wrapper like FString or FName has an implicit
   constructor from a string literal, so `PostGameMessage("hi")` reaches the emitter as
   CXXConstructExpr(FString, StringLiteral). Only the string literal matters for lowering; the
   constructor is a compile-surface artifact. This does drop later constructor args if any, so
   it is safe only because our wrappers take a single literal. */
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

/* The literal as C++ meant it: clang reports the source spelling, quotes and escapes included. */
std::string Unquote(const std::string& Spelling)
{
    if (Spelling.size() < 2) return Spelling;
    std::string Out;
    for (size_t I = 1; I + 1 < Spelling.size(); ++I)
    {
        if (Spelling[I] != '\\' || I + 2 >= Spelling.size()) { Out.push_back(Spelling[I]); continue; }
        const char C = Spelling[++I];
        switch (C)
        {
        case 'n': Out.push_back('\n'); break;
        case 't': Out.push_back('\t'); break;
        case 'r': Out.push_back('\r'); break;
        case '0': Out.push_back('\0'); break;
        default: Out.push_back(C); break;       // covers \\ , \" and \'
        }
    }
    return Out;
}

/* The first string literal under a node — how UE_CLASS and UE_MOD_PACKAGE reach the generator. */
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

/* Whether a declaration is `static`. clang spells the storage class on the declaration itself. */
bool IsStaticDecl(const Json& Decl) { return Decl.value("storageClass", std::string()) == "static"; }

struct FRecord
{
    std::string CppName;
    std::string UePackage;                          // set by UE_CLASS; empty means "declared here"
    std::string UeName;
    std::string Base;
    std::map<std::string, const Json*> Methods;     // method name -> CXXMethodDecl
    std::vector<const Json*> Fields;                // FieldDecl, in declaration order

    bool IsNative() const { return !UePackage.empty(); }
};

/* ---- the body IR ---- */

struct FCallIR;

struct FArgIR
{
    enum EKind { Self, Int, Float, Bool, Str, Field, Local, Call, NullObj } K = Self;
    int32 I = 0;
    float F = 0.0f;
    bool B = false;
    bool bWide = false;     // Str: true = UCS-2 literal, emit EX_UnicodeStringConst
    std::string S;          // Str: the literal. Field/Local: the property name.
    FIndex Owner;           // Field: the class that declares it (Local owner = function itself, resolved at emit)
    EExprToken LetOp = EX_Let;      // Field: the opcode that writes this type
    std::shared_ptr<FCallIR> Sub;   // Call: the nested call/intrinsic that produces this value
};

struct FCallIR
{
    FIndex Fn;                          // Fn is set for a resolved UFunction; empty for intrinsics
    std::string Intrinsic;              // non-empty when this is an __NAME__ compiler intrinsic
    FIndex Extra;                       // Intrinsic: an auxiliary import (e.g. the donor script struct)
    bool bScript = false;               // the callee is Blueprint bytecode, not a native function
    FIndex Context;                     // the CDO a static call runs against; null = call on self
    std::vector<FArgIR> Args;
};

/*
Writes the call itself - the opcode, not its arguments.

Which opcode is not a preference. EX_CallMath dereferences UFunction::Func and calls it with
the CALLER's frame, which is only correct for a native function; for one implemented in
bytecode Func is UObject::ProcessInternal, which would then run the callee against a frame
that describes the caller. EX_FinalFunction routes through UFunction::Invoke, which builds the
callee its own frame, so that is what a call into another generated class has to use.
*/
void EmitCallOp(FScript& S, const FCallIR& Call)
{
    if (Call.bScript) S.FinalFunction(Call.Fn);
    else S.CallMath(Call.Fn);
}

/* A whole call: the context it runs against, the opcode, its arguments, and the terminator. */
bool EmitCall(FScript& S, const FCallIR& Call, FIndex SelfExp, std::string* Err);

/*
One statement. The shapes a generated event actually needs: reach a subsystem and tell it
something, call something on yourself, or write one of your own properties.
*/
struct FStmtIR
{
    enum EKind
    {
        StaticCall,     // a static library call, standing alone (not `Call`: that is a member)
        TargetCall,     // a call on the object another call produced
        SelfCall,       // a call on `this`
        Assign,         // `this->Field = value`
        Return,         // `return <value>;` - Value carries what to return (or Self kind = void)
    } K = StaticCall;

    FCallIR Target;
    FCallIR Call;
    FArgIR Var;                 // Assign: the destination
    FArgIR Value;                // Assign / Return: what is written or returned
    bool bHasValue = false;      // Return: false = void return (EX_Nothing operand)
};

/*
Emits one argument, or fails with a named error when the argument is a not-yet-supported call.

SelfExp is the enclosing function's own export index - EX_LocalVariable's FFieldPath owner
because a param property lives on its function. Nothing else at emit-time needs it.

Intrinsics (a call whose IR carries an Intrinsic name) are the __NAME__ compiler helpers
declared in Types.h / ReadProperty.cpp - they do not name a UFunction, so calling one goes
through a per-name lowering rather than through a normal EX_CallMath. Only the intrinsics with
a switch arm below are actually emittable; the rest are rejected at Lower time.
*/
bool EmitArgs(FScript& S, const std::vector<FArgIR>& Args, FIndex SelfExp, std::string* Err);

bool EmitArg(FScript& S, const FArgIR& A, FIndex SelfExp, std::string* Err)
{
    switch (A.K)
    {
    case FArgIR::Self:    S.Self(); return true;
    case FArgIR::NullObj: S.NoObject(); return true;
    case FArgIR::Int:   S.IntConst(A.I); return true;
    case FArgIR::Float: S.FloatConst(A.F); return true;
    case FArgIR::Bool:  A.B ? S.True() : S.False(); return true;
    case FArgIR::Str:
        if (A.bWide)
        {
            /*
            Widen the narrow byte stream we captured from the AST: clang reports a wide-string
            literal as its source spelling, so a mod that writes L"hi" arrives here as "hi" +
            bWide=true. Anything above 0x7F is a non-ASCII code point that a proper UTF-8 or
            UTF-16 decode would need to handle - the mod sources this needs today are ASCII
            names/messages, so a byte-by-byte widen is enough.
            */
            std::u16string W;
            W.reserve(A.S.size());
            for (unsigned char C : A.S) W.push_back(char16_t(C));
            S.UnicodeStringConst(W);
        }
        else
        {
            S.StringConst(A.S);
        }
        return true;
    case FArgIR::Field: S.InstanceVariable(A.S, A.Owner); return true;
    case FArgIR::Local: S.LocalVariable(A.S, SelfExp); return true;
    case FArgIR::Call:
        if (!A.Sub) { if (Err) *Err = "internal: Call arg has no sub-call"; return false; }
        if (A.Sub->Intrinsic == "__AddrOf__")
        {
            /*
            Type-confusion read: read the 8 bytes at the argument's storage AS the uint64 Key of
            FScreenMessageString. StructMemberContext copies Member.ElementSize (8) bytes from
            (innerAddress + Member.Offset_Internal). Key's offset is 0, so the copy is straight
            out of the argument's own storage - which for a pointer-typed parameter is the
            pointer value itself, returned as int64.
            */
            if (A.Sub->Args.size() != 1)
            {
                if (Err) *Err = "__AddrOf__ takes exactly one argument";
                return false;
            }
            const FArgIR& Inner = A.Sub->Args[0];
            std::string SubErr;
            bool bInnerOk = true;
            S.StructMember("Key", A.Sub->Extra,
                [&](FScript& Ctx) { bInnerOk = EmitArg(Ctx, Inner, SelfExp, &SubErr); });
            if (!bInnerOk) { if (Err) *Err = SubErr; return false; }
            return true;
        }
        if (!A.Sub->Intrinsic.empty())
        {
            if (Err) *Err = "TODO: unimplemented intrinsic " + A.Sub->Intrinsic;
            return false;
        }
        /*
        A call whose value flows into the surrounding expression. The VM leaves a call's return
        value where the enclosing expression reads its operand from, so nesting one needs no
        temporary - the call expression simply stands where a literal would.
        */
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
    /*
    A static function runs against the CDO of the class that declares it, not against whatever
    object happens to be running the caller - so the call is wrapped in an EX_Context naming
    that CDO. EX_CallMath is the exception: it looks the CDO up itself from the function's own
    outer, which is exactly why a native static library call needs no context at all.
    */
    if (Call.Context.V != 0)
    {
        bool bOk = true;
        std::string SubErr;
        S.Context(
            [&](FScript& O) { O.ObjectConst(Call.Context); },
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

/* Gives a generated package a stable identity, with no editor around to allocate one. */
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

class FCompiler
{
public:
    bool Run(const std::string& SourcePath, const std::string& IncludeDir,
             const std::string& OutDir, std::string* Err);

private:
    bool Collect(std::string* Err);
    bool Generate(const FRecord& R, const std::string& OutDir, std::string* Err);
    bool LowerBody(const Json& Body, FBlueprintClass& BP, std::vector<FStmtIR>& Out, std::string* Err);
    bool LowerCall(const Json& CallExprNode, FBlueprintClass& BP, FCallIR& Out, std::string* Err);
    bool LowerArg(const Json& ArgNode, FBlueprintClass& BP, FArgIR& Out, std::string* Err);
    bool LowerField(const Json& MemberNode, FBlueprintClass& BP, FArgIR& Out, std::string* Err);

    /* The native function behind an override, found by walking the declared ancestry. */
    FIndex FindEvent(FBlueprintClass& BP, const std::string& FromRecord, const std::string& Method);

    const FRecord* Find(const std::string& CppName) const
    {
        auto It = Records.find(CppName);
        return It == Records.end() ? nullptr : &It->second;
    }

    Json Doc;
    std::string ModPackage;
    std::map<std::string, FRecord> Records;
    std::map<std::string, std::string> MethodOwner;   // clang decl id -> owning record
    std::map<std::string, std::string> FieldOwner;    // clang decl id -> declaring record
    const FRecord* Cur = nullptr;                     // the record Generate is working on

    /* One row per generated class, baked into AssetRegistry.bin once the run succeeds. */
    std::vector<FRegistryAsset> RegistryRows;
};

bool FCompiler::Collect(std::string* Err)
{
    ForEach(Doc, [&](const Json& N) {
        if (Kind(N) == "VarDecl" && Name(N) == "UeModPackage") FindLiteral(N, ModPackage);
        if (Kind(N) != "CXXRecordDecl" || !N.contains("name") || !N.contains("inner")) return;

        FRecord R;
        R.CppName = Name(N);
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
                }
            }
            else if (Kind(C) == "CXXMethodDecl" && C.contains("name"))
            {
                R.Methods[Name(C)] = &C;
                MethodOwner[C.value("id", std::string())] = R.CppName;
            }
            else if (Kind(C) == "FieldDecl" && C.contains("name"))
            {
                R.Fields.push_back(&C);
                FieldOwner[C.value("id", std::string())] = R.CppName;
            }
        });
        Records[R.CppName] = R;
    });

    if (ModPackage.empty())
    {
        *Err = "the source declares no UE_MOD_PACKAGE, so its classes have no /Game path";
        return false;
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
    return Null();      // not an override: a new function, which the FuncMap still reaches
}

/*
The opcode that writes a value of this type. See FScript::Let - this is dictated by the
destination, so it is read off the declared type rather than chosen.
*/
EExprToken LetOpFor(const std::string& QualType)
{
    if (QualType == "bool") return EX_LetBool;
    if (!QualType.empty() && QualType.back() == '*') return EX_LetObj;
    return EX_Let;
}

bool FCompiler::LowerField(const Json& MemberNode, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    /*
    Only `this->Field` for now. Reaching a property through another object means EX_Context, whose
    RValuePointer must then name the property being read rather than the null a call writes - a
    different shape, not a longer one.
    */
    const Json* Obj = Strip(First(MemberNode));
    const std::string ObjKind = Obj ? Kind(*Obj) : "<none>";
    if (ObjKind != "CXXThisExpr")
    {
        *Err = "TODO: a property is only reachable on `this`, not on " + ObjKind;
        return false;
    }

    auto It = FieldOwner.find(MemberNode.value("referencedMemberDecl", std::string()));
    if (It == FieldOwner.end()) { *Err = "access to an unknown property: " + Name(MemberNode); return false; }
    const FRecord* R = Find(It->second);
    if (!R) { *Err = "access to a property of an unknown class: " + Name(MemberNode); return false; }
    if (!R->IsNative() && R != Cur)
    {
        /*
        A variable declared on an ANCESTOR generated class. Its FFieldPath owner would be that
        other mod class, which is a Blueprint import rather than an engine one - a different
        import row than PropertyOwner writes, so it is refused rather than mis-spelled.
        */
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

bool FCompiler::LowerArg(const Json& ArgNode, FBlueprintClass& BP, FArgIR& Out, std::string* Err)
{
    const Json* N = Strip(&ArgNode);
    if (!N) { *Err = "empty argument expression"; return false; }

    const std::string K = Kind(*N);
    if (K == "MemberExpr") return LowerField(*N, BP, Out, Err);
    if (K == "CXXThisExpr") { Out.K = FArgIR::Self; return true; }
    if (K == "CXXNullPtrLiteralExpr") { Out.K = FArgIR::NullObj; return true; }
    if (K == "StringLiteral")
    {
        /*
        Wide-string literals arrive as clang's source spelling ("L\"hi\"" for L"hi"), so the
        detection is on the literal's type - const wchar_t [N] or const char16_t [N] - rather
        than on the value bytes. Unquote already strips the leading and trailing quote; the L
        (or u / U) prefix has to go before that.
        */
        Out.K = FArgIR::Str;
        const std::string Ty = TypeOf(*N);
        Out.bWide = Ty.find("wchar_t") != std::string::npos
                 || Ty.find("char16_t") != std::string::npos
                 || Ty.find("char32_t") != std::string::npos;
        std::string V = N->value("value", std::string());
        if (!V.empty() && (V.front() == 'L' || V.front() == 'u' || V.front() == 'U'))
            V.erase(V.begin());     // drop the encoding prefix so Unquote sees a bare "..." literal
        Out.S = Unquote(V);
        return true;
    }
    if (K == "IntegerLiteral") { Out.K = FArgIR::Int; Out.I = int32(std::stoll(N->value("value", std::string("0")))); return true; }
    if (K == "FloatingLiteral") { Out.K = FArgIR::Float; Out.F = std::stof(N->value("value", std::string("0"))); return true; }
    if (K == "CXXBoolLiteralExpr") { Out.K = FArgIR::Bool; Out.B = N->value("value", false); return true; }
    if (K == "DeclRefExpr")
    {
        /*
        A bare name in an expression - typically a reference to a function parameter or a local
        variable. Only parameters are supported so far, and they lower to EX_LocalVariable
        against the enclosing function's own FField chain.
        */
        const Json& Ref = (*N)["referencedDecl"];
        const std::string RefKind = Ref.value("kind", std::string());
        if (RefKind != "ParmVarDecl")
        {
            *Err = "TODO: DeclRefExpr to " + RefKind;
            return false;
        }
        Out.K = FArgIR::Local;
        Out.S = Ref.value("name", std::string());
        return true;
    }
    if (K == "CallExpr")
    {
        /*
        A call in argument position: another expression whose value flows into the outer call.
        LowerCall handles the same intrinsic / UFunction dispatch as it does for statement calls;
        the resulting FCallIR is attached to the FArgIR so the emitter can lower it in place.
        Only intrinsic calls are supported here right now - a nested UFunction call needs the
        emitter to write EX_LocalOutVariable + a call, which is not built yet. Since every
        intrinsic currently fails at LowerCall time (no lowering table), this branch effectively
        just forwards the intrinsic's TODO up to the caller.
        */
        Out.K = FArgIR::Call;
        Out.Sub = std::make_shared<FCallIR>();
        return LowerCall(*N, BP, *Out.Sub, Err);
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

    /*
    Compiler intrinsics are free functions whose name is __NAME__: they do not correspond to a
    UFunction anywhere and their lowering is per-name. Each one resolves its supporting imports
    here (Extra) so the emit closure can stay reference-free; a name with no lowering fails at
    Lower time so the emitter never faces an emission it cannot make.
    */
    const bool bIntrinsic = MethodName.size() >= 5
        && MethodName.compare(0, 2, "__") == 0
        && MethodName.compare(MethodName.size() - 2, 2, "__") == 0;
    if (bIntrinsic)
    {
        Out.Intrinsic = MethodName;
        if (MethodName == "__AddrOf__")
        {
            /*
            The donor struct: any UScriptStruct with an 8-byte scalar at Offset_Internal=0 will
            do. FScreenMessageString.Key (uint64) is in /Script/Engine and always loaded, so it
            costs one import row and no plugin dependency. FDateTime.Ticks would be the natural
            fit but is not reflected in a shipping build - Dumper-7's dump shows the struct as
            an eight-byte pad.
            */
            Out.Extra = BP.ScriptStruct("/Script/Engine", "ScreenMessageString");
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
        if (!R || !R->IsNative())
        {
            *Err = "TODO: unimplemented call into a generated class: " + Owner->second + "::" + MethodName;
            return false;
        }
        Out.Fn = BP.EngineFunction(R->UePackage, R->UeName, MethodName);
        Out.bScript = R->UePackage.compare(0, 6, "/Game/") == 0;   // a Blueprint, not a /Script class
        /*
        EX_CallMath resolves the CDO itself, so a native static needs no context; every other
        static does, and a Blueprint's function is never reached through EX_CallMath.
        */
        auto Decl = R->Methods.find(MethodName);
        const bool bStatic = Decl != R->Methods.end() && IsStaticDecl(*Decl->second);
        if (Out.bScript && bStatic)
            Out.Context = BP.ClassDefaultObject(R->UePackage, R->UeName);
    }

    /* inner[0] is the callee; everything after it is an argument. */
    bool bFirst = true, bOk = true;
    ForEach(CallExprNode, [&](const Json& C) {
        if (bFirst) { bFirst = false; return; }
        if (!bOk) return;
        FArgIR A;
        bOk = LowerArg(C, BP, A, Err);
        if (bOk) Out.Args.push_back(A);
    });
    return bOk;
}

bool FCompiler::LowerBody(const Json& Body, FBlueprintClass& BP, std::vector<FStmtIR>& Out, std::string* Err)
{
    bool bOk = true;
    ForEach(Body, [&](const Json& Raw) {
        if (!bOk) return;
        const Json* S = Strip(&Raw);
        if (!S) return;

        FStmtIR St;
        const std::string K = Kind(*S);
        if (K == "CXXMemberCallExpr")
        {
            /* The object being called on has to be produced by a call of its own. */
            const Json* Member = Strip(First(*S));
            const Json* Object = Member ? Strip(First(*Member)) : nullptr;
            const std::string ObjKind = Object ? Kind(*Object) : "<none>";

            if (ObjKind == "CXXThisExpr")
            {
                /*
                A call on `this` needs no context at all: the VM already has this object, so the
                call is emitted directly. Wrapping it in EX_Context would push a redundant self
                and make the VM skip-count a call that can never be null.
                */
                St.K = FStmtIR::SelfCall;
                bOk = LowerCall(*S, BP, St.Call, Err);
            }
            else if (ObjKind == "CallExpr")
            {
                St.K = FStmtIR::TargetCall;
                bOk = LowerCall(*Object, BP, St.Target, Err) && LowerCall(*S, BP, St.Call, Err);
            }
            else
            {
                *Err = "TODO: unimplemented call target " + ObjKind;
                bOk = false;
                return;
            }
        }
        else if (K == "CallExpr")
        {
            bOk = LowerCall(*S, BP, St.Call, Err);
        }
        else if (K == "BinaryOperator" && S->value("opcode", std::string()) == "=")
        {
            const Json* Lhs = Strip(Nth(*S, 0));
            const Json* Rhs = Strip(Nth(*S, 1));
            if (!Lhs || !Rhs) { *Err = "assignment with a missing side"; bOk = false; return; }
            if (Kind(*Lhs) != "MemberExpr")
            {
                *Err = "TODO: assignment to " + Kind(*Lhs) + ", not a property";
                bOk = false;
                return;
            }
            St.K = FStmtIR::Assign;
            bOk = LowerField(*Lhs, BP, St.Var, Err) && LowerArg(*Rhs, BP, St.Value, Err);
        }
        else if (K == "ReturnStmt")
        {
            /*
            The return value is one expression: `return <expr>;`. The emitter's ReturnValue slot
            takes 8 bytes for now, so what a body can return is scoped to what an argument can
            already lower to - a literal, a `this->Field`, or an intrinsic. An empty return is a
            void return and the emitter writes EX_Return + EX_Nothing.
            */
            St.K = FStmtIR::Return;
            const Json* Val = Strip(First(*S));
            if (Val)
            {
                St.bHasValue = true;
                bOk = LowerArg(*Val, BP, St.Value, Err);
            }
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

bool FCompiler::Generate(const FRecord& R, const std::string& OutDir, std::string* Err)
{
    const FRecord* B = Find(R.Base);
    if (!B) { *Err = R.CppName + " derives from an undeclared class: " + R.Base; return false; }
    Cur = &R;

    const std::string PackageName = ModPackage + "/" + R.CppName;
    FPackage P(PackageName);
    StampIdentity(P, PackageName);

    const bool bParentIsBlueprint = !B->IsNative();
    FBlueprintClass BP(P, R.CppName + "_C",
                       bParentIsBlueprint ? ModPackage + "/" + B->CppName : B->UePackage,
                       bParentIsBlueprint ? B->CppName + "_C" : B->UeName,
                       bParentIsBlueprint);

    /*
    Only an actor gets the SimpleConstructionScript trio. The ancestry is walked in the mod's own
    declarations rather than asked of the engine: the UeApi headers a mod includes declare each
    native class with its base, so the chain from a mod class up to Actor is all in Records.
    */
    bool bIsActor = false;
    for (const FRecord* A = &R; A && !A->Base.empty(); A = Find(A->Base))
        if (A->UeName == "Actor") { bIsActor = true; break; }
    BP.SetIsActor(bIsActor);

    /*
    Turn a mod-source qualType into an FPropertyDef. `Where` names the location for the
    error message ("parameter X on Y", "return type on Y", "property X"); ExtraFlags is
    CPF_ReturnParm | CPF_OutParm for a ReturnValue and zero for an ordinary param or variable.
    */
    auto TypeToProperty = [&](const std::string& Type, const std::string& PName,
                              uint64 ExtraFlags, const std::string& Where,
                              FPropertyDef* Out, std::string* PErr) -> bool
    {
        if (Type == "float") { *Out = FloatParam(PName, ExtraFlags); return true; }
        if (Type == "int" || Type == "int32") { *Out = IntParam(PName, ExtraFlags); return true; }
        if (Type == "int64" || Type == "long long") { *Out = Int64Param(PName, ExtraFlags); return true; }
        if (Type == "bool") { *Out = BoolParam(PName, ExtraFlags); return true; }
        if (Type == "const char *" || Type == "const char*"
            || Type == "FString" || Type == "struct FString")
        { *Out = StringParam(PName, ExtraFlags); return true; }
        if (Type == "FName" || Type == "struct FName")
        { *Out = NameParam(PName, ExtraFlags); return true; }

        /* Object pointer, spelled `[const] class X *`. Resolve X against Records. */
        std::string ClassName;
        {
            const size_t Star = Type.find('*');
            const std::string Head = Star == std::string::npos ? Type : Type.substr(0, Star);
            size_t I = 0;
            while (I < Head.size() && Head[I] == ' ') ++I;
            if (Head.compare(I, 6, "const ") == 0) I += 6;
            if (Head.compare(I, 6, "class ") == 0) I += 6;
            size_t J = Head.size();
            while (J > I && (Head[J - 1] == ' ' || Head[J - 1] == '\t')) --J;
            if (Star != std::string::npos && J > I) ClassName = Head.substr(I, J - I);
        }
        const FRecord* PR = ClassName.empty() ? nullptr : Find(ClassName);
        if (!PR) { *PErr = "TODO: unimplemented " + Where + ": " + Type; return false; }
        const FIndex ClassImp = PR->IsNative()
            ? BP.EngineClass(PR->UePackage, PR->UeName)
            : BP.EngineClass(ModPackage + "/" + PR->CppName, PR->CppName + "_C");
        *Out = ObjectParam(PName, ClassImp, ExtraFlags);
        return true;
    };

    /*
    The class variables, in declaration order. A field with an initializer is accepted only when
    that initializer is the type's zero: the CDO writes no defaults, so anything else would be
    silently dropped rather than honoured.
    */
    for (const Json* F : R.Fields)
    {
        const std::string FieldName = Name(*F);
        if (const Json* Init = Strip(First(*F)))
        {
            const std::string IK = Kind(*Init);
            const std::string IV = Init->value("value", std::string());
            const bool bZero = (IK == "IntegerLiteral" && IV == "0")
                            || (IK == "FloatingLiteral" && (IV == "0" || IV == "0.0"))
                            || (IK == "CXXBoolLiteralExpr" && !Init->value("value", false))
                            || (IK == "StringLiteral" && Unquote(IV).empty());
            if (!bZero)
            {
                *Err = "TODO: a class variable's initializer is not written to the CDO yet: "
                     + FieldName;
                return false;
            }
        }

        FPropertyDef PD;
        std::string PErr;
        if (!TypeToProperty(TypeOf(*F), FieldName, 0, "property " + FieldName, &PD, &PErr))
        { *Err = PErr; return false; }

        /*
        A parm's flags are the wrong ones for a variable: CPF_Parm makes the engine count it as
        part of the call frame, and CPF_BlueprintReadOnly forbids the very assignment the field
        exists for. What is left is a plain, script-writable class variable.
        */
        PD.PropertyFlags = (PD.PropertyFlags & ~uint64(CPF_Parm | CPF_BlueprintReadOnly))
                         | CPF_Edit | CPF_BlueprintVisible | CPF_DisableEditOnInstance;
        BP.AddVariable(PD);
    }

    for (const auto& Entry : R.Methods)
    {
        const Json& M = *Entry.second;
        const Json* Body = nullptr;
        ForEach(M, [&](const Json& C) { if (Kind(C) == "CompoundStmt") Body = &C; });
        if (!Body) continue;                    // a declaration without a definition defines nothing

        std::vector<FPropertyDef> Params;
        bool bOk = true;
        ForEach(M, [&](const Json& C) {
            if (Kind(C) != "ParmVarDecl" || !bOk) return;
            const std::string Type = C["type"].value("qualType", std::string());
            const std::string PName = Name(C);
            FPropertyDef PD;
            std::string PErr;
            if (!TypeToProperty(Type, PName, 0, "parameter " + PName + " on " + Entry.first,
                                &PD, &PErr))
            { *Err = PErr; bOk = false; return; }
            Params.push_back(PD);
        });
        if (!bOk) return false;

        /*
        A non-void return grows the parm chain by one ReturnValue property. UE walks the chain
        looking for that exact name, so the emitter's Return-with-value has somewhere to write.
        The type comes from the method signature's return part - "int64 (class UObject *)"
        splits at the '(' and trims.
        */
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
                                "return type on " + Entry.first, &PD, &PErr))
            { *Err = PErr; return false; }
            Params.push_back(PD);
        }

        std::vector<FStmtIR> Stmts;
        if (!LowerBody(*Body, BP, Stmts, Err))
        {
            *Err = R.CppName + "::" + Entry.first + ": " + *Err;
            return false;
        }

        const bool bEndsWithReturn = !Stmts.empty() && Stmts.back().K == FStmtIR::Return;

        /*
        A static method is not an event: the engine never dispatches it, script calls it by name.
        Leaving it FUNC_Event would have the loader treat a library function as an overridable
        entry point on a class that has no such entry point.
        */
        const uint32 Flags = IsStaticDecl(M)
            ? uint32(FUNC_Static | FUNC_BlueprintCallable | FUNC_Public | FUNC_Final)
            : 0u;

        BP.AddFunction(Entry.first, FindEvent(BP, R.CppName, Entry.first), Params,
                       [Stmts, bEndsWithReturn](FScript& S, FIndex SelfExp) {
            for (const FStmtIR& St : Stmts)
            {
                switch (St.K)
                {
                case FStmtIR::TargetCall:
                    S.Context(
                        [St, SelfExp](FScript& O) { EmitCall(O, St.Target, SelfExp, nullptr); },
                        [St, SelfExp](FScript& C) { C.FinalFunction(St.Call.Fn); EmitArgs(C, St.Call.Args, SelfExp, nullptr); C.EndFunctionParms(); });
                    break;

                case FStmtIR::SelfCall:
                    S.FinalFunction(St.Call.Fn);
                    EmitArgs(S, St.Call.Args, SelfExp, nullptr);
                    S.EndFunctionParms();
                    break;


                case FStmtIR::Assign:
                    S.Let(St.Var.LetOp, St.Var.S, St.Var.Owner,
                          [St, SelfExp](FScript& V) { EmitArgs(V, { St.Var }, SelfExp, nullptr); },
                          [St, SelfExp](FScript& V) { EmitArgs(V, { St.Value }, SelfExp, nullptr); });
                    break;

                case FStmtIR::StaticCall:
                    EmitCall(S, St.Call, SelfExp, nullptr);   // a static library call stands alone
                    break;

                case FStmtIR::Return:
                    /*
                    An explicit return. Void form (no value expression) uses the same
                    EX_Return + EX_Nothing pairing an implicit tail would; a value return runs
                    the source expression inside the Return op so the VM writes into the
                    ReturnValue slot straight away.
                    */
                    if (St.bHasValue)
                        S.Return([St, SelfExp](FScript& V) { EmitArg(V, St.Value, SelfExp, nullptr); });
                    else
                        S.Return();
                    break;
                }
            }
            /*
            Every function must terminate with EX_Return. When the source did not spell one out,
            add the void-return tail; when it did, its Return is already in place and the trailing
            EndOfScript closes the stream.
            */
            if (!bEndsWithReturn) S.Return();
            S.EndOfScript();
        }, Flags);
    }

    BP.Finish();
    if (!P.Save(OutDir + "/" + R.CppName, Err)) return false;
    RegistryRows.push_back({ PackageName, R.CppName + "_C", "BlueprintGeneratedClass" });
    printf("  %-14s -> %s.uasset  (%s %s)\n", R.CppName.c_str(), R.CppName.c_str(),
           bParentIsBlueprint ? "extends BP" : "extends native", R.Base.c_str());
    return true;
}

bool FCompiler::Run(const std::string& SourcePath, const std::string& IncludeDir,
                    const std::string& OutDir, std::string* Err)
{
    /*
    clang is the validator, not just the parser: if this command fails the mod source was not
    valid C++, and its diagnostics are a far better error message than anything we could invent.
    */
    const std::string AstPath = OutDir + "/ast.json";
    /*
    Both the UeApi directory and its parent are on the include path, so a mod may spell its
    include either way ("FSD.h" or "UeApi/FSD.h"). A quoted include otherwise resolves relative
    to the mod source, which silently works for a file sitting in BpMods and fails for one
    anywhere else.
    */
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

    if (!Collect(Err)) return false;

    int32 Generated = 0;
    for (const auto& Entry : Records)
    {
        const FRecord& R = Entry.second;
        if (R.IsNative() || R.Base.empty()) continue;      // declarations, not definitions of a mod class
        if (!Generate(R, OutDir, Err))
        {
            /*
            A half-written mod is worse than none: the assets already saved would ship against a
            class that was never regenerated. The AST stays behind, since that is what a failure
            wants looked at.
            */
            for (const auto& Other : Records)
                if (!Other.second.IsNative() && !Other.second.Base.empty())
                    for (const char* Ext : { ".uasset", ".uexp" })
                        remove((OutDir + "/" + Other.first + Ext).c_str());
            return false;
        }
        ++Generated;
    }
    if (Generated == 0) { *Err = "the source declares no class deriving from a UE class"; return false; }

    /*
    The registry bake. A cooked package carries no asset-registry data of its own, so without
    this the generated classes are invisible to the registry until something scans their path
    for them - which today is the mod DLL, the dependency the pak is meant to shed.
    */
    if (!SaveAssetRegistry(RegistryRows, OutDir + "/AssetRegistry.bin", Err)) return false;
    printf("  %-14s -> AssetRegistry.bin  (%d asset%s)\n", "registry",
           int32(RegistryRows.size()), RegistryRows.size() == 1 ? "" : "s");

    remove(AstPath.c_str());        // kept only on failure; otherwise it would land in the pak
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
