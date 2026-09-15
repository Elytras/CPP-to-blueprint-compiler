#pragma once
/*
Blueprint.h — the shape a caller actually wants: "a Blueprint class deriving from X".

Everything below this line is the package format; everything above it should read like a
class declaration. FBlueprintClass is that seam. It owns the imports a generated class always
needs, hands out FIndex values for the engine types being referenced, and assembles the six
exports (class, CDO, its functions, and the default scene root) in an order the loader
accepts.

Imports are resolved on demand and deduplicated, so declaring a call to some engine function
pulls in the class, package and function rows without the caller listing them. That is the
half of "no manual property links" this layer can do on its own; resolving a bare type NAME
to its owning /Script package is the SDK-index half, and is not wired in yet.
*/
#include <string>
#include <unordered_map>
#include <vector>

#include "Package.h"
#include "Script.h"

namespace Uasset
{
class FBlueprintClass
{
public:
    /*
    `ParentPackage`/`ParentClass` name what the generated class derives from: either an engine
    class ("/Script/Engine", "Actor") or another Blueprint in a cooked package.
    */
    FBlueprintClass(FPackage& InPkg, std::string InClassName,
                    std::string ParentPackage, std::string ParentClass, bool bParentIsBlueprint);

    /* Imports, memoised: the same engine object asked for twice yields the same row. */
    FIndex PackageImport(const std::string& PackageName);   // "/Script/Engine" or "/Game/Mod/Asset"
    FIndex EngineClass(const std::string& PackageName, const std::string& ClassName);
    FIndex ScriptStruct(const std::string& PackageName, const std::string& StructName);
    FIndex EngineFunction(const std::string& PackageName, const std::string& OwningClass,
                          const std::string& FunctionName);

    /*
    The class declaring a property a function body reads or writes. Same import as EngineClass,
    but it also records a load dependency - which EngineClass must not do, since it is equally
    the route to the parent class and to an ObjectProperty's type, neither of which is a thing
    the bytecode reaches.
    */
    FIndex PropertyOwner(const std::string& PackageName, const std::string& ClassName);

    /*
    A class's default object, as the bytecode sees it. A call to a static function runs against
    the CDO of the class declaring it, and EX_Context needs that object as a literal - so this
    is an import the script names, and therefore a load dependency like any function it calls.
    */
    FIndex ClassDefaultObject(const std::string& PackageName, const std::string& ClassName);

    /*
    Whether this class gets the SimpleConstructionScript / SCS_Node / DefaultSceneRoot trio.
    Every cooked actor Blueprint has it, and nothing that is not an actor may: an SCS fixes up
    its root node through the owning class's CDO cast to AActor, which for (say) a function
    library is not an actor at all.
    */
    void SetIsActor(bool bValue) { bIsActor = bValue; }

    /*
    Declares a function on the class. `Super` should be the engine UFunction being overridden
    for an event like ReceiveTick, or null for a new method.

    Body takes the function's OWN export index so bytecode that names its own params can spell
    the FFieldPath owner ("Ref" belongs to <thisFunction>, not to <thisClass>). The index is
    not known here; Finish() fills it in when the function's export row is fixed.
    */
    void AddFunction(const std::string& Name, FIndex Super,
                     const std::vector<FPropertyDef>& Params,
                     const std::function<void(FScript&, FIndex)>& Body,
                     uint32 FunctionFlags = 0);      // 0 = the event-override default

    /*
    Declares a class variable. It becomes one ChildProperties entry on the class, which is what
    an EX_InstanceVariable FFieldPath owned by this class resolves against. No CDO default is
    written, so the value the instance starts at is the type's zero.
    */
    void AddVariable(const FPropertyDef& Var);

    /* Writes the class, its CDO, its functions and a default scene root into the package. */
    void Finish();

    FIndex ClassIndex() const { return Exp(ClassRow); }

private:
    struct FPending
    {
        FFunctionDef Def;
        std::function<void(FScript&, FIndex)> Body;
    };

    FPackage& P;
    std::string ClassName;
    std::string ParentPackage, ParentClass;
    bool bParentIsBlueprint = false;
    bool bIsActor = true;

    std::unordered_map<std::string, int32> ImportCache;

    /*
    Every import a function body could call, and the classes declaring them.

    The event-driven loader wants each function export to list what its bytecode reaches, as
    create-before-serialize edges. Which function used which import is not tracked - the script
    is assembled inside a closure that runs later - so every function declares the union. Over-
    declaring only forces those objects to exist earlier, which is what a real cooked class does
    anyway; under-declaring is the failure that crashes the async loader.
    */
    std::vector<int32> CallImports;
    std::vector<FPending> Functions;
    std::vector<FPropertyDef> Vars;

    /*
    The class is always export row 0 - Finish() lays the rows out that way, and it is fixed here
    rather than there because ClassIndex() is asked for while a body is being lowered, which is
    long before Finish() runs.
    */
    int32 ClassRow = 0;
};

}   // namespace Uasset
