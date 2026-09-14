/*
Main.cpp — the AssetGen driver, and for now its own test case.

The class it builds is a real cooked DRG asset: `_ElytrasMods/Autosprint/InitCave`, an empty
Blueprint class deriving from another Blueprint (`Autosprint_C`) with nothing but the default
scene root. That is the exact shape of the eventual goal — "a BP class inheriting from X" —
and it is small enough that every byte can be accounted for, so it doubles as the round-trip
gate: run with a reference directory and the tool diffs what it wrote against Epic's output.

Once the round trip holds, this hand-written declaration is what the Blueprint DSL replaces.

usage: assetgen <out-dir> [reference-dir]
*/
#include <cstdio>
#include <string>
#include <vector>

#include "Package.h"

using namespace Uasset;

namespace
{
/*
Builds the InitCave package. `ParentPkg`/`ParentClass` are the Blueprint it derives from; the
import and export rows are declared in Epic's own order so a byte comparison is meaningful
(the loader itself does not care about table order).
*/
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

    // Export rows are referenced by index before they exist, so fix the layout up front.
    const int32 ExClass = 0, ExCdo = 1, ExRootTemplate = 2, ExScsNode = 3, ExScs = 4;

    /*
    The generated class itself. Its Blueprint-specific members (here, SimpleConstructionScript)
    are ordinary reflected properties and so ride in the tagged block; everything after the
    property terminator is UStruct's and UClass's own fixed layout.
    */
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
        Ar.Bool(false);                     // no lazy-object guid for this export

        Ar.Idx(Imp(ImpParent));             // UStruct::SuperStruct
        Ar.Idx(Null());                     // Children
        Ar.I32(0);                          // ChildProperties
        Ar.I32(0);                          // script bytecode size
        Ar.I32(0);                          // script storage size
        Ar.I32(0);                          // UClass::FuncMap
        Ar.U32(0x00840814);                 // ClassFlags
        Ar.Idx(Imp(ImpObject));             // ClassWithin
        Ar.Name("Engine");                  // ClassConfigName
        Ar.I32(0);                          // implemented interfaces
        Ar.Idx(Null());                     // ClassGeneratedBy
        Ar.Bool(false);                     // bDeprecatedForceScriptOrder
        Ar.Name("None");                    // an unused FName the format still reserves
        Ar.Bool(true);                      // bCooked
        Ar.Idx(Exp(ExCdo));                 // ClassDefaultObject
    };
    P.AddExport(std::move(Class));

    /* The class default object. Nothing is overridden, so its property list is empty. */
    FExport Cdo;
    Cdo.ClassIndex = Exp(ExClass);
    Cdo.TemplateIndex = Imp(ImpParentCDO);
    Cdo.ObjectName = CDOName;
    Cdo.ObjectFlags = RF_Public | RF_ClassDefaultObject | RF_ArchetypeObject;
    Cdo.SerBeforeSer = { -12 };
    Cdo.CreateBeforeSer = { -2 };
    Cdo.SerBeforeCreate = { 1, -1 };
    // A class default object skips the trailing lazy-object guid that every other export writes.
    Cdo.Serialize = [](FArc& Ar) { TagEnd(Ar); };
    P.AddExport(std::move(Cdo));

    /* The archetype behind the root component the SCS node spawns. */
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

    /* One construction-script node, wiring the archetype above in as the root component. */
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

/* Reports the first divergence from the reference, which is the only one worth looking at. */
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
}   // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: assetgen <out-dir> [reference-dir]\n");
        return 2;
    }
    const std::string OutDir = argv[1];
    const std::string Base = OutDir + "/InitCave";

    FPackage P("/Game/_ElytrasMods/Autosprint/InitCave");
    P.SetGuid(0xCEC37518, 0x4D6F2623, 0xBBF4428C, 0x6E6EEB19);
    P.SetPackageSource(0xDD9E5085);
    BuildInitCave(P, "/Game/_ElytrasMods/Autosprint/Autosprint", "Autosprint_C", "InitCave_C");

    std::string Err;
    if (!P.Save(Base, &Err))
    {
        printf("save failed: %s\n", Err.c_str());
        return 1;
    }
    printf("wrote %s.uasset / .uexp\n", Base.c_str());

    if (argc < 3) return 0;
    const std::string RefBase = std::string(argv[2]) + "/InitCave";
    const bool bAsset = Diff("uasset", Base + ".uasset", RefBase + ".uasset");
    const bool bExp = Diff("uexp", Base + ".uexp", RefBase + ".uexp");
    return (bAsset && bExp) ? 0 : 1;
}
