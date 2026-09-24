// Layout transcribed from UE4.27 PackageFileSummary.cpp / LinkerSave.cpp / UnrealNames.cpp.
#include "Package.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Uasset
{
namespace
{
constexpr uint32 kPackageFileTag = 0x9E2A83C1;
constexpr int32 kLegacyFileVersion = -7;
constexpr uint32 kPkgFilterEditorOnly = 0x80000000;
constexpr int32 kImportEntrySize = 28;
constexpr int32 kExportEntrySize = 104;
/* Uncooked: FObjectImport grows an editor-only PackageName FName (VER_UE4_NON_OUTER_PACKAGE_IMPORT). */
constexpr int32 kUncookedImportEntrySize = 36;

/* Measured on a 4.27.2 editor-saved package. The editor refuses unversioned non-cooked content, and
   a missing custom version reads as "oldest", which silently flips serializers onto legacy branches -
   so the whole set is reproduced, not just the file version. */
constexpr int32 kUncookedFileVersionUE4 = 522;
constexpr int32 kUncookedLegacyUE3Version = 864;
struct FCustomVersionEntry
{
    uint32 Guid[4];
    int32 Version;
};
const FCustomVersionEntry kUncookedCustomVersions[] = {
    { { 0x29E575DD, 0xE0A34627, 0x9D10D276, 0x232CDCEA }, 17 },
    { { 0x375EC13C, 0x06E448FB, 0xB50084F0, 0x262A717E }, 4 },
    { { 0x601D1886, 0xAC644F84, 0xAA16D3DE, 0x0DEAC7D6 }, 47 },
    { { 0x9C54D522, 0xA8264FBE, 0x94210746, 0x61B482D0 }, 43 },
    { { 0xB0D832E4, 0x1F894F0D, 0xACCF7EB7, 0x36FD4AA2 }, 10 },
    { { 0xCFFC743F, 0x43B04480, 0x939114DF, 0x171D2073 }, 37 },
    { { 0xE4B068ED, 0xF49442E9, 0xA231DA0B, 0x2E46BB41 }, 40 },
};

/* Deprecated = CRCTable_DEPRECATED (MSB-first 0x04C11DB7, used by Strihash); Reflected = CRCTablesSB8[0] (used by StrCrc32). */
struct FCrcTables
{
    uint32 Deprecated[256]{};
    uint32 Reflected[256]{};

    FCrcTables()
    {
        for (uint32 I = 0; I < 256; ++I)
        {
            uint32 C = I << 24;
            for (int32 J = 0; J < 8; ++J) C = (C & 0x80000000u) ? ((C << 1) ^ 0x04C11DB7u) : (C << 1);
            Deprecated[I] = C;

            C = I;
            for (int32 J = 0; J < 8; ++J) C = (C & 1u) ? ((C >> 1) ^ 0xEDB88320u) : (C >> 1);
            Reflected[I] = C;
        }
    }
};
const FCrcTables& Crc()
{
    static const FCrcTables Tables;
    return Tables;
}

std::string GuidString(const uint32 (&G)[4])
{
    static const char* const Hex = "0123456789ABCDEF";
    std::string Out;
    for (uint32 V : G)
        for (int32 Shift = 28; Shift >= 0; Shift -= 4) Out.push_back(Hex[(V >> Shift) & 0xF]);
    return Out;
}

void WriteFString(std::vector<uint8>& B, const std::string& S)
{
    const int32 Len = int32(S.size()) + 1;
    const uint8* P = reinterpret_cast<const uint8*>(&Len);
    B.insert(B.end(), P, P + 4);
    B.insert(B.end(), S.begin(), S.end());
    B.push_back(0);
}
}   // namespace

std::string Lower(const std::string& S)
{
    std::string R = S;
    for (char& C : R) if (C >= 'A' && C <= 'Z') C = char(C - 'A' + 'a');
    return R;
}

void SplitName(const std::string& S, std::string& OutBase, int32& OutNumber)
{
    OutBase = S;
    OutNumber = 0;

    const size_t Underscore = S.rfind('_');
    if (Underscore == std::string::npos || Underscore + 1 >= S.size()) return;

    const std::string Digits = S.substr(Underscore + 1);
    if (Digits.size() > 1 && Digits[0] == '0') return;       // leading zeros are part of the name
    if (Digits.size() > 9) return;
    for (char C : Digits) if (C < '0' || C > '9') return;

    OutBase = S.substr(0, Underscore);
    OutNumber = std::stoi(Digits) + 1;                       // stored number is one-based
}

uint32 Strihash(const std::string& S)
{
    uint32 Hash = 0;
    for (char Ch : S)
    {
        const uint8 B = uint8((Ch >= 'a' && Ch <= 'z') ? Ch - 'a' + 'A' : Ch);
        Hash = ((Hash >> 8) & 0x00FFFFFFu) ^ Crc().Deprecated[(Hash ^ B) & 0xFFu];
    }
    return Hash;
}

uint32 StrCrc32(const std::string& S)
{
    uint32 C = 0xFFFFFFFFu;
    for (char Ch : S)
    {
        const uint32 W = uint8(Ch);   // hashes all four bytes of the widened TCHAR
        for (int32 Shift : { 0, 8, 16, 24 })
            C = (C >> 8) ^ Crc().Reflected[(C ^ ((W >> Shift) & 0xFFu)) & 0xFFu];
    }
    return C ^ 0xFFFFFFFFu;
}

/* ---- FArc ---- */

void FArc::Raw(const void* P, size_t N)
{
    const uint8* Bytes = static_cast<const uint8*>(P);
    B.insert(B.end(), Bytes, Bytes + N);
}

void FArc::Str(const std::string& S) { WriteFString(B, S); }

void FArc::Name(const std::string& S, int32 Number)
{
    std::string Base;
    int32 Split = 0;
    SplitName(S, Base, Split);
    I32(Pkg->NameIndex(S));
    I32(Number ? Number : Split);
}

void FArc::Guid(const uint32 (&G)[4])
{
    for (uint32 V : G) U32(V);
}

void TagBool(FArc& Ar, const std::string& Name, bool Value)
{
    Ar.Name(Name);
    Ar.Name("BoolProperty");
    Ar.I32(0);                                    // Size
    Ar.I32(0);                                    // ArrayIndex
    Ar.U8(Value ? 1 : 0);                         // BoolVal
    Ar.U8(0);                                     // HasPropertyGuid
}

void Tag(FArc& Ar, const std::string& Name, const std::string& Type,
         const std::function<void(FArc&)>& Value, const std::string& StructName)
{
    FArc Scratch(Ar.Owner());
    Value(Scratch);

    Ar.Name(Name);
    Ar.Name(Type);
    Ar.I32(int32(Scratch.B.size()));
    Ar.I32(0);                                    // ArrayIndex
    if (Type == "StructProperty")
    {
        Ar.Name(StructName);
        for (int32 I = 0; I < 4; ++I) Ar.U32(0);  // StructGuid, zero for engine structs
    }
    else if (Type == "ByteProperty" || Type == "EnumProperty")
        Ar.Name(StructName.empty() ? "None" : StructName);          // EnumName
    else if (Type == "ArrayProperty" || Type == "SetProperty")
        Ar.Name(StructName);                                        // InnerType
    else if (Type == "MapProperty")
    {
        const size_t Comma = StructName.find(',');                  // "InnerType,ValueType"
        Ar.Name(StructName.substr(0, Comma));
        Ar.Name(StructName.substr(Comma + 1));
    }
    Ar.U8(0);                                     // HasPropertyGuid
    Ar.Append(Scratch);
}

/* ---- FPackage ---- */

int32 FPackage::AddImport(const FImport& In)
{
    NameIndex(In.ClassPackage);
    NameIndex(In.ClassName);
    NameIndex(In.ObjectName);
    Imports.push_back(In);
    return int32(Imports.size()) - 1;
}

const FImport* FPackage::ImportAt(FIndex Idx) const
{
    if (Idx.V >= 0) return nullptr;
    const size_t Row = size_t(-Idx.V - 1);
    return Row < Imports.size() ? &Imports[Row] : nullptr;
}

int32 FPackage::AddExport(FExport&& In)
{
    NameIndex(In.ObjectName);
    Exports.push_back(std::move(In));
    return int32(Exports.size()) - 1;
}

int32 FPackage::NameIndex(const std::string& S)
{
    // Only the base of a numbered name reaches the table.
    std::string Base;
    int32 Number = 0;
    SplitName(S, Base, Number);

    const std::string Key = Lower(Base);
    auto It = NameLookup.find(Key);
    if (It != NameLookup.end()) return It->second;

    // Pass one returns 0 for everything; every FName is 8 bytes regardless, so sizes still line up.
    if (bNamesFinal) return 0;
    NameLookup.emplace(Key, 0);
    Names.push_back(Base);
    return 0;
}

bool FPackage::Save(const std::string& OutBaseNoExt, std::string* Err) const
{
    auto Fail = [&](const char* Msg) { if (Err) *Err = Msg; return false; };

    FPackage* Self = const_cast<FPackage*>(this);
    Self->NameIndex(PackageName);

    // Pass one: run every payload only to collect FNames.
    for (const FExport& E : Exports)
    {
        FArc Discard(Self);
        E.Serialize(Discard);
    }

    // The name table is stored sorted case-insensitively; indices follow that order.
    std::sort(Self->Names.begin(), Self->Names.end(),
              [](const std::string& A, const std::string& B) { return Lower(A) < Lower(B); });
    Self->NameLookup.clear();
    for (int32 I = 0; I < int32(Names.size()); ++I) Self->NameLookup[Lower(Names[size_t(I)])] = I;
    Self->bNamesFinal = true;

    // Pass two: real bytes.
    std::vector<std::vector<uint8>> Payloads;
    Payloads.reserve(Exports.size());
    for (const FExport& E : Exports)
    {
        FArc Ar(Self);
        E.Serialize(Ar);
        Payloads.push_back(std::move(Ar.B));
    }

    std::vector<uint8> NameTable;
    for (const std::string& S : Names)
    {
        WriteFString(NameTable, S);
        const uint16 H1 = uint16(Strihash(S) & 0xFFFF);
        const uint16 H2 = uint16(StrCrc32(S) & 0xFFFF);
        const uint8* P1 = reinterpret_cast<const uint8*>(&H1);
        const uint8* P2 = reinterpret_cast<const uint8*>(&H2);
        NameTable.insert(NameTable.end(), P1, P1 + 2);
        NameTable.insert(NameTable.end(), P2, P2 + 2);
    }

    FArc ImportTable(Self);
    for (const FImport& I : Imports)
    {
        ImportTable.Name(I.ClassPackage);
        ImportTable.Name(I.ClassName);
        ImportTable.Idx(I.Outer);
        ImportTable.Name(I.ObjectName);
        if (bUncooked) ImportTable.Name("None");    // PackageName: only set for a package override
    }
    const int32 ImportEntrySize = bUncooked ? kUncookedImportEntrySize : kImportEntrySize;
    if (ImportTable.B.size() != size_t(ImportEntrySize) * Imports.size())
        return Fail("import entry size drifted");

    // EDL table: one flat run of FPackageIndex; each export row gives its first index plus four per-phase counts.
    FArc PreloadDeps(Self);
    std::vector<int32> FirstDep(Exports.size(), -1);
    std::vector<std::vector<int32>> CreateBeforeCreate(Exports.size());
    for (size_t I = 0; I < Exports.size(); ++I)
    {
        const FExport& E = Exports[I];
        CreateBeforeCreate[I] = E.CreateBeforeCreate;

        // An export that declares nothing gets CreateBeforeCreate on the objects its own row references.
        if (E.SerBeforeSer.empty() && E.CreateBeforeSer.empty()
            && E.SerBeforeCreate.empty() && E.CreateBeforeCreate.empty())
        {
            for (FIndex Ref : { E.ClassIndex, E.SuperIndex, E.TemplateIndex, E.OuterIndex })
            {
                if (Ref.V == 0) continue;
                std::vector<int32>& Into = CreateBeforeCreate[I];
                if (std::find(Into.begin(), Into.end(), Ref.V) == Into.end()) Into.push_back(Ref.V);
            }
        }

        const size_t Count = E.SerBeforeSer.size() + E.CreateBeforeSer.size()
                           + E.SerBeforeCreate.size() + CreateBeforeCreate[I].size();
        FirstDep[I] = Count ? int32(PreloadDeps.B.size() / 4) : -1;
        const std::vector<int32>* const Phases[4] = {
            &E.SerBeforeSer, &E.CreateBeforeSer, &E.SerBeforeCreate, &CreateBeforeCreate[I]
        };
        for (const std::vector<int32>* List : Phases)
            for (int32 V : *List) PreloadDeps.I32(V);
    }
    if (bUncooked)
    {
        // The editor writes no EDL table; the count is -1, not 0, and every export row says -1 too.
        PreloadDeps.B.clear();
        for (size_t I = 0; I < Exports.size(); ++I) { FirstDep[I] = -1; CreateBeforeCreate[I].clear(); }
    }
    const int32 PreloadCount = bUncooked ? -1 : int32(PreloadDeps.B.size() / 4);

    // The summary is written once with zero offsets to measure itself, then again for real.
    struct FOffsets
    {
        int32 TotalHeaderSize = 0, NameOff = 0, ExportOff = 0, ImportOff = 0;
        int32 DependsOff = 0, AssetRegistryOff = 0, PreloadOff = 0;
        int64 BulkDataStart = 0;
    } Off;

    auto WriteSummary = [&](const FOffsets& O) {
        FArc S(Self);
        S.U32(kPackageFileTag);
        S.I32(kLegacyFileVersion);
        S.I32(bUncooked ? kUncookedLegacyUE3Version : 0);
        S.I32(bUncooked ? kUncookedFileVersionUE4 : 0);
        S.I32(0);                                   // FileVersionLicenseeUE4
        if (bUncooked)
        {
            S.I32(int32(sizeof(kUncookedCustomVersions) / sizeof(kUncookedCustomVersions[0])));
            for (const FCustomVersionEntry& V : kUncookedCustomVersions) { S.Guid(V.Guid); S.I32(V.Version); }
        }
        else
            S.I32(0);                               // CustomVersion count
        S.I32(O.TotalHeaderSize);
        S.Str("None");                              // FolderName
        S.U32(bUncooked ? 0 : kPkgFilterEditorOnly);
        S.I32(int32(Names.size()));
        S.I32(O.NameOff);
        if (bUncooked) S.Str(GuidString(PkgGuid));       // editor-only, dropped by PKG_FilterEditorOnly
        S.I32(0); S.I32(0);                         // GatherableTextData count/offset
        S.I32(int32(Exports.size()));
        S.I32(O.ExportOff);
        S.I32(int32(Imports.size()));
        S.I32(O.ImportOff);
        S.I32(O.DependsOff);
        S.I32(0); S.I32(0);                         // SoftPackageReferences count/offset
        S.I32(0);                                   // SearchableNamesOffset
        S.I32(0);                                   // ThumbnailTableOffset
        S.Guid(PkgGuid);
        if (bUncooked) S.Guid(PkgGuid);             // PersistentGuid, editor-only
        S.I32(1);                                   // Generations count
        S.I32(int32(Exports.size()));
        S.I32(int32(Names.size()));
        for (int32 I = 0; I < 2; ++I)               // SavedByEngineVersion, CompatibleWithEngineVersion
        {
            // A cooked package is unversioned throughout; the editor reads these and warns when a
            // package claims a newer engine than its own, so an uncooked one names 4.27.
            if (bUncooked)
            {
                S.U16(4); S.U16(27); S.U16(I == 0 ? 2 : 0);
                S.U32(I == 0 ? 18319896u : 17155196u);
                S.Str("++UE4+Release-4.27");
            }
            else { S.U16(0); S.U16(0); S.U16(0); S.U32(0); S.I32(0); }
        }
        S.U32(0);                                   // CompressionFlags
        S.I32(0);                                   // CompressedChunks
        S.U32(PackageSource);
        S.I32(0);                                   // AdditionalPackagesToCook
        S.I32(O.AssetRegistryOff);
        S.I64(O.BulkDataStart);
        S.I32(0);                                   // WorldTileInfoDataOffset
        S.I32(0);                                   // ChunkIDs
        S.I32(PreloadCount);
        S.I32(O.PreloadOff);
        return S.B;
    };

    /*
    Uncooked AssetRegistryData: an int64 pointing past the rows, then one row per object, then the
    per-import "used in game" bit arrays the offset names. This is what the content browser reads
    without loading the package, so an uncooked asset with an empty block shows up as nothing.
    */
    std::vector<uint8> RegistryRows;
    std::vector<uint8> DepBlock;
    if (bUncooked)
    {
        auto I32Into = [](std::vector<uint8>& B, int32 V) {
            const uint8* P = reinterpret_cast<const uint8*>(&V);
            B.insert(B.end(), P, P + 4);
        };
        I32Into(RegistryRows, int32(RegistryObjects.size()));
        for (const FRegistryObject& Row : RegistryObjects)
        {
            WriteFString(RegistryRows, Row.ObjectPath);
            WriteFString(RegistryRows, Row.ClassName);
            I32Into(RegistryRows, int32(Row.Tags.size()));
            for (const auto& Tag : Row.Tags)
            {
                WriteFString(RegistryRows, Tag.first);
                WriteFString(RegistryRows, Tag.second);
            }
        }
        // TBitArray: bit count, then one uint32 per 32 bits. Every import counts as used in game.
        I32Into(DepBlock, int32(Imports.size()));
        for (size_t Word = 0; Word * 32 < Imports.size(); ++Word)
        {
            const size_t Bits = Imports.size() - Word * 32;
            I32Into(DepBlock, Bits >= 32 ? int32(0xFFFFFFFF) : int32((1u << Bits) - 1));
        }
        I32Into(DepBlock, 0);                       // SoftPackageUsedInGame: no soft references
    }

    const int32 SummarySize = int32(WriteSummary(Off).size());
    Off.NameOff = SummarySize;
    Off.ImportOff = Off.NameOff + int32(NameTable.size());
    Off.ExportOff = Off.ImportOff + int32(ImportTable.B.size());
    Off.DependsOff = Off.ExportOff + kExportEntrySize * int32(Exports.size());
    Off.AssetRegistryOff = Off.DependsOff + 4 * int32(Exports.size());
    Off.PreloadOff = Off.AssetRegistryOff
                   + (bUncooked ? 8 + int32(RegistryRows.size()) + int32(DepBlock.size()) : 4);
    Off.TotalHeaderSize = Off.PreloadOff + int32(PreloadDeps.B.size());

    int64 PayloadCursor = Off.TotalHeaderSize;
    for (const std::vector<uint8>& P : Payloads) PayloadCursor += int64(P.size());
    Off.BulkDataStart = PayloadCursor;

    FArc ExportTable(Self);
    for (size_t I = 0; I < Exports.size(); ++I)
    {
        const FExport& E = Exports[I];
        int64 SerialOffset = Off.TotalHeaderSize;
        for (size_t J = 0; J < I; ++J) SerialOffset += int64(Payloads[J].size());

        ExportTable.Idx(E.ClassIndex);
        ExportTable.Idx(E.SuperIndex);
        ExportTable.Idx(E.TemplateIndex);
        ExportTable.Idx(E.OuterIndex);
        ExportTable.Name(E.ObjectName);
        ExportTable.U32(E.ObjectFlags);
        ExportTable.I64(int64(Payloads[I].size()));
        ExportTable.I64(SerialOffset);
        ExportTable.Bool(false);                    // bForcedExport
        ExportTable.Bool(false);                    // bNotForClient
        ExportTable.Bool(false);                    // bNotForServer
        for (int32 J = 0; J < 4; ++J) ExportTable.U32(0);   // PackageGuid
        ExportTable.U32(0);                         // PackageFlags
        ExportTable.Bool(false);                    // bNotAlwaysLoadedForEditorGame
        ExportTable.Bool(E.bIsAsset);
        ExportTable.I32(FirstDep[I]);
        ExportTable.I32(bUncooked ? 0 : int32(E.SerBeforeSer.size()));
        ExportTable.I32(bUncooked ? 0 : int32(E.CreateBeforeSer.size()));
        ExportTable.I32(bUncooked ? 0 : int32(E.SerBeforeCreate.size()));
        ExportTable.I32(bUncooked ? 0 : int32(CreateBeforeCreate[I].size()));
    }
    if (ExportTable.B.size() != size_t(kExportEntrySize) * Exports.size())
        return Fail("export entry size drifted from 104 bytes");

    std::vector<uint8> Header = WriteSummary(Off);
    Header.insert(Header.end(), NameTable.begin(), NameTable.end());
    Header.insert(Header.end(), ImportTable.B.begin(), ImportTable.B.end());
    Header.insert(Header.end(), ExportTable.B.begin(), ExportTable.B.end());
    for (size_t I = 0; I < Exports.size(); ++I) { const int32 Z = 0; Header.insert(Header.end(), (const uint8*)&Z, (const uint8*)&Z + 4); }
    if (bUncooked)
    {
        const int64 DependencyDataOffset = int64(Off.AssetRegistryOff) + 8 + int64(RegistryRows.size());
        Header.insert(Header.end(), (const uint8*)&DependencyDataOffset, (const uint8*)&DependencyDataOffset + 8);
        Header.insert(Header.end(), RegistryRows.begin(), RegistryRows.end());
        Header.insert(Header.end(), DepBlock.begin(), DepBlock.end());
    }
    else
    { const int32 Z = 0; Header.insert(Header.end(), (const uint8*)&Z, (const uint8*)&Z + 4); }   // AssetRegistry: no tags
    Header.insert(Header.end(), PreloadDeps.B.begin(), PreloadDeps.B.end());
    if (int32(Header.size()) != Off.TotalHeaderSize) return Fail("header size mismatch");

    std::vector<uint8> Exp;
    for (const std::vector<uint8>& P : Payloads) Exp.insert(Exp.end(), P.begin(), P.end());
    const uint32 Tail = kPackageFileTag;            // .uexp ends with the package tag
    Exp.insert(Exp.end(), (const uint8*)&Tail, (const uint8*)&Tail + 4);

    auto Dump = [&](const std::string& Path, const std::vector<uint8>& Bytes) {
        FILE* F = fopen(Path.c_str(), "wb");
        if (!F) return false;
        const bool bOk = fwrite(Bytes.data(), 1, Bytes.size(), F) == Bytes.size();
        fclose(F);
        return bOk;
    };
    if (bUncooked)
    {
        Header.insert(Header.end(), Exp.begin(), Exp.end());
        remove((OutBaseNoExt + ".uexp").c_str());   // a stale cooked pair next to it would shadow this
        return Dump(OutBaseNoExt + ".uasset", Header) ? true : Fail("cannot write .uasset");
    }
    if (!Dump(OutBaseNoExt + ".uasset", Header)) return Fail("cannot write .uasset");
    if (!Dump(OutBaseNoExt + ".uexp", Exp)) return Fail("cannot write .uexp");
    return true;
}

}   // namespace Uasset
