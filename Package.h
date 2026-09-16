#pragma once
// Cooked UE4.27 package writer. Unversioned (FileVersionUE4 = 0), tagged properties (not
// PKG_UnversionedProperties), PKG_FilterEditorOnly (drops LocalizationId/PersistentGuid).
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "SharedLib/core/Types.h"
#include "UeEnums.h"

namespace Uasset
{
/* FPackageIndex: 0 null, +n Exports[n-1], -n Imports[n-1]. Build via Exp()/Imp(). */
struct FIndex
{
    int32 V = 0;
};
inline FIndex Null() { return FIndex{ 0 }; }
inline FIndex Exp(int32 ExportIdx) { return FIndex{ ExportIdx + 1 }; }
inline FIndex Imp(int32 ImportIdx) { return FIndex{ -ImportIdx - 1 }; }

class FPackage;

/* Little-endian byte sink. Bool is 4 bytes (FArchive serializes bool as int32). */
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

    void Str(const std::string& S);                            // FString: length incl. null, ANSI bytes, null
    void Name(const std::string& S, int32 Number = 0);         // FName: table index + number; registers S with the package
    void Guid(const uint32 (&G)[4]);
    void Append(const FArc& Other) { Raw(Other.B.data(), Other.B.size()); }

    FPackage* Owner() const { return Pkg; }
    std::vector<uint8> B;

private:
    FPackage* Pkg = nullptr;
};

/* One FPropertyTag + value. Value runs into a scratch archive first so the tag's Size is measured.
   StructName is the StructProperty's struct, or the ByteProperty/EnumProperty's enum (empty = None). */
void Tag(FArc& Ar, const std::string& Name, const std::string& Type,
         const std::function<void(FArc&)>& Value, const std::string& StructName = "");

/* A bool's value lives in the tag (BoolVal) with zero payload; writing one via Tag() loads as false. */
void TagBool(FArc& Ar, const std::string& Name, bool Value);

inline void TagEnd(FArc& Ar) { Ar.Name("None"); }

struct FImport
{
    std::string ClassPackage;
    std::string ClassName;
    FIndex Outer;
    std::string ObjectName;
};

/*
Serialize runs twice per Save (FName discovery, then real bytes) and must be deterministic.
The four lists are EDL preload dependencies as raw FPackageIndex values.
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

class FPackage
{
public:
    explicit FPackage(std::string InPackageName) : PackageName(std::move(InPackageName)) {}

    int32 AddImport(const FImport& In);
    int32 AddExport(FExport&& In);
    FIndex ImportIndex(int32 Row) const { return Imp(Row); }

    /* Pass one registers and returns 0; pass two resolves to the sorted table index. */
    int32 NameIndex(const std::string& S);

    void SetGuid(uint32 A, uint32 B, uint32 C, uint32 D) { PkgGuid[0] = A; PkgGuid[1] = B; PkgGuid[2] = C; PkgGuid[3] = D; }
    void SetPackageSource(uint32 S) { PackageSource = S; }

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

/* FName number convention: "SCS_Node_0" is base "SCS_Node" number 1. Trailing "_<digits>" only, no leading zeros. */
void SplitName(const std::string& S, std::string& OutBase, int32& OutNumber);

/* FCrc::Strihash_DEPRECATED and FCrc::StrCrc32, the two per-name-table-entry hashes. */
uint32 Strihash(const std::string& S);
uint32 StrCrc32(const std::string& S);

}   // namespace Uasset
