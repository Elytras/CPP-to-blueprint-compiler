/*
Main.cpp — the AssetGen driver.

Two modes, answering different questions.

`verify` rebuilds a real cooked DRG asset (_ElytrasMods/Autosprint/InitCave) and diffs it
against Epic's own bytes. That is the regression gate on the package format: if the container
ever drifts, this catches it offline, with no game and no editor involved.

`gen` writes the actual deliverable — a Blueprint class authored here in C++, with an event
whose body is real Kismet bytecode, plus the two spawn-hook subclasses DRG's mod loader looks
for by name. Nothing verifies that one but the game, which is rather the point of it.

usage: assetgen verify <out-dir> <reference-dir>
       assetgen gen <out-dir>
*/
#include <cstdio>
#include <string>
#include <vector>

#include "Blueprint.h"
#include "Package.h"
#include "Script.h"

using namespace Uasset;

namespace
{
/* ---- the regression specimen ---- */

void BuildInitCave(FPackage& P, const std::string& ParentPkg, const std::string& ParentClass,
                   const std::string& ClassName)
{
    const std::string CDOName = "Default__" + ClassName;

    const int32 ImpParentCDO = P.AddImport({ ParentPkg, ParentClass, Imp(8), "Default__" + ParentClass });
    const int32 ImpParent = P.AddImport({ "/Script/Engine", "BlueprintGeneratedClass", Imp(8), ParentClass });
    const int32 ImpBPGCCdo = P.AddImport({ "/Script/Engine", "BlueprintGeneratedClass", Imp(10), "Default__BlueprintGeneratedClass" });
    const int32 ImpObject = P.AddImport({ "/Script/CoreUObject", "Class", Imp(9), "Object" });
    const int32 ImpBPGC = P.AddImport({ "/Script/CoreUObject", "Class", Imp(10), "BlueprintGeneratedClass" });
    const int32 ImpSceneComp = P.AddImport({ "/Script/CoreUObject", "Class", Imp(10), "SceneComponent" });
    const int32 ImpScsNode = P.AddImport({ "/Script/CoreUObject", "Class", Imp(10), "SCS_Node" });
    const int32 ImpScs = P.AddImport({ "/Script/CoreUObject", "Class", Imp(10), "SimpleConstructionScript" });
    P.AddImport({ "/Script/CoreUObject", "Package", Null(), ParentPkg });               // 8
    P.AddImport({ "/Script/CoreUObject", "Package", Null(), "/Script/CoreUObject" });   // 9
    P.AddImport({ "/Script/CoreUObject", "Package", Null(), "/Script/Engine" });        // 10
    P.AddImport({ "/Script/Engine", "SceneComponent", Imp(1), "DefaultSceneRoot_GEN_VARIABLE" });
    const int32 ImpSceneCompCdo = P.AddImport({ "/Script/Engine", "SceneComponent", Imp(10), "Default__SceneComponent" });
    const int32 ImpScsNodeCdo = P.AddImport({ "/Script/Engine", "SCS_Node", Imp(10), "Default__SCS_Node" });
    const int32 ImpScsCdo = P.AddImport({ "/Script/Engine", "SimpleConstructionScript", Imp(10), "Default__SimpleConstructionScript" });

    const int32 ExClass = 0, ExCdo = 1, ExRootTemplate = 2, ExScsNode = 3, ExScs = 4;

    FExport Class;
    Class.ClassIndex = Imp(ImpBPGC);
    Class.SuperIndex = Imp(ImpParent);
    Class.TemplateIndex = Imp(ImpBPGCCdo);
    Class.OuterIndex = Null();
    Class.ObjectName = ClassName;
    Class.ObjectFlags = RF_Public | RF_Transactional;
    Class.bIsAsset = true;
    Class.SerBeforeSer = { -2, -1, 5 };
    Class.SerBeforeCreate = { -5, -3 };
    Class.CreateBeforeCreate = { -2 };
    Class.Serialize = [=](FArc& Ar) {
        Tag(Ar, "SimpleConstructionScript", "ObjectProperty", [=](FArc& V) { V.Idx(Exp(ExScs)); });
        TagEnd(Ar);
        Ar.Bool(false);

        Ar.Idx(Imp(ImpParent));
        Ar.Idx(Null());                     // Children
        Ar.I32(0);                          // ChildProperties
        Ar.I32(0);                          // script bytecode size
        Ar.I32(0);                          // script storage size
        Ar.I32(0);                          // FuncMap
        Ar.U32(0x00840814);
        Ar.Idx(Imp(ImpObject));
        Ar.Name("Engine");
        Ar.I32(0);
        Ar.Idx(Null());
        Ar.Bool(false);
        Ar.Name("None");
        Ar.Bool(true);
        Ar.Idx(Exp(ExCdo));
    };
    P.AddExport(std::move(Class));

    FExport Cdo;
    Cdo.ClassIndex = Exp(ExClass);
    Cdo.TemplateIndex = Imp(ImpParentCDO);
    Cdo.ObjectName = CDOName;
    Cdo.ObjectFlags = RF_Public | RF_ClassDefaultObject | RF_ArchetypeObject;
    Cdo.SerBeforeSer = { -12 };
    Cdo.CreateBeforeSer = { -2 };
    Cdo.SerBeforeCreate = { 1, -1 };
    Cdo.Serialize = [](FArc& Ar) { TagEnd(Ar); };
    P.AddExport(std::move(Cdo));

    FExport RootTemplate;
    RootTemplate.ClassIndex = Imp(ImpSceneComp);
    RootTemplate.TemplateIndex = Imp(ImpSceneCompCdo);
    RootTemplate.OuterIndex = Exp(ExClass);
    RootTemplate.ObjectName = "DefaultSceneRoot_GEN_VARIABLE";
    RootTemplate.ObjectFlags = RF_Public | RF_Transactional | RF_ArchetypeObject;
    RootTemplate.SerBeforeSer = { 1 };
    RootTemplate.SerBeforeCreate = { -6, -13 };
    RootTemplate.CreateBeforeCreate = { 1 };
    RootTemplate.Serialize = [](FArc& Ar) { TagEnd(Ar); Ar.Bool(false); };
    P.AddExport(std::move(RootTemplate));

    static const uint8 kRootVariableGuid[16] = {
        0x46, 0x90, 0x8F, 0xEE, 0x0D, 0x0A, 0x89, 0x44, 0x86, 0xF9, 0x68, 0xB3, 0x46, 0xBF, 0x40, 0xA2
    };
    FExport ScsNode;
    ScsNode.ClassIndex = Imp(ImpScsNode);
    ScsNode.TemplateIndex = Imp(ImpScsNodeCdo);
    ScsNode.OuterIndex = Exp(ExScs);
    ScsNode.ObjectName = "SCS_Node_0";
    ScsNode.ObjectFlags = RF_Transactional;
    ScsNode.CreateBeforeSer = { 3, -6 };
    ScsNode.SerBeforeCreate = { -7, -14 };
    ScsNode.CreateBeforeCreate = { 5 };
    ScsNode.Serialize = [=](FArc& Ar) {
        Tag(Ar, "ComponentClass", "ObjectProperty", [=](FArc& V) { V.Idx(Imp(ImpSceneComp)); });
        Tag(Ar, "ComponentTemplate", "ObjectProperty", [=](FArc& V) { V.Idx(Exp(ExRootTemplate)); });
        Tag(Ar, "VariableGuid", "StructProperty", [](FArc& V) { V.Raw(kRootVariableGuid, 16); }, "Guid");
        Tag(Ar, "InternalVariableName", "NameProperty", [](FArc& V) { V.Name("DefaultSceneRoot"); });
        TagEnd(Ar);
        Ar.Bool(false);
    };
    P.AddExport(std::move(ScsNode));

    FExport Scs;
    Scs.ClassIndex = Imp(ImpScs);
    Scs.TemplateIndex = Imp(ImpScsCdo);
    Scs.OuterIndex = Exp(ExClass);
    Scs.ObjectName = "SimpleConstructionScript_0";
    Scs.ObjectFlags = RF_Transactional;
    Scs.CreateBeforeSer = { 4 };
    Scs.SerBeforeCreate = { -8, -15 };
    Scs.CreateBeforeCreate = { 1 };
    Scs.Serialize = [=](FArc& Ar) {
        Tag(Ar, "DefaultSceneRootNode", "ObjectProperty", [=](FArc& V) { V.Idx(Exp(ExScsNode)); });
        TagEnd(Ar);
        Ar.Bool(false);
    };
    P.AddExport(std::move(Scs));
}

std::vector<uint8> ReadFile(const std::string& Path)
{
    std::vector<uint8> Out;
    FILE* F = nullptr;
    if (fopen_s(&F, Path.c_str(), "rb") != 0 || !F) return Out;
    fseek(F, 0, SEEK_END);
    Out.resize(size_t(ftell(F)));
    fseek(F, 0, SEEK_SET);
    if (!Out.empty() && fread(Out.data(), 1, Out.size(), F) != Out.size()) Out.clear();
    fclose(F);
    return Out;
}

bool Diff(const std::string& Label, const std::string& Mine, const std::string& Ref)
{
    const std::vector<uint8> A = ReadFile(Mine), B = ReadFile(Ref);
    if (B.empty()) { printf("  %-8s reference missing: %s\n", Label.c_str(), Ref.c_str()); return false; }

    const size_t N = A.size() < B.size() ? A.size() : B.size();
    for (size_t I = 0; I < N; ++I)
    {
        if (A[I] == B[I]) continue;
        printf("  %-8s DIFFERS at 0x%zx: got %02X, expected %02X  (sizes %zu vs %zu)\n",
               Label.c_str(), I, A[I], B[I], A.size(), B.size());
        return false;
    }
    if (A.size() != B.size())
    {
        printf("  %-8s length differs: got %zu, expected %zu\n", Label.c_str(), A.size(), B.size());
        return false;
    }
    printf("  %-8s identical (%zu bytes)\n", Label.c_str(), A.size());
    return true;
}

/* Gives a generated package a stable identity, with no editor around to allocate one. */
void StampIdentity(FPackage& P, const std::string& PackageName)
{
    const uint32 H = StrCrc32(PackageName);
    P.SetGuid(H, H ^ 0x9E3779B9u, ~H, H * 2654435761u);
    P.SetPackageSource(H);
}

bool Save(FPackage& P, const std::string& Base)
{
    std::string Err;
    if (!P.Save(Base, &Err)) { printf("  FAILED %s: %s\n", Base.c_str(), Err.c_str()); return false; }
    printf("  wrote %s.uasset / .uexp\n", Base.c_str());
    return true;
}

/* ---- the generated mod ---- */

const char* kModPath = "/Game/_ElytrasMods/CppTest";

/*
The class this whole exercise is for:

    class Test : public AActor
    {
        void ReceiveBeginPlay() { PrintString("Hello from a C++ generated Blueprint"); }
    };

ReceiveBeginPlay rather than ReceiveTick only so the greeting arrives once instead of every
frame; swapping the event name and adding a float DeltaSeconds param is the whole difference.
*/
bool GenerateTest(const std::string& OutDir)
{
    const std::string PackageName = std::string(kModPath) + "/Test";
    FPackage P(PackageName);
    StampIdentity(P, PackageName);

    FBlueprintClass BP(P, "Test_C", "/Script/Engine", "Actor", /*bParentIsBlueprint=*/false);

    const FIndex PrintString = BP.EngineFunction("/Script/Engine", "KismetSystemLibrary", "PrintString");
    const FIndex LinearColor = BP.ScriptStruct("/Script/CoreUObject", "LinearColor");
    const FIndex BeginPlay = BP.EngineFunction("/Script/Engine", "Actor", "ReceiveBeginPlay");

    BP.AddFunction("ReceiveBeginPlay", BeginPlay, {}, [=](FScript& S) {
        S.CallMath(PrintString);
        S.Self();                                   // WorldContextObject
        S.StringConst("Hello from a C++ generated Blueprint");
        S.True();                                   // bPrintToScreen
        S.True();                                   // bPrintToLog
        S.StructConst(LinearColor, 16, [](FScript& C) {
            C.FloatConst(0.0f); C.FloatConst(0.66f); C.FloatConst(1.0f); C.FloatConst(1.0f);
        });
        S.FloatConst(8.0f);                         // Duration
        S.EndFunctionParms();
        S.Return();
        S.EndOfScript();
    });

    BP.Finish();
    return Save(P, OutDir + "/Test");
}

/*
DRG's mod loader spawns assets named InitCave and InitSpacerig, so a mod's entry point is the
asset name rather than any registration call. Both simply derive from the class above.
*/
bool GenerateSpawnHook(const std::string& OutDir, const std::string& AssetName)
{
    const std::string PackageName = std::string(kModPath) + "/" + AssetName;
    FPackage P(PackageName);
    StampIdentity(P, PackageName);

    FBlueprintClass BP(P, AssetName + "_C", std::string(kModPath) + "/Test", "Test_C",
                       /*bParentIsBlueprint=*/true);
    BP.Finish();
    return Save(P, OutDir + "/" + AssetName);
}

int Verify(const std::string& OutDir, const std::string& RefDir)
{
    const std::string Base = OutDir + "/InitCave";
    FPackage P("/Game/_ElytrasMods/Autosprint/InitCave");
    P.SetGuid(0xCEC37518, 0x4D6F2623, 0xBBF4428C, 0x6E6EEB19);
    P.SetPackageSource(0xDD9E5085);
    BuildInitCave(P, "/Game/_ElytrasMods/Autosprint/Autosprint", "Autosprint_C", "InitCave_C");

    if (!Save(P, Base)) return 1;

    const std::string RefBase = RefDir + "/InitCave";
    const bool bAsset = Diff("uasset", Base + ".uasset", RefBase + ".uasset");
    const bool bExp = Diff("uexp", Base + ".uexp", RefBase + ".uexp");
    return (bAsset && bExp) ? 0 : 1;
}
}   // namespace

int main(int argc, char** argv)
{
    if (argc >= 4 && std::string(argv[1]) == "verify")
        return Verify(argv[2], argv[3]);

    if (argc >= 3 && std::string(argv[1]) == "gen")
    {
        const std::string OutDir = argv[2];
        const bool bOk = GenerateTest(OutDir)
                       && GenerateSpawnHook(OutDir, "InitCave")
                       && GenerateSpawnHook(OutDir, "InitSpacerig");
        return bOk ? 0 : 1;
    }

    printf("usage: assetgen verify <out-dir> <reference-dir>\n"
           "       assetgen gen <out-dir>\n");
    return 2;
}
