#pragma once
/*
S38: a cooked UE4.27 package read into its tables and written back. An export's payload stays the bytes it was cooked
with; an edit replaces one, and Write lays the header and the payloads out again around it.

The layout is the one every package in FSD-WindowsNoEditor.pak has (52,645 surveyed, 2026-09-26): unversioned,
PKG_FilterEditorOnly, one generation, no engine version, and the name map, imports, exports, depends, asset registry
and preload dependencies back to back after the summary, with the payloads in export order in the .uexp. Anything
else is refused with its reason rather than read loosely.
*/
#include <string>
#include <vector>

#include "SharedLib/core/Types.h"

namespace Uasset
{
/* An FName as a package stores it: a name-map row and a number, one-based (0 = no number). */
struct FNameRef
{
    int32 Index = 0;
    int32 Number = 0;
};

struct FCookedName
{
    std::string Text;                           // UTF-8
    uint16 NonCaseHash = 0, CaseHash = 0;       // as read; Write computes its own
};

struct FCookedImport
{
    FNameRef ClassPackage, ClassName;
    int32 Outer = 0;                            // FPackageIndex
    FNameRef ObjectName;
};

struct FCookedExport
{
    int32 Class = 0, Super = 0, Template = 0, Outer = 0;
    FNameRef ObjectName;
    uint32 ObjectFlags = 0;
    bool bForcedExport = false, bNotForClient = false, bNotForServer = false;
    uint32 PackageGuid[4] = { 0, 0, 0, 0 };
    uint32 PackageFlags = 0;
    bool bNotAlwaysLoadedForEditorGame = false, bIsAsset = false;
    /* EDL: where this export's run starts in PreloadDependencies (-1 = none), then the four phase counts in order. */
    int32 FirstExportDependency = -1;
    int32 SerBeforeSer = 0, CreateBeforeSer = 0, SerBeforeCreate = 0, CreateBeforeCreate = 0;
    std::vector<int32> Depends;                 // its DependsMap row: empty in every cooked package seen
    std::vector<uint8> Payload;                 // its bytes in the .uexp
};

class FCookedPackage
{
public:
    /* The header (.uasset or .umap) and the .uexp beside it. */
    bool Load(const std::string& HeaderPath, std::string* Err);
    bool Read(const std::vector<uint8>& Header, const std::vector<uint8>& Exp, std::string* Err);
    void Write(std::vector<uint8>& Header, std::vector<uint8>& Exp) const;
    bool Save(const std::string& HeaderPath, std::string* Err) const;

    /* "Base" or "Base_<n>", as FName::ToString spells a numbered name. */
    std::string NameOf(const FNameRef& N) const;
    /* Whether N is the plain name S (no number), compared as FName compares: case-insensitively. */
    bool Is(const FNameRef& N, const char* S) const;
    /* The name of the class an export or import index names: an import's ObjectName, or an export's own. */
    std::string ClassNameOf(int32 Index) const;

    /* S38 edits. Every index the package already uses stays valid: a name or an import it lacks is appended. */
    /* S as an FName of this package ("Base_<n>" split off as FName does), appended if the table lacks it. */
    FNameRef NameRef(const std::string& S);
    /* Whether N is the FName S spells (the same number, the base compared case-insensitively). */
    bool SameName(const FNameRef& N, const std::string& S) const;
    /* The export row of the object named ObjectName in Outer (an FPackageIndex; 0 = top level), or -1. */
    int32 FindExport(const std::string& ObjectName, int32 Outer = 0) const;
    /* The FPackageIndex of the import (ClassPackage, ClassName, Outer, ObjectName), appended when there is none. */
    int32 Import(const std::string& ClassPackage, const std::string& ClassName, int32 Outer, const std::string& ObjectName);
    /* Dep (an FPackageIndex) created before export row Export is serialized - the edge the cook gives an object that
       export's tags reference. Nothing when Dep is already one of its dependencies. */
    void CreateBeforeSerialize(int32 Export, int32 Dep);

    uint32 PackageFlags = 0;
    uint32 Guid[4] = { 0, 0, 0, 0 };
    uint32 PackageSource = 0;
    std::vector<FCookedName> Names;
    std::vector<FCookedImport> Imports;
    std::vector<FCookedExport> Exports;
    std::vector<int32> PreloadDependencies;
};

/*
One FPropertyTag and its value, the way UStruct::SerializeTaggedProperties writes them (4.27 PropertyTag.cpp). The
value stays raw bytes, and the tag's Size is its length. The type's own fields exist only when Type has no number.
*/
struct FTag
{
    FNameRef Name, Type;
    int32 ArrayIndex = 0;
    FNameRef StructName;                        // StructProperty, with its guid
    uint32 StructGuid[4] = { 0, 0, 0, 0 };
    uint8 BoolVal = 0;                          // BoolProperty: the value itself, with no payload
    FNameRef EnumName;                          // ByteProperty (None for a plain byte), EnumProperty
    FNameRef InnerType, ValueType;              // ArrayProperty and SetProperty: the inner; MapProperty: both
    uint8 HasPropertyGuid = 0;
    uint32 PropertyGuid[4] = { 0, 0, 0, 0 };
    std::vector<uint8> Value;
};

/* A tag list starting at At, through its terminating None; At ends just past it. False if the bytes there are not
   one: an out-of-range name, a size running past the end, or no None before the end. */
bool ReadTags(const FCookedPackage& P, const std::vector<uint8>& Bytes, size_t& At, std::vector<FTag>& Out);
/* The tags and the None that ends them. */
void WriteTags(const FCookedPackage& P, const std::vector<FTag>& Tags, std::vector<uint8>& Out);

/* S38: Tags into the tag list export row Export's payload starts with, each replacing the tag of its name and array
   index or appended after the rest; what follows the list's None stays as it was. False, saying why, when the
   payload does not start with a tag list. */
bool SetTags(FCookedPackage& P, int32 Export, const std::vector<FTag>& Tags, std::string* Err);

/* The gate before any edit is trusted: every cooked package under Dir read and written back in memory, with a report
   of what was refused, what came back different, and whether AssetGen's name hashes and name order match the cook's.
   Each export's tagged properties go through ReadTags / WriteTags on the way. 0 when every package comes back
   byte-identical. */
int RoundTrip(const std::string& Dir);

}   // namespace Uasset
