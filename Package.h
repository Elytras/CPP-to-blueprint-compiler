#pragma once
/*
Package.h — a writer for cooked UE4 packages (.uasset + .uexp), with no engine and no editor.

This is the bottom layer of the C++-to-asset pipeline: a faithful assembler for the on-disk
package format, not yet a Blueprint DSL. A caller declares the name/import/export tables by
hand and supplies, per export, a closure that serializes that object's bytes; `FPackage::Save`
lays out the header, resolves FName indices and writes the two files. The Blueprint-shaped
sugar (`BlueprintClass("BP_ModLoader") : AActor`) is built on top of this, later.

Everything here is transcribed from UE 4.27's own serializers — PackageFileSummary.cpp,
UnrealNames.cpp, Class.cpp, Obj.cpp — and verified byte-for-byte against real cooked DRG
assets. The format is unversioned (`FileVersionUE4 = 0`, no custom versions), uses tagged
property serialization (NOT PKG_UnversionedProperties), and sets PKG_FilterEditorOnly, which
removes LocalizationId and PersistentGuid from the summary.
*/
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "SharedLib/core/Types.h"

namespace Uasset
{
/*
FPackageIndex — the engine's one-based, sign-encoded reference into the import/export tables:
0 is null, +n means Exports[n-1] and -n means Imports[n-1]. Construct via Exp()/Imp() rather
than by hand; the off-by-one is the single most common way to corrupt a package.
*/
struct FIndex
{
    int32 V = 0;
};
inline FIndex Null() { return FIndex{ 0 }; }
inline FIndex Exp(int32 ExportIdx) { return FIndex{ ExportIdx + 1 }; }
inline FIndex Imp(int32 ImportIdx) { return FIndex{ -ImportIdx - 1 }; }

class FPackage;

/*
FArc — a little-endian byte sink with the handful of FArchive spellings a package needs.

The one non-obvious rule: `Bool` writes four bytes, because FArchive serializes bool as int32.
`Name` writes an FName as (name-table index, number) and registers the string with the owning
package, which is what lets the caller's serialize closures run before the name table exists.
*/
class FArc
{
public:
    explicit FArc(FPackage* InPkg) : Pkg(InPkg) {}

    void U8(uint8 V) { B.push_back(V); }
    void U16(uint16 V) { Raw(&V, 2); }
    void U32(uint32 V) { Raw(&V, 4); }
    void I32(int32 V) { Raw(&V, 4); }
    void I64(int64 V) { Raw(&V, 8); }
    void Bool(bool V) { I32(V ? 1 : 0); }
    void Idx(FIndex V) { I32(V.V); }
    void Raw(const void* P, size_t N);

    void Str(const std::string& S);                            // FString: length incl. null, then ANSI
    void Name(const std::string& S, int32 Number = 0);         // FName: table index + number
    void Guid(const uint32 (&G)[4]);
    void Append(const FArc& Other) { Raw(Other.B.data(), Other.B.size()); }

    FPackage* Owner() const { return Pkg; }
    std::vector<uint8> B;

private:
    FPackage* Pkg = nullptr;
};

/*
Tag — writes one FPropertyTag and its value, the way SerializeTaggedProperties does.

`Value` is a closure so the tag's Size field can be measured rather than declared: the value is
serialized into a scratch archive first, then spliced in behind its own length. `StructName` is
only consulted for StructProperty tags, where the tag carries the struct's FName and guid.
*/
void Tag(FArc& Ar, const std::string& Name, const std::string& Type,
         const std::function<void(FArc&)>& Value, const std::string& StructName = "");

/* Terminates a tagged-property block. Every object's property list ends with the name "None". */
inline void TagEnd(FArc& Ar) { Ar.Name("None"); }

struct FImport
{
    std::string ClassPackage;   // e.g. "/Script/Engine"
    std::string ClassName;      // e.g. "BlueprintGeneratedClass"
    FIndex Outer;               // usually the package import this object lives in
    std::string ObjectName;
};

/*
FExport — one object in the package: its table row plus the bytes it serializes to.

`Serialize` is called twice by Save: once to discover which FNames the payload references (the
name table must be complete and sorted before any index is handed out) and once for real. It
must therefore be deterministic and free of side effects.

The four EDL lists are the event-driven-loader preload dependencies, as raw FPackageIndex
values. They are declared explicitly for now; deriving them from the payload's own references
is a later refinement.
*/
struct FExport
{
    FIndex ClassIndex, SuperIndex, TemplateIndex, OuterIndex;
    std::string ObjectName;
    uint32 ObjectFlags = 0;
    bool bIsAsset = false;
    std::function<void(FArc&)> Serialize;

    std::vector<int32> SerBeforeSer, CreateBeforeSer, SerBeforeCreate, CreateBeforeCreate;
};

/* Object flags, as they appear in an export row (Obj.h EObjectFlags). */
enum : uint32
{
    RF_Public = 0x00000001,
    RF_Standalone = 0x00000002,
    RF_Transactional = 0x00000008,
    RF_ClassDefaultObject = 0x00000010,
    RF_ArchetypeObject = 0x00000020,
    RF_DefaultSubObject = 0x00080000,
};

class FPackage
{
public:
    explicit FPackage(std::string InPackageName) : PackageName(std::move(InPackageName)) {}

    int32 AddImport(const FImport& In);        // returns the import's table row
    int32 AddExport(FExport&& In);             // returns the export's table row
    FIndex ImportIndex(int32 Row) const { return Imp(Row); }

    /* Registers a name during pass one, resolves it to a table index during pass two. */
    int32 NameIndex(const std::string& S);

    /*
    Package identity. The guid and source hash are cosmetic for loading but are part of a
    byte-exact comparison against a reference asset, so they are settable.
    */
    void SetGuid(uint32 A, uint32 B, uint32 C, uint32 D) { PkgGuid[0] = A; PkgGuid[1] = B; PkgGuid[2] = C; PkgGuid[3] = D; }
    void SetPackageSource(uint32 S) { PackageSource = S; }

    /* Writes <OutBaseNoExt>.uasset and .uexp. Returns false and fills Err on failure. */
    bool Save(const std::string& OutBaseNoExt, std::string* Err) const;

private:
    friend class FArc;

    std::string PackageName;
    std::vector<FImport> Imports;
    std::vector<FExport> Exports;

    mutable std::vector<std::string> Names;
    mutable std::unordered_map<std::string, int32> NameLookup;   // lowercased -> row
    mutable bool bNamesFinal = false;

    uint32 PkgGuid[4] = { 0, 0, 0, 0 };
    uint32 PackageSource = 0;
};

/*
SplitName — the FName number convention: "SCS_Node_0" is stored as the name "SCS_Node" with
number 1, not as a distinct string. Splits on a trailing underscore followed by digits with no
leading zeros; anything else (say "Autosprint_C") is returned whole with number 0.
*/
void SplitName(const std::string& S, std::string& OutBase, int32& OutNumber);

/* FCrc::Strihash_DEPRECATED and FCrc::StrCrc32 — the two hashes stored per name-table entry. */
uint32 Strihash(const std::string& S);
uint32 StrCrc32(const std::string& S);

}   // namespace Uasset
