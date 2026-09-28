#pragma once
#include <optional>
#include <string>

namespace Uasset
{
/* One .uasset/.uexp per class in OutDir; with ApiDir, also each class's editor-side stub (Uncooked.h). GameDir is the
   folder /Game is in, in the extracted game pak: where UE_ASSET_EDIT and UE_PATCH read the packages they edit. */
bool CompileToAssets(const std::string& SourcePath, const std::string& IncludeDir,
                     const std::string& OutDir, const std::optional<std::string>& ApiDir, const std::string& GameDir,
                     std::string* Err);

}   // namespace Uasset
