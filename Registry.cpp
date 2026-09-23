#include "Registry.h"

#include <cstdio>
#include <unordered_map>

#include "Package.h"

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

/* Archive-form name batch (UnrealNames.cpp): strings packed with no alignment padding. */
void WriteNameBatch(const std::vector<std::string>& Names, std::vector<uint8>& Out)
{
    FBin Batch;
    Batch.U32(uint32(Names.size()));
    if (Names.empty())
    {
        Out.insert(Out.end(), Batch.B.begin(), Batch.B.end());
        return;
    }

    uint32 StringBytes = 0;
    for (const std::string& S : Names) StringBytes += uint32(S.size());
    Batch.U32(StringBytes);
    Batch.U64(kUnusableHashVersion);

    for (size_t I = 0; I < Names.size(); ++I) Batch.U64(0);     // hashes, ignored per the version
    for (const std::string& S : Names)
    {
        const uint32 Len = uint32(S.size());
        Batch.U8(uint8(Len >> 8));                              // high bit set would mean UTF-16
        Batch.U8(uint8(Len & 0xFF));
    }
    for (const std::string& S : Names) Batch.Raw(S.data(), S.size());

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

}   // namespace Uasset
