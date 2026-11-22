/*
usage: assetgen verify <out-dir> <reference-dir>     rebuilds Autosprint/InitCave and byte-diffs it against the cooked original
       assetgen compile <source.cpp> <include-dir> <out-dir> [--api <api-dir>] [--game <folder /Game is in>]
       assetgen registry <out AssetRegistry.bin> <AssetRegistry.bin>...   merges registries assetgen wrote into one
       assetgen registry-rows <out AssetRegistry.bin> <rows.tsv>   merges in "<package>\t<asset>\t<class>[\t<key>=<value>...]" rows
       assetgen roundtrip <dir>      reads every cooked package under dir and writes it back in memory: the S38 gate
       assetgen astcheck <source.cpp> <include-dir>   checks that the AST dump filter changes nothing on source's dump
       assetgen astcheck --dump <file.json>            ... on a saved dump
*/
#include <cstdio>
#include <string>

#include "Cooked.h"
#include "Cpp.h"
#include "Registry.h"
#include "Verify.h"

using namespace Uasset;

int main(int argc, char** argv)
{
    if (argc >= 4 && std::string(argv[1]) == "verify")
        return VerifyInitCave(argv[2], argv[3]);

    if (argc >= 5 && std::string(argv[1]) == "compile")
    {
        std::optional<std::string> ApiDir;
        std::string GameDir;        // where UE_ASSET_EDIT / UE_PATCH read the game's packages: <extracted pak>/FSD/Content
        for (int I = 5; I + 1 < argc; ++I)
            if (std::string(argv[I]) == "--api") ApiDir = argv[I + 1];
            else if (std::string(argv[I]) == "--game") GameDir = argv[I + 1];

        std::string Err;
        try { if (CompileToAssets(argv[2], argv[3], argv[4], ApiDir, GameDir, &Err)) return 0; }
        catch (const std::exception& E) { Err = std::string("internal error: ") + E.what(); }   // not a silent 0xC0000409
        printf("  FAILED: %s\n", Err.c_str());
        return 1;
    }

    if (argc >= 4 && std::string(argv[1]) == "registry")
    {
        /* One pak, one registry: bpbuild folds in the registry of each dependency it embeds. */
        std::string Err;
        for (int I = 3; I < argc; ++I)
        {
            std::vector<FRegistryAsset> Rows;
            if (!LoadAssetRegistry(argv[I], Rows, &Err) || !MergeAssetRegistry(Rows, argv[2], &Err))
            {
                printf("  FAILED: %s\n", Err.c_str());
                return 1;
            }
        }
        return 0;
    }

    if (argc >= 4 && std::string(argv[1]) == "registry-rows")
    {
        /* Rows for packages a pak ships as they were cooked (bpbuild's `kept` and `bundle`), which no compile wrote:
           one "<package>\t<asset>\t<class>" line each, then any "\t<key>=<value>" columns as its tags (a kept
           material's MaterialDomain or Parent), merged into the registry as a compile's rows are. */
        std::vector<FRegistryAsset> Rows;
        FILE* F = fopen(argv[3], "rb");
        if (!F)
        {
            printf("  FAILED: cannot read %s\n", argv[3]);
            return 1;
        }
        char Line[4096];
        while (fgets(Line, sizeof(Line), F))
        {
            std::string L(Line);
            while (!L.empty() && (L.back() == '\n' || L.back() == '\r')) L.pop_back();
            std::vector<std::string> Cols;
            for (size_t At = 0;;)
            {
                const size_t Tab = L.find('\t', At);
                Cols.push_back(L.substr(At, Tab == std::string::npos ? std::string::npos : Tab - At));
                if (Tab == std::string::npos) break;
                At = Tab + 1;
            }
            if (Cols.size() < 3) continue;
            FRegistryAsset Row{ Cols[0], Cols[1], Cols[2] };
            for (size_t I = 3; I < Cols.size(); ++I)
            {
                const size_t Eq = Cols[I].find('=');
                if (Eq != std::string::npos)
                    Row.Tags.push_back({ Cols[I].substr(0, Eq), Cols[I].substr(Eq + 1) });
            }
            Rows.push_back(Row);
        }
        fclose(F);
        std::string Err;
        if (!MergeAssetRegistry(Rows, argv[2], &Err))
        {
            printf("  FAILED: %s\n", Err.c_str());
            return 1;
        }
        return 0;
    }

    if (argc >= 3 && std::string(argv[1]) == "roundtrip")
        return RoundTrip(argv[2]);

    if (argc >= 4 && std::string(argv[1]) == "astcheck")
    {
        try { return std::string(argv[2]) == "--dump" ? AstCheckDump(argv[3]) : AstCheck(argv[2], argv[3]); }
        catch (const std::exception& E) { printf("  FAILED: internal error: %s\n", E.what()); }
        return 1;
    }

    printf("usage: assetgen verify <out-dir> <reference-dir>\n"
           "       assetgen compile <source.cpp> <include-dir> <out-dir> [--api <api-dir>] [--game <folder /Game is in>]\n"
           "       assetgen registry <out AssetRegistry.bin> <AssetRegistry.bin>...\n"
           "       assetgen registry-rows <out AssetRegistry.bin> <rows.tsv>\n"
           "       assetgen roundtrip <dir>\n"
           "       assetgen astcheck <source.cpp> <include-dir>\n"
           "       assetgen astcheck --dump <file.json>\n");
    return 2;
}
