/*
usage: assetgen verify <out-dir> <reference-dir>     rebuilds Autosprint/InitCave and byte-diffs it against the cooked original
       assetgen compile <source.cpp> <include-dir> <out-dir> [--api <api-dir>]
       assetgen registry <out AssetRegistry.bin> <AssetRegistry.bin>...   merges registries assetgen wrote into one
*/
#include <cstdio>
#include <string>

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
        for (int I = 5; I + 1 < argc; ++I)
            if (std::string(argv[I]) == "--api") ApiDir = argv[I + 1];

        std::string Err;
        try { if (CompileToAssets(argv[2], argv[3], argv[4], ApiDir, &Err)) return 0; }
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

    printf("usage: assetgen verify <out-dir> <reference-dir>\n"
           "       assetgen compile <source.cpp> <include-dir> <out-dir> [--api <api-dir>]\n"
           "       assetgen registry <out AssetRegistry.bin> <AssetRegistry.bin>...\n");
    return 2;
}
