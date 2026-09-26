// Layout transcribed from UE4.27 PackageFileSummary.cpp (operator<<) and ObjectResource.cpp; the same fields
// Package.cpp writes for a package of our own.
#include "Cooked.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include "Package.h"
#include "Script.h"

namespace Uasset
{
namespace
{
constexpr uint32 kPackageFileTag = 0x9E2A83C1;
constexpr uint32 kPkgFilterEditorOnly = 0x80000000;

/* UTF-16 to UTF-8; a lone surrogate keeps its three-byte form, which Utf8To16 turns back into the same unit. */
std::string Utf16To8(const std::u16string& W)
{
    std::string Out;
    for (size_t I = 0; I < W.size(); ++I)
    {
        uint32 C = W[I];
        if (C >= 0xD800 && C < 0xDC00 && I + 1 < W.size() && W[I + 1] >= 0xDC00 && W[I + 1] < 0xE000)
            C = 0x10000 + ((C - 0xD800) << 10) + (uint32(W[++I]) - 0xDC00);
        if (C < 0x80) Out.push_back(char(C));
        else if (C < 0x800) { Out.push_back(char(0xC0 | C >> 6)); Out.push_back(char(0x80 | (C & 0x3F))); }
        else if (C < 0x10000)
        {
            Out.push_back(char(0xE0 | C >> 12));
            Out.push_back(char(0x80 | (C >> 6 & 0x3F)));
            Out.push_back(char(0x80 | (C & 0x3F)));
        }
        else
        {
            Out.push_back(char(0xF0 | C >> 18));
            Out.push_back(char(0x80 | (C >> 12 & 0x3F)));
            Out.push_back(char(0x80 | (C >> 6 & 0x3F)));
            Out.push_back(char(0x80 | (C & 0x3F)));
        }
    }
    return Out;
}

/* A little-endian cursor. A read past the end marks it bad and yields zeros, so a caller checks once at the end. */
struct FReader
{
    const std::vector<uint8>& B;
    size_t P = 0;
    bool bBad = false;

    explicit FReader(const std::vector<uint8>& InB) : B(InB) {}

    void Raw(void* Out, size_t N)
    {
        if (bBad || N > B.size() - P) { bBad = true; std::memset(Out, 0, N); return; }
        std::memcpy(Out, B.data() + P, N);
        P += N;
    }
    template <typename T> T Get() { T V; Raw(&V, sizeof V); return V; }
    int32 I32() { return Get<int32>(); }
    uint32 U32() { return Get<uint32>(); }
    uint16 U16() { return Get<uint16>(); }
    int64 I64() { return Get<int64>(); }
    bool Bool() { return U32() != 0; }
    FNameRef Name() { FNameRef N; N.Index = I32(); N.Number = I32(); return N; }
    void Guid(uint32 (&G)[4]) { for (uint32& V : G) V = U32(); }

    /* FString: 0 empty, a positive length of Latin-1 bytes, or a negative one of UTF-16 units; each counts the null. */
    std::string Str()
    {
        const int32 Len = I32();
        if (Len == 0 || bBad) return {};
        const size_t Units = size_t(Len > 0 ? Len : -int64(Len)), Bytes = Units * (Len > 0 ? 1 : 2);
        if (Units > (1u << 20) || Bytes > B.size() - P) { bBad = true; return {}; }
        std::u16string W;
        for (size_t I = 0; I + 1 < Units; ++I)
            W.push_back(Len > 0 ? char16_t(B[P + I]) : char16_t(B[P + 2 * I] | B[P + 2 * I + 1] << 8));
        P += Bytes;
        return Utf16To8(W);
    }
};

void PutName(FArc& Ar, const FNameRef& N) { Ar.I32(N.Index); Ar.I32(N.Number); }

std::vector<uint8> ReadAll(const std::filesystem::path& Path, bool* bOk)
{
    std::ifstream F(Path, std::ios::binary | std::ios::ate);
    std::vector<uint8> Out(F ? size_t(F.tellg()) : 0);
    F.seekg(0);
    *bOk = F && F.read(reinterpret_cast<char*>(Out.data()), std::streamsize(Out.size()));
    return Out;
}

bool WriteAll(const std::filesystem::path& Path, const std::vector<uint8>& Bytes)
{
    std::ofstream F(Path, std::ios::binary | std::ios::trunc);
    return F && F.write(reinterpret_cast<const char*>(Bytes.data()), std::streamsize(Bytes.size()));
}
}   // namespace

std::string FCookedPackage::NameOf(const FNameRef& N) const
{
    const std::string Base = N.Index >= 0 && size_t(N.Index) < Names.size() ? Names[size_t(N.Index)].Text : "?";
    return N.Number ? Base + "_" + std::to_string(N.Number - 1) : Base;
}

bool FCookedPackage::Load(const std::string& HeaderPath, std::string* Err)
{
    const std::filesystem::path Path = std::filesystem::u8path(HeaderPath);
    bool bOk = false;
    const std::vector<uint8> Header = ReadAll(Path, &bOk);
    if (!bOk) { *Err = "cannot read " + HeaderPath; return false; }
    std::filesystem::path ExpPath = Path;
    const std::vector<uint8> Exp = ReadAll(ExpPath.replace_extension(".uexp"), &bOk);
    if (!bOk) { *Err = "no .uexp"; return false; }
    return Read(Header, Exp, Err);
}

bool FCookedPackage::Read(const std::vector<uint8>& Header, const std::vector<uint8>& Exp, std::string* Err)
{
    auto Refuse = [&](const std::string& Why) { *Err = Why; return false; };
    FReader R(Header);

    /* The summary. Each field an unversioned FilterEditorOnly cook writes as a constant is checked against it. */
    if (R.U32() != kPackageFileTag) return Refuse("not a package");
    if (R.I32() != -7) return Refuse("legacy file version");
    const int32 LegacyUE3 = R.I32(), UE4 = R.I32(), Licensee = R.I32(), CustomVersions = R.I32();
    if (LegacyUE3 || UE4 || Licensee) return Refuse("versioned");
    if (CustomVersions) return Refuse("custom versions");
    const int32 TotalHeaderSize = R.I32();
    if (R.Str() != "None") return Refuse("folder name");
    PackageFlags = R.U32();
    if (!(PackageFlags & kPkgFilterEditorOnly)) return Refuse("editor-only data (no PKG_FilterEditorOnly)");
    const int32 NameCount = R.I32(), NameOffset = R.I32();
    if (R.I32() || R.I32()) return Refuse("gatherable text data");
    const int32 ExportCount = R.I32(), ExportOffset = R.I32(), ImportCount = R.I32(), ImportOffset = R.I32();
    const int32 DependsOffset = R.I32();
    if (R.I32() || R.I32()) return Refuse("soft package references");
    if (R.I32()) return Refuse("searchable names");
    if (R.I32()) return Refuse("thumbnails");
    R.Guid(Guid);
    if (R.I32() != 1) return Refuse("generations");
    if (R.I32() != ExportCount || R.I32() != NameCount) return Refuse("generation counts");
    for (int32 I = 0; I < 2; ++I)
        if (R.U16() || R.U16() || R.U16() || R.U32() || !R.Str().empty()) return Refuse("engine version");
    if (R.U32() || R.I32()) return Refuse("package compression");
    PackageSource = R.U32();
    if (R.I32()) return Refuse("additional packages to cook");
    const int32 AssetRegistryOffset = R.I32();
    const int64 BulkDataStartOffset = R.I64();
    if (R.I32()) return Refuse("world tile info");
    if (R.I32()) return Refuse("chunk ids");
    const int32 PreloadCount = R.I32(), PreloadOffset = R.I32();
    if (R.bBad) return Refuse("truncated summary");
    if (NameCount < 0 || ImportCount < 0 || ExportCount < 0 || PreloadCount < 0) return Refuse("negative count");

    /* The sections, back to back in this order; each offset has to be where the one before it ended. */
    auto At = [&](int32 Offset, const char* Section) {
        if (size_t(Offset) == R.P) return true;
        *Err = std::string("layout: ") + Section + " not where the section before it ends";
        return false;
    };
    if (!At(NameOffset, "name map")) return false;
    Names.resize(size_t(NameCount));
    for (FCookedName& N : Names)
    {
        N.Text = R.Str();
        N.NonCaseHash = R.U16();
        N.CaseHash = R.U16();
    }
    if (!At(ImportOffset, "imports")) return false;
    Imports.resize(size_t(ImportCount));
    for (FCookedImport& I : Imports)
    {
        I.ClassPackage = R.Name();
        I.ClassName = R.Name();
        I.Outer = R.I32();
        I.ObjectName = R.Name();
    }
    if (!At(ExportOffset, "exports")) return false;
    Exports.resize(size_t(ExportCount));
    std::vector<int64> SerialSize(Exports.size()), SerialOffset(Exports.size());
    for (size_t I = 0; I < Exports.size(); ++I)
    {
        FCookedExport& E = Exports[I];
        E.Class = R.I32();
        E.Super = R.I32();
        E.Template = R.I32();
        E.Outer = R.I32();
        E.ObjectName = R.Name();
        E.ObjectFlags = R.U32();
        SerialSize[I] = R.I64();
        SerialOffset[I] = R.I64();
        E.bForcedExport = R.Bool();
        E.bNotForClient = R.Bool();
        E.bNotForServer = R.Bool();
        R.Guid(E.PackageGuid);
        E.PackageFlags = R.U32();
        E.bNotAlwaysLoadedForEditorGame = R.Bool();
        E.bIsAsset = R.Bool();
        E.FirstExportDependency = R.I32();
        E.SerBeforeSer = R.I32();
        E.CreateBeforeSer = R.I32();
        E.SerBeforeCreate = R.I32();
        E.CreateBeforeCreate = R.I32();
    }
    if (!At(DependsOffset, "depends map")) return false;
    for (FCookedExport& E : Exports)
    {
        const int32 Count = R.I32();
        if (Count < 0 || size_t(Count) > (Header.size() - R.P) / 4) return Refuse("depends map");
        E.Depends.resize(size_t(Count));
        for (int32& D : E.Depends) D = R.I32();
    }
    if (!At(AssetRegistryOffset, "asset registry data")) return false;
    if (R.I32()) return Refuse("asset registry data");
    if (!At(PreloadOffset, "preload dependencies")) return false;
    PreloadDependencies.resize(size_t(PreloadCount));
    for (int32& D : PreloadDependencies) D = R.I32();
    if (R.bBad) return Refuse("truncated header");
    if (size_t(TotalHeaderSize) != R.P) return Refuse("layout: TotalHeaderSize");
    if (R.P != Header.size()) return Refuse("bytes after the header");

    /* The .uexp: every payload in export order, back to back, then the package tag. */
    int64 Cursor = TotalHeaderSize;
    for (size_t I = 0; I < Exports.size(); ++I)
    {
        if (SerialOffset[I] != Cursor) return Refuse("exports not in .uexp order");
        const int64 From = Cursor - TotalHeaderSize;
        if (SerialSize[I] < 0 || From + SerialSize[I] > int64(Exp.size())) return Refuse("export past the .uexp");
        Exports[I].Payload.assign(Exp.begin() + From, Exp.begin() + From + SerialSize[I]);
        Cursor += SerialSize[I];
    }
    const int64 Used = Cursor - TotalHeaderSize;
    uint32 Tail = 0;
    if (Used + 4 != int64(Exp.size())) return Refuse(".uexp holds more than the exports");
    std::memcpy(&Tail, Exp.data() + Used, 4);
    if (Tail != kPackageFileTag) return Refuse(".uexp tag");
    if (BulkDataStartOffset != Cursor) return Refuse("BulkDataStartOffset");
    return true;
}

void FCookedPackage::Write(std::vector<uint8>& HeaderOut, std::vector<uint8>& ExpOut) const
{
    FArc NameMap(nullptr), ImportMap(nullptr), DependsMap(nullptr), Preload(nullptr);
    for (const FCookedName& N : Names)
    {
        NameMap.Str(N.Text);
        NameMap.U16(uint16(Strihash(N.Text) & 0xFFFF));
        NameMap.U16(uint16(StrCrc32(N.Text) & 0xFFFF));
    }
    for (const FCookedImport& I : Imports)
    {
        PutName(ImportMap, I.ClassPackage);
        PutName(ImportMap, I.ClassName);
        ImportMap.I32(I.Outer);
        PutName(ImportMap, I.ObjectName);
    }
    for (const FCookedExport& E : Exports)
    {
        DependsMap.I32(int32(E.Depends.size()));
        for (int32 D : E.Depends) DependsMap.I32(D);
    }
    for (int32 D : PreloadDependencies) Preload.I32(D);
    constexpr int32 kExportRow = 104, kAssetRegistry = 4;

    struct FOffsets { int32 Total = 0, Names = 0, Imports = 0, Exports = 0, Depends = 0, Registry = 0, Preload = 0; int64 Bulk = 0; };
    auto Summary = [&](const FOffsets& O) {
        FArc S(nullptr);
        S.U32(kPackageFileTag);
        S.I32(-7);
        for (int32 I = 0; I < 4; ++I) S.I32(0);    // LegacyUE3Version, FileVersionUE4, licensee, custom versions
        S.I32(O.Total);
        S.Str("None");                              // FolderName
        S.U32(PackageFlags);
        S.I32(int32(Names.size()));
        S.I32(O.Names);
        S.I32(0); S.I32(0);                         // GatherableTextData
        S.I32(int32(Exports.size()));
        S.I32(O.Exports);
        S.I32(int32(Imports.size()));
        S.I32(O.Imports);
        S.I32(O.Depends);
        S.I32(0); S.I32(0);                         // SoftPackageReferences
        S.I32(0);                                   // SearchableNamesOffset
        S.I32(0);                                   // ThumbnailTableOffset
        S.Guid(Guid);
        S.I32(1);                                   // one generation
        S.I32(int32(Exports.size()));
        S.I32(int32(Names.size()));
        for (int32 I = 0; I < 2; ++I) { S.U16(0); S.U16(0); S.U16(0); S.U32(0); S.I32(0); }   // engine versions
        S.U32(0);                                   // CompressionFlags
        S.I32(0);                                   // CompressedChunks
        S.U32(PackageSource);
        S.I32(0);                                   // AdditionalPackagesToCook
        S.I32(O.Registry);
        S.I64(O.Bulk);
        S.I32(0);                                   // WorldTileInfoDataOffset
        S.I32(0);                                   // ChunkIDs
        S.I32(int32(PreloadDependencies.size()));
        S.I32(O.Preload);
        return S.B;
    };
    FOffsets O;
    O.Names = int32(Summary(O).size());
    O.Imports = O.Names + int32(NameMap.B.size());
    O.Exports = O.Imports + int32(ImportMap.B.size());
    O.Depends = O.Exports + kExportRow * int32(Exports.size());
    O.Registry = O.Depends + int32(DependsMap.B.size());
    O.Preload = O.Registry + kAssetRegistry;
    O.Total = O.Preload + int32(Preload.B.size());

    FArc ExportMap(nullptr);
    int64 Cursor = O.Total;
    for (const FCookedExport& E : Exports)
    {
        ExportMap.I32(E.Class);
        ExportMap.I32(E.Super);
        ExportMap.I32(E.Template);
        ExportMap.I32(E.Outer);
        PutName(ExportMap, E.ObjectName);
        ExportMap.U32(E.ObjectFlags);
        ExportMap.I64(int64(E.Payload.size()));
        ExportMap.I64(Cursor);
        ExportMap.Bool(E.bForcedExport);
        ExportMap.Bool(E.bNotForClient);
        ExportMap.Bool(E.bNotForServer);
        ExportMap.Guid(E.PackageGuid);
        ExportMap.U32(E.PackageFlags);
        ExportMap.Bool(E.bNotAlwaysLoadedForEditorGame);
        ExportMap.Bool(E.bIsAsset);
        ExportMap.I32(E.FirstExportDependency);
        ExportMap.I32(E.SerBeforeSer);
        ExportMap.I32(E.CreateBeforeSer);
        ExportMap.I32(E.SerBeforeCreate);
        ExportMap.I32(E.CreateBeforeCreate);
        Cursor += int64(E.Payload.size());
    }
    O.Bulk = Cursor;

    HeaderOut = Summary(O);
    for (const FArc* Section : { &NameMap, &ImportMap, &ExportMap, &DependsMap })
        HeaderOut.insert(HeaderOut.end(), Section->B.begin(), Section->B.end());
    HeaderOut.insert(HeaderOut.end(), size_t(kAssetRegistry), uint8(0));     // asset registry data: no objects
    HeaderOut.insert(HeaderOut.end(), Preload.B.begin(), Preload.B.end());

    ExpOut.clear();
    for (const FCookedExport& E : Exports) ExpOut.insert(ExpOut.end(), E.Payload.begin(), E.Payload.end());
    const uint32 Tag = kPackageFileTag;
    ExpOut.insert(ExpOut.end(), reinterpret_cast<const uint8*>(&Tag), reinterpret_cast<const uint8*>(&Tag) + 4);
}

bool FCookedPackage::Save(const std::string& HeaderPath, std::string* Err) const
{
    std::vector<uint8> Header, Exp;
    Write(Header, Exp);
    std::filesystem::path Path = std::filesystem::u8path(HeaderPath);
    if (!WriteAll(Path, Header)) { *Err = "cannot write " + HeaderPath; return false; }
    if (!WriteAll(Path.replace_extension(".uexp"), Exp)) { *Err = "cannot write the .uexp"; return false; }
    return true;
}

int RoundTrip(const std::string& Dir)
{
    size_t Packages = 0, Identical = 0, NamesTotal = 0, HashMismatch = 0, Unsorted = 0;
    std::map<std::string, std::pair<size_t, std::string>> Refused;      // reason -> count, first path
    std::vector<std::string> Differ;
    std::error_code Ec;
    for (auto It = std::filesystem::recursive_directory_iterator(std::filesystem::u8path(Dir), Ec);
         !Ec && It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
    {
        const std::filesystem::path& Path = It->path();
        const std::string Ext = Path.extension().string();
        if (Ext != ".uasset" && Ext != ".umap") continue;
        ++Packages;
        const std::string Shown = Path.u8string();

        bool bOk = false;
        const std::vector<uint8> Header = ReadAll(Path, &bOk);
        std::filesystem::path ExpPath = Path;
        const std::vector<uint8> Exp = bOk ? ReadAll(ExpPath.replace_extension(".uexp"), &bOk) : std::vector<uint8>();
        FCookedPackage P;
        std::string Err;
        if (!bOk) Err = "unreadable, or no .uexp";
        else if (P.Read(Header, Exp, &Err)) Err.clear();
        if (!Err.empty())
        {
            auto& Slot = Refused[Err];
            if (!Slot.first++) Slot.second = Shown;
            continue;
        }

        /* Would AssetGen's own writer have made this name map? The same hashes, and the same order. */
        NamesTotal += P.Names.size();
        for (const FCookedName& N : P.Names)
            HashMismatch += N.NonCaseHash != uint16(Strihash(N.Text) & 0xFFFF) || N.CaseHash != uint16(StrCrc32(N.Text) & 0xFFFF);
        Unsorted += !std::is_sorted(P.Names.begin(), P.Names.end(),
                                    [](const FCookedName& A, const FCookedName& B) { return Lower(A.Text) < Lower(B.Text); });

        std::vector<uint8> Header2, Exp2;
        P.Write(Header2, Exp2);
        if (Header2 == Header && Exp2 == Exp) { ++Identical; continue; }
        const std::vector<uint8>& A = Header2 == Header ? Exp2 : Header2;
        const std::vector<uint8>& B = Header2 == Header ? Exp : Header;
        size_t At = 0;
        while (At < A.size() && At < B.size() && A[At] == B[At]) ++At;
        char Where[96];
        snprintf(Where, sizeof Where, "  (%s differs at 0x%zx, sizes %zu vs %zu)", Header2 == Header ? ".uexp" : "header",
                 At, A.size(), B.size());
        Differ.push_back(Shown + Where);
    }
    if (Ec) printf("  walk stopped: %s\n", Ec.message().c_str());

    size_t RefusedCount = 0;
    for (const auto& R : Refused) RefusedCount += R.second.first;
    printf("roundtrip %s: %zu packages, %zu identical, %zu differ, %zu refused\n", Dir.c_str(), Packages, Identical,
           Differ.size(), RefusedCount);
    for (const auto& [Why, Slot] : Refused) printf("  refused x%zu: %s  (first: %s)\n", Slot.first, Why.c_str(), Slot.second.c_str());
    for (size_t I = 0; I < Differ.size() && I < 20; ++I) printf("  differs: %s\n", Differ[I].c_str());
    printf("  name maps in AssetGen's order: %zu of %zu read; stored name hashes AssetGen computes too: %zu of %zu\n",
           Packages - RefusedCount - Unsorted, Packages - RefusedCount, NamesTotal - HashMismatch, NamesTotal);
    return Packages && Identical == Packages ? 0 : 1;
}

}   // namespace Uasset
