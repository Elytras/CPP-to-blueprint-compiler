#include "Registry.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <unordered_map>

#include "Package.h"
#include "Script.h"

namespace Uasset
{
namespace
{
constexpr uint32 kVersionGuid[4] = { 0x717F9EE7, 0xE9B0493A, 0x88B39132, 0x1B388107 };
constexpr int32 kVersionFixedTags = 8;          // FAssetRegistryVersion::LatestVersion in 4.27
constexpr uint32 kStoreBeginMagic = 0x12345679;
constexpr uint32 kStoreEndMagic = 0x87654321;
constexpr int32 kStoreViewCount = 11;           // FixedTagPrivate::VisitViews
constexpr uint32 kNumberedNameBit = 0x80000000;
constexpr uint64 kUnusableHashVersion = 0;      // != FNameHash::AlgorithmId, so the loader rehashes (UnrealNames.cpp CanUseSavedHashes)

/* Registry FNames index a first-use-order name batch, not a package header's sorted table. */
class FBin
{
public:
    void U8(uint8 V) { B.push_back(V); }
    void U32(uint32 V) { Raw(&V, 4); }
    void I32(int32 V) { Raw(&V, 4); }
    void U64(uint64 V) { Raw(&V, 8); }
    void I64(int64 V) { Raw(&V, 8); }

    void Raw(const void* P, size_t N)
    {
        const uint8* Bytes = static_cast<const uint8*>(P);
        B.insert(B.end(), Bytes, Bytes + N);
    }

    void Name(const std::string& S)
    {
        std::string Base;
        int32 Number = 0;
        SplitName(S, Base, Number);

        const uint32 Index = NameIndex(Base);
        if (Number == 0) { U32(Index); return; }
        U32(Index | kNumberedNameBit);
        U32(uint32(Number));
    }

    const std::vector<std::string>& Names() const { return NameList; }
    std::vector<uint8> B;

private:
    /* Case-insensitive pool, first spelling wins, like the engine's name pool. */
    uint32 NameIndex(const std::string& S)
    {
        std::string Key = S;
        for (char& C : Key) if (C >= 'A' && C <= 'Z') C = char(C - 'A' + 'a');

        auto It = NameLookup.find(Key);
        if (It != NameLookup.end()) return It->second;

        const uint32 Index = uint32(NameList.size());
        NameLookup.emplace(Key, Index);
        NameList.push_back(S);
        return Index;
    }

    std::vector<std::string> NameList;
    std::unordered_map<std::string, uint32> NameLookup;
};

std::string PackagePathOf(const std::string& PackageName)
{
    const size_t Slash = PackageName.rfind('/');
    return Slash == std::string::npos ? PackageName : PackageName.substr(0, Slash);
}

std::string Utf16To8(const std::u16string& W)
{
    std::string S;
    for (size_t I = 0; I < W.size(); ++I)
    {
        uint32 Cp = W[I];
        if (Cp >= 0xD800 && Cp < 0xDC00 && I + 1 < W.size()) Cp = 0x10000 + ((Cp - 0xD800) << 10) + (W[++I] - 0xDC00);
        if (Cp < 0x80) S.push_back(char(Cp));
        else if (Cp < 0x800) { S.push_back(char(0xC0 | Cp >> 6)); S.push_back(char(0x80 | (Cp & 0x3F))); }
        else if (Cp < 0x10000) { S.push_back(char(0xE0 | Cp >> 12)); S.push_back(char(0x80 | (Cp >> 6 & 0x3F))); S.push_back(char(0x80 | (Cp & 0x3F))); }
        else
        {
            S.push_back(char(0xF0 | Cp >> 18)); S.push_back(char(0x80 | (Cp >> 12 & 0x3F)));
            S.push_back(char(0x80 | (Cp >> 6 & 0x3F))); S.push_back(char(0x80 | (Cp & 0x3F)));
        }
    }
    return S;
}

/* Archive-form name batch (UnrealNames.cpp). An ANSI name is read back as Latin-1, so a non-ASCII one goes out as
   UTF-16: header high bit set, length in UTF-16 units, and a pad byte first when it would start at an odd offset
   (the loader aligns its string pointer to a UTF16CHAR before reading one). */
void WriteNameBatch(const std::vector<std::string>& Names, std::vector<uint8>& Out)
{
    FBin Batch;
    Batch.U32(uint32(Names.size()));
    if (Names.empty())
    {
        Out.insert(Out.end(), Batch.B.begin(), Batch.B.end());
        return;
    }

    FBin Strings;
    std::vector<uint32> Headers;
    for (const std::string& S : Names)
    {
        if (IsAscii(S))
        {
            Headers.push_back(uint32(S.size()));
            Strings.Raw(S.data(), S.size());
            continue;
        }
        const std::u16string W = Utf8To16(S);
        Headers.push_back(0x8000 | uint32(W.size()));
        if (Strings.B.size() & 1) Strings.U8(0);
        Strings.Raw(W.data(), W.size() * 2);
    }
    Batch.U32(uint32(Strings.B.size()));
    Batch.U64(kUnusableHashVersion);

    for (size_t I = 0; I < Names.size(); ++I) Batch.U64(0);     // hashes, ignored per the version
    for (uint32 H : Headers)
    {
        Batch.U8(uint8(H >> 8));
        Batch.U8(uint8(H & 0xFF));
    }
    Batch.Raw(Strings.B.data(), Strings.B.size());

    Out.insert(Out.end(), Batch.B.begin(), Batch.B.end());
}

void WriteEmptyStore(std::vector<uint8>& Out)
{
    FBin Store;
    Store.U32(kStoreBeginMagic);
    for (int32 I = 0; I < kStoreViewCount; ++I) Store.I32(0);
    Store.I32(0);                                               // FText data block, in bytes
    Store.U32(kStoreEndMagic);
    Out.insert(Out.end(), Store.B.begin(), Store.B.end());
}
}   // namespace

bool SaveAssetRegistry(const std::vector<FRegistryAsset>& Assets, const std::string& OutPath,
                       std::string* Err)
{
    auto Fail = [&](const char* Msg) { if (Err) *Err = Msg; return false; };

    // The body is written first because it is what registers the names the batch must precede it with.
    FBin Body;
    Body.I32(int32(Assets.size()));
    for (const FRegistryAsset& A : Assets)
    {
        Body.Name(A.PackageName + "." + A.AssetName);           // ObjectPath
        Body.Name(PackagePathOf(A.PackageName));                // PackagePath
        Body.Name(A.AssetClass);
        Body.Name(A.PackageName);
        Body.Name(A.AssetName);
        Body.U64(0);                                            // tag map handle: the empty map
        Body.I32(0);                                            // tagged asset bundles
        Body.I32(0);                                            // ChunkIDs
        Body.U32(A.PackageFlags);
    }

    Body.I64(4);                                                // dependency section size: just the count
    Body.I32(0);                                                // dependency node count
    Body.I32(0);                                                // package data count

    std::vector<uint8> File;
    FBin Header;
    for (uint32 V : kVersionGuid) Header.U32(V);
    Header.I32(kVersionFixedTags);
    File.insert(File.end(), Header.B.begin(), Header.B.end());

    WriteNameBatch(Body.Names(), File);
    WriteEmptyStore(File);
    File.insert(File.end(), Body.B.begin(), Body.B.end());

    FILE* F = fopen(OutPath.c_str(), "wb");
    if (!F) return Fail("cannot write AssetRegistry.bin");
    const bool bOk = fwrite(File.data(), 1, File.size(), F) == File.size();
    fclose(F);
    return bOk ? true : Fail("short write on AssetRegistry.bin");
}

bool LoadAssetRegistry(const std::string& Path, std::vector<FRegistryAsset>& Out, std::string* Err)
{
    auto Fail = [&](const char* Msg) { if (Err) *Err = Path + ": " + Msg; return false; };
    FILE* F = fopen(Path.c_str(), "rb");
    if (!F) return Fail("cannot read");
    std::vector<uint8> B;
    uint8 Chunk[65536];
    for (size_t N; (N = fread(Chunk, 1, sizeof(Chunk), F)) > 0;) B.insert(B.end(), Chunk, Chunk + N);
    fclose(F);

    size_t At = 0;
    bool bOk = true;
    auto Take = [&](void* P, size_t Size) {
        if (!bOk || At + Size > B.size()) { bOk = false; return; }
        memcpy(P, B.data() + At, Size);
        At += Size;
    };
    auto U32 = [&] { uint32 V = 0; Take(&V, 4); return V; };
    auto U64 = [&] { uint64 V = 0; Take(&V, 8); return V; };

    for (uint32 V : kVersionGuid) if (U32() != V) return Fail("not an AssetRegistry.bin");
    if (int32(U32()) != kVersionFixedTags) return Fail("not a UE 4.27 AssetRegistry.bin");

    std::vector<std::string> Names(U32());
    if (!Names.empty())
    {
        U32();                                                  // string bytes: the headers say each length
        U64();                                                  // hash algorithm
        for (size_t I = 0; I < Names.size(); ++I) U64();        // hashes
        std::vector<uint32> Headers;
        for (size_t I = 0; I < Names.size(); ++I)
        {
            uint8 H[2] = {};
            Take(H, 2);
            Headers.push_back(uint32(H[0]) << 8 | H[1]);
        }
        const size_t StringsAt = At;
        for (size_t I = 0; I < Names.size() && bOk; ++I)
        {
            const uint32 Len = Headers[I] & 0x7FFF;
            if (!(Headers[I] & 0x8000))
            {
                Names[I].resize(Len);
                Take(&Names[I][0], Len);
                continue;
            }
            if ((At - StringsAt) & 1) ++At;                     // the writer's alignment pad
            std::u16string W(Len, u'\0');
            Take(&W[0], Len * 2);
            Names[I] = Utf16To8(W);
        }
    }

    if (U32() != kStoreBeginMagic) return Fail("a malformed tag store");
    for (int32 I = 0; I <= kStoreViewCount; ++I)                // the views, then the FText block
        if (U32() != 0) return Fail("asset tags - not a registry assetgen wrote, so not one it can merge");
    if (U32() != kStoreEndMagic) return Fail("a malformed tag store");

    auto Name = [&]() -> std::string {
        const uint32 Index = U32();
        const uint32 Base = Index & ~kNumberedNameBit;
        if (!bOk || Base >= Names.size()) { bOk = false; return std::string(); }
        return Index & kNumberedNameBit ? Names[Base] + "_" + std::to_string(int64(U32()) - 1) : Names[Base];
    };
    const uint32 Count = U32();
    for (uint32 I = 0; I < Count && bOk; ++I)
    {
        FRegistryAsset A;
        Name();                                                 // ObjectPath and PackagePath: derived from the rest
        Name();
        A.AssetClass = Name();
        A.PackageName = Name();
        A.AssetName = Name();
        if (U64() != 0 || U32() != 0 || U32() != 0) return Fail("asset tags, bundles or chunks, which assetgen never writes");
        A.PackageFlags = U32();
        Out.push_back(A);
    }
    return bOk ? true : Fail("truncated");
}

bool MergeAssetRegistry(const std::vector<FRegistryAsset>& Assets, const std::string& Path, std::string* Err)
{
    std::vector<FRegistryAsset> Rows;
    if (FILE* F = fopen(Path.c_str(), "rb"))
    {
        fclose(F);
        if (!LoadAssetRegistry(Path, Rows, Err)) return false;
    }
    std::set<std::string> Replaced;
    for (const FRegistryAsset& A : Assets) Replaced.insert(Lower(A.PackageName));
    std::vector<FRegistryAsset> Kept;
    for (const FRegistryAsset& A : Rows)
        if (!Replaced.count(Lower(A.PackageName))) Kept.push_back(A);
    Kept.insert(Kept.end(), Assets.begin(), Assets.end());
    return SaveAssetRegistry(Kept, Path, Err);
}

}   // namespace Uasset
