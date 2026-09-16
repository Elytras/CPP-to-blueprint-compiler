#pragma once
#include <string>

namespace Uasset
{
/* Compiles SourcePath (clang, against IncludeDir/UeApi.h) and writes one .uasset/.uexp per class into OutDir. */
bool CompileToAssets(const std::string& SourcePath, const std::string& IncludeDir,
                     const std::string& OutDir, std::string* Err);

}   // namespace Uasset
