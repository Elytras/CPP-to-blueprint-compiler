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
    Declares a function on the class. `Super` should be the engine UFunction being overridden
    for an event like ReceiveTick, or null for a new method.
    */
    void AddFunction(const std::string& Name, FIndex Super,
                     const std::vector<FPropertyDef>& Params,
                     const std::function<void(FScript&)>& Body);

    /* Writes the class, its CDO, its functions and a default scene root into the package. */
    void Finish();

    FIndex ClassIndex() const { return Exp(ClassRow); }

private:
    struct FPending
    {
        FFunctionDef Def;
        std::function<void(FScript&)> Body;
    };

    FPackage& P;
    std::string ClassName;
    std::string ParentPackage, ParentClass;
    bool bParentIsBlueprint = false;

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

    int32 ClassRow = -1;
};

}   // namespace Uasset
