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
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Blueprint.h"
#include "Package.h"
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

/* Casts, parens and temporaries carry no meaning here; the operand underneath does. */
const Json* Strip(const Json* N)
{
    while (N)
    {
        const std::string K = Kind(*N);
        if (K != "ImplicitCastExpr" && K != "CStyleCastExpr" && K != "ParenExpr"
            && K != "ConstantExpr" && K != "ExprWithCleanups"
            && K != "CXXBindTemporaryExpr" && K != "MaterializeTemporaryExpr")
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

struct FRecord
{
    std::string CppName;
    std::string UePackage;                          // set by UE_CLASS; empty means "declared here"
    std::string UeName;
    std::string Base;
    std::map<std::string, const Json*> Methods;     // method name -> CXXMethodDecl

    bool IsNative() const { return !UePackage.empty(); }
};

/* ---- the body IR ---- */

struct FArgIR
{
    enum EKind { Self, Int, Float, Bool, Str } K = Self;
    int32 I = 0;
    float F = 0.0f;
    bool B = false;
    std::string S;
};

struct FCallIR
{
    FIndex Fn;
    std::vector<FArgIR> Args;
};

/*
One statement: a static library call, optionally acting as the target of a member call. That
pair covers the shape a generated event actually needs — reach a subsystem, tell it something.
*/
struct FStmtIR
{
    bool bHasTarget = false;    // the call is made on an object another call produced
    bool bSelfCall = false;     // the call is made on `this`
    FCallIR Target;
    FCallIR Call;
};

void EmitArgs(FScript& S, const std::vector<FArgIR>& Args)
{
    for (const FArgIR& A : Args)
    {
        switch (A.K)
        {
        case FArgIR::Self: S.Self(); break;
        case FArgIR::Int: S.IntConst(A.I); break;
        case FArgIR::Float: S.FloatConst(A.F); break;
        case FArgIR::Bool: A.B ? S.True() : S.False(); break;
        case FArgIR::Str: S.StringConst(A.S); break;
        }
    }
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
    bool LowerArg(const Json& ArgNode, FArgIR& Out, std::string* Err);

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

bool FCompiler::LowerArg(const Json& ArgNode, FArgIR& Out, std::string* Err)
{
    const Json* N = Strip(&ArgNode);
    if (!N) { *Err = "empty argument expression"; return false; }

    const std::string K = Kind(*N);
    if (K == "CXXThisExpr") { Out.K = FArgIR::Self; return true; }
    if (K == "StringLiteral") { Out.K = FArgIR::Str; Out.S = Unquote(N->value("value", std::string())); return true; }
    if (K == "IntegerLiteral") { Out.K = FArgIR::Int; Out.I = int32(std::stoll(N->value("value", std::string("0")))); return true; }
    if (K == "FloatingLiteral") { Out.K = FArgIR::Float; Out.F = std::stof(N->value("value", std::string("0"))); return true; }
    if (K == "CXXBoolLiteralExpr") { Out.K = FArgIR::Bool; Out.B = N->value("value", false); return true; }

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

    auto Owner = MethodOwner.find(DeclId);
    if (Owner == MethodOwner.end()) { *Err = "call to an unknown function: " + MethodName; return false; }
    const FRecord* R = Find(Owner->second);
    if (!R || !R->IsNative())
    {
        *Err = "TODO: unimplemented call into a generated class: " + Owner->second + "::" + MethodName;
        return false;
    }
    Out.Fn = BP.EngineFunction(R->UePackage, R->UeName, MethodName);

    /* inner[0] is the callee; everything after it is an argument. */
    bool bFirst = true, bOk = true;
    ForEach(CallExprNode, [&](const Json& C) {
        if (bFirst) { bFirst = false; return; }
        if (!bOk) return;
        FArgIR A;
        bOk = LowerArg(C, A, Err);
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
                St.bSelfCall = true;
                bOk = LowerCall(*S, BP, St.Call, Err);
            }
            else if (ObjKind == "CallExpr")
            {
                St.bHasTarget = true;
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

    const std::string PackageName = ModPackage + "/" + R.CppName;
    FPackage P(PackageName);
    StampIdentity(P, PackageName);

    const bool bParentIsBlueprint = !B->IsNative();
    FBlueprintClass BP(P, R.CppName + "_C",
                       bParentIsBlueprint ? ModPackage + "/" + B->CppName : B->UePackage,
                       bParentIsBlueprint ? B->CppName + "_C" : B->UeName,
                       bParentIsBlueprint);

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
            if (Type == "float") Params.push_back(FloatParam(PName));
            else if (Type == "int") Params.push_back(IntParam(PName));
            else if (Type == "bool") Params.push_back(BoolParam(PName));
            else { *Err = "TODO: unimplemented parameter type " + Type + " on " + Entry.first; bOk = false; }
        });
        if (!bOk) return false;

        std::vector<FStmtIR> Stmts;
        if (!LowerBody(*Body, BP, Stmts, Err))
        {
            *Err = R.CppName + "::" + Entry.first + ": " + *Err;
            return false;
        }

        BP.AddFunction(Entry.first, FindEvent(BP, R.CppName, Entry.first), Params, [Stmts](FScript& S) {
            for (const FStmtIR& St : Stmts)
            {
                if (St.bHasTarget)
                {
                    S.Context(
                        [St](FScript& O) { O.CallMath(St.Target.Fn); EmitArgs(O, St.Target.Args); O.EndFunctionParms(); },
                        [St](FScript& C) { C.FinalFunction(St.Call.Fn); EmitArgs(C, St.Call.Args); C.EndFunctionParms(); });
                }
                else if (St.bSelfCall)
                {
                    S.FinalFunction(St.Call.Fn);
                    EmitArgs(S, St.Call.Args);
                    S.EndFunctionParms();
                }
                else
                {
                    S.CallMath(St.Call.Fn);     // a static library call stands alone
                    EmitArgs(S, St.Call.Args);
                    S.EndFunctionParms();
                }
            }
            S.Return();
            S.EndOfScript();
        });
    }

    BP.Finish();
    if (!P.Save(OutDir + "/" + R.CppName, Err)) return false;
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
