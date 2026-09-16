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

std::string Lower(const std::string& S)
{
    std::string R = S;
    for (char& C : R) if (C >= 'A' && C <= 'Z') C = char(C - 'A' + 'a');
    return R;
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
    }
    if (ImportTable.B.size() != size_t(kImportEntrySize) * Imports.size())
        return Fail("import entry size drifted from 28 bytes");

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
    const int32 PreloadCount = int32(PreloadDeps.B.size() / 4);

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
        S.I32(0);                                   // LegacyUE3Version
        S.I32(0);                                   // FileVersionUE4
        S.I32(0);                                   // FileVersionLicenseeUE4
        S.I32(0);                                   // CustomVersion count
        S.I32(O.TotalHeaderSize);
        S.Str("None");                              // FolderName
        S.U32(kPkgFilterEditorOnly);
        S.I32(int32(Names.size()));
        S.I32(O.NameOff);
        // LocalizationId omitted: PKG_FilterEditorOnly.
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
        // PersistentGuid omitted: PKG_FilterEditorOnly.
        S.I32(1);                                   // Generations count
        S.I32(int32(Exports.size()));
        S.I32(int32(Names.size()));
        for (int32 I = 0; I < 2; ++I)               // SavedByEngineVersion, CompatibleWithEngineVersion
        {
            S.U16(0); S.U16(0); S.U16(0); S.U32(0); S.I32(0);
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

    const int32 SummarySize = int32(WriteSummary(Off).size());
    Off.NameOff = SummarySize;
    Off.ImportOff = Off.NameOff + int32(NameTable.size());
    Off.ExportOff = Off.ImportOff + int32(ImportTable.B.size());
    Off.DependsOff = Off.ExportOff + kExportEntrySize * int32(Exports.size());
    Off.AssetRegistryOff = Off.DependsOff + 4 * int32(Exports.size());
    Off.PreloadOff = Off.AssetRegistryOff + 4;
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
        ExportTable.I32(int32(E.SerBeforeSer.size()));
        ExportTable.I32(int32(E.CreateBeforeSer.size()));
        ExportTable.I32(int32(E.SerBeforeCreate.size()));
        ExportTable.I32(int32(CreateBeforeCreate[I].size()));
    }
    if (ExportTable.B.size() != size_t(kExportEntrySize) * Exports.size())
        return Fail("export entry size drifted from 104 bytes");

    std::vector<uint8> Header = WriteSummary(Off);
    Header.insert(Header.end(), NameTable.begin(), NameTable.end());
    Header.insert(Header.end(), ImportTable.B.begin(), ImportTable.B.end());
    Header.insert(Header.end(), ExportTable.B.begin(), ExportTable.B.end());
    for (size_t I = 0; I < Exports.size(); ++I) { const int32 Z = 0; Header.insert(Header.end(), (const uint8*)&Z, (const uint8*)&Z + 4); }
    { const int32 Z = 0; Header.insert(Header.end(), (const uint8*)&Z, (const uint8*)&Z + 4); }   // AssetRegistry: no tags
    Header.insert(Header.end(), PreloadDeps.B.begin(), PreloadDeps.B.end());
    if (int32(Header.size()) != Off.TotalHeaderSize) return Fail("header size mismatch");

    std::vector<uint8> Exp;
    for (const std::vector<uint8>& P : Payloads) Exp.insert(Exp.end(), P.begin(), P.end());
    const uint32 Tail = kPackageFileTag;            // .uexp ends with the package tag
    Exp.insert(Exp.end(), (const uint8*)&Tail, (const uint8*)&Tail + 4);

    auto Dump = [&](const std::string& Path, const std::vector<uint8>& Bytes) {
        FILE* F = nullptr;
        if (fopen_s(&F, Path.c_str(), "wb") != 0 || !F) return false;
        const bool bOk = fwrite(Bytes.data(), 1, Bytes.size(), F) == Bytes.size();
        fclose(F);
        return bOk;
    };
    if (!Dump(OutBaseNoExt + ".uasset", Header)) return Fail("cannot write .uasset");
    if (!Dump(OutBaseNoExt + ".uexp", Exp)) return Fail("cannot write .uexp");
    return true;
}

}   // namespace Uasset
