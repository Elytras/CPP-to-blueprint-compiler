#pragma once
#include <optional>
#include <string>

namespace Uasset
{
/* One .uasset/.uexp per class in OutDir; with ApiDir, also each class's editor-side stub (Uncooked.h). */
bool CompileToAssets(const std::string& SourcePath, const std::string& IncludeDir,
                     const std::string& OutDir, const std::optional<std::string>& ApiDir, std::string* Err);

}   // namespace Uasset
