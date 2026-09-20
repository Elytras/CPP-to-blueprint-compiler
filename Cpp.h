#pragma once
#include <string>

namespace Uasset
{
/* Compiles SourcePath (clang, against IncludeDir/UeApi.h) and writes one .uasset/.uexp per class into OutDir.
   With a non-empty ApiDir, each class also gets its uncooked editor-side stub there (Uncooked.h). */
bool CompileToAssets(const std::string& SourcePath, const std::string& IncludeDir,
                     const std::string& OutDir, const std::string& ApiDir, std::string* Err);

}   // namespace Uasset
