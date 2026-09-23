/*
usage: assetgen verify <out-dir> <reference-dir>     rebuilds Autosprint/InitCave and byte-diffs it against the cooked original
       assetgen compile <source.cpp> <include-dir> <out-dir> [--api <api-dir>]
*/
#include <cstdio>
#include <string>

#include "Cpp.h"
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
        if (CompileToAssets(argv[2], argv[3], argv[4], ApiDir, &Err)) return 0;
        printf("  FAILED: %s\n", Err.c_str());
        return 1;
    }

    printf("usage: assetgen verify <out-dir> <reference-dir>\n"
           "       assetgen compile <source.cpp> <include-dir> <out-dir>\n");
    return 2;
}
