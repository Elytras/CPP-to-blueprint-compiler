#include "Registry.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
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

/* FValueId's type, its low 3 bits (AssetDataTagMap.cpp EValueType); the rest is the index into that type's view. */
enum EValueType : uint32 { AnsiString, WideString, NumberlessName, NumberedName, NumberlessPath, NumberedPath, LocalizedText };

bool IsNumberless(const std::string& Name)
{
    std::string Base;
    int32 Number = 0;
    SplitName(Name, Base, Number);
    return Number == 0;
}

/* Every row's tags as one 4.27 tag store, as FStoreBuilder lays it out (AssetDataTagMap.cpp): each distinct value once
   in its type's view, each row's pairs contiguous, in the numberless view when every key of the row is numberless.
   A value spelled <Class>'<Package>.<Object>' is an export path, which is how the cooker stores ParentClass and
   NativeParentClass (measured on FSD/AssetRegistry.bin); any other an ANSI string, or UTF-16 when it is not ASCII. */
class FTagStore
{
public:
    /* The row's FPartialMapHandle::ToInt: numberless bit, pair count, first pair; 0 for no tags. */
    uint64 AddMap(const std::vector<std::pair<std::string, std::string>>& Tags)
    {
        if (Tags.empty()) return 0;
        bool bNumberless = true;
        for (const auto& Tag : Tags) bNumberless &= IsNumberless(Tag.first);
        std::vector<std::pair<std::string, uint32>>& To = Pairs[bNumberless ? 0 : 1];
        const uint64 Handle = uint64(bNumberless) << 63 | uint64(Tags.size()) << 32 | uint64(To.size());
        for (const auto& Tag : Tags) To.push_back({ Tag.first, Value(Tag.second) });
        return Handle;
    }

    /* SaveStore, text-first: the view counts in member order, the (empty) text block, then the rest. */
    void Write(FBin& Out) const
    {
        Out.U32(kStoreBeginMagic);
        for (int32 Count : { 0, 0, int32(Paths[0].size()), int32(Paths[1].size()), 0, int32(AnsiOffsets.size()),
                             int32(WideOffsets.size()), int32(Ansi.size()), int32(Wide.size()),
                             int32(Pairs[0].size()), int32(Pairs[1].size()) })
            Out.I32(Count);
        Out.I32(0);                                             // FText data block, in bytes
        for (const auto& View : Paths)
            for (const FPath& P : View) { Out.Name(P.Class); Out.Name(P.Object); Out.Name(P.Package); }
        for (uint32 O : AnsiOffsets) Out.U32(O);
        for (uint32 O : WideOffsets) Out.U32(O);
        Out.Raw(Ansi.data(), Ansi.size());
        Out.Raw(Wide.data(), Wide.size() * 2);
        for (const auto& View : Pairs)
            for (const auto& Pair : View) { Out.Name(Pair.first); Out.U32(Pair.second); }
        Out.U32(kStoreEndMagic);
    }

private:
    struct FPath
    {
        std::string Class, Object, Package;
        bool operator<(const FPath& O) const { return std::tie(Class, Object, Package) < std::tie(O.Class, O.Object, O.Package); }
    };

    uint32 Value(const std::string& V)
    {
        const size_t Quote = V.find('\'');
        if (Quote != std::string::npos && V.size() > Quote + 2 && V.back() == '\'')
        {
            const std::string Path = V.substr(Quote + 1, V.size() - Quote - 2);
            const size_t Dot = Path.find('.');
            FPath P{ V.substr(0, Quote), Dot == std::string::npos ? "None" : Path.substr(Dot + 1), Path.substr(0, Dot) };
            const int32 View = IsNumberless(P.Class) && IsNumberless(P.Object) && IsNumberless(P.Package) ? 0 : 1;
            auto [It, bNew] = PathIds[View].emplace(P, uint32(Paths[View].size()));
            if (bNew) Paths[View].push_back(P);
            return (View ? NumberedPath : NumberlessPath) | It->second << 3;
        }
        if (IsAscii(V))
        {
            auto [It, bNew] = AnsiIds.emplace(V, uint32(AnsiOffsets.size()));
            if (bNew) { AnsiOffsets.push_back(uint32(Ansi.size())); Ansi.append(V).push_back('\0'); }
            return AnsiString | It->second << 3;
        }
        auto [It, bNew] = WideIds.emplace(V, uint32(WideOffsets.size()));
        if (bNew) { WideOffsets.push_back(uint32(Wide.size())); Wide.append(Utf8To16(V)).push_back(u'\0'); }
        return WideString | It->second << 3;
    }

    std::vector<FPath> Paths[2];                                // numberless, numbered
    std::map<FPath, uint32> PathIds[2];
    std::string Ansi;
    std::vector<uint32> AnsiOffsets;
    std::map<std::string, uint32> AnsiIds;
    std::u16string Wide;
    std::vector<uint32> WideOffsets;
    std::map<std::string, uint32> WideIds;
    std::vector<std::pair<std::string, uint32>> Pairs[2];       // numberless keys, numbered
};
}   // namespace

bool SaveAssetRegistry(const std::vector<FRegistryAsset>& Assets, const std::string& OutPath,
                       std::string* Err)
{
    auto Fail = [&](const char* Msg) { if (Err) *Err = Msg; return false; };

    // The tag store and body are written first because they are what registers the names the batch must precede them with.
    FTagStore Tags;
    std::vector<uint64> Handles;
    for (const FRegistryAsset& A : Assets) Handles.push_back(Tags.AddMap(A.Tags));
    FBin Body;
    Tags.Write(Body);
    Body.I32(int32(Assets.size()));
    for (size_t I = 0; I < Assets.size(); ++I)
    {
        const FRegistryAsset& A = Assets[I];
        Body.Name(A.PackageName + "." + A.AssetName);           // ObjectPath
        Body.Name(PackagePathOf(A.PackageName));                // PackagePath
        Body.Name(A.AssetClass);
        Body.Name(A.PackageName);
        Body.Name(A.AssetName);
        Body.U64(Handles[I]);
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

    auto Name = [&]() -> std::string {
        const uint32 Index = U32();
        const uint32 Base = Index & ~kNumberedNameBit;
        if (!bOk || Base >= Names.size()) { bOk = false; return std::string(); }
        return Index & kNumberedNameBit ? Names[Base] + "_" + std::to_string(int64(U32()) - 1) : Names[Base];
    };

    /* The tag store FTagStore writes, read back to the strings it was given. Texts, which it never writes, are refused. */
    if (U32() != kStoreBeginMagic) return Fail("a malformed tag store");
    uint32 Counts[kStoreViewCount] = {};
    for (uint32& C : Counts) C = U32();
    if (Counts[4] != 0 || U32() != 0) return Fail("localized tags - not a registry assetgen wrote, so not one it can merge");
    std::vector<std::string> Values[4];                         // names, numbered names, paths, numbered paths
    for (int32 V = 0; V < 2; ++V)
        for (uint32 I = 0; I < Counts[V] && bOk; ++I) Values[V].push_back(Name());
    for (int32 V = 2; V < 4; ++V)
        for (uint32 I = 0; I < Counts[V] && bOk; ++I)
        {
            const std::string Class = Name(), Object = Name(), Package = Name();
            const std::string Path = Package + (Object == "None" ? "" : "." + Object);
            Values[V].push_back(Class == "None" ? Path : Class + "'" + Path + "'");
        }
    std::vector<uint32> AnsiOffsets(Counts[5]), WideOffsets(Counts[6]);
    for (uint32& O : AnsiOffsets) O = U32();
    for (uint32& O : WideOffsets) O = U32();
    std::string Ansi(Counts[7], '\0');
    std::u16string Wide(Counts[8], u'\0');
    if (!Ansi.empty()) Take(&Ansi[0], Ansi.size());
    if (!Wide.empty()) Take(&Wide[0], Wide.size() * 2);
    std::vector<std::pair<std::string, std::string>> Pairs[2];  // numberless keys, numbered
    for (int32 V = 0; V < 2; ++V)
        for (uint32 I = 0; I < Counts[9 + V] && bOk; ++I)
        {
            std::string Key = Name();
            const uint32 Id = U32(), Index = Id >> 3;
            std::string Value;
            switch (Id & 7)
            {
            case AnsiString:
                if (Index >= AnsiOffsets.size() || AnsiOffsets[Index] >= Ansi.size()) return Fail("a malformed tag store");
                Value = Ansi.c_str() + AnsiOffsets[Index];
                break;
            case WideString:
                if (Index >= WideOffsets.size() || WideOffsets[Index] >= Wide.size()) return Fail("a malformed tag store");
                Value = Utf16To8(Wide.c_str() + WideOffsets[Index]);
                break;
            case NumberlessName: case NumberedName: case NumberlessPath: case NumberedPath:
            {
                const std::vector<std::string>& From = Values[(Id & 7) - NumberlessName];
                if (Index >= From.size()) return Fail("a malformed tag store");
                Value = From[Index];
                break;
            }
            default:
                return Fail("a malformed tag store");
            }
            Pairs[V].push_back({ std::move(Key), std::move(Value) });
        }
    if (U32() != kStoreEndMagic) return Fail("a malformed tag store");

    const uint32 Count = U32();
    for (uint32 I = 0; I < Count && bOk; ++I)
    {
        FRegistryAsset A;
        Name();                                                 // ObjectPath and PackagePath: derived from the rest
        Name();
        A.AssetClass = Name();
        A.PackageName = Name();
        A.AssetName = Name();
        const uint64 Handle = U64();
        const std::vector<std::pair<std::string, std::string>>& From = Pairs[Handle >> 63 ? 0 : 1];
        const size_t Begin = uint32(Handle), Num = Handle >> 32 & 0xFFFF;
        if (Begin + Num > From.size()) return Fail("a malformed tag store");
        A.Tags.assign(From.begin() + Begin, From.begin() + Begin + Num);
        if (U32() != 0 || U32() != 0) return Fail("asset bundles or chunks, which assetgen never writes");
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
