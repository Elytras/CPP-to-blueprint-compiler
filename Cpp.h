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

/* assetgen astcheck: proves on one AST dump that the filter in front of the compile's parser changes nothing. The
   filter's output must not depend on where the dump is cut, and must parse to the same tree as the dump itself. The
   dump is the one clang writes for SourcePath with a compile's own command (AstCheck), or a saved or hand-written
   file (AstCheckDump). Prints `same (<values> values, <raw> -> <filtered> bytes)` or the first difference, and
   returns the exit code: 0 only for same. About 1 GB for an FSD.h mod, so run one at a time. */
int AstCheck(const std::string& SourcePath, const std::string& IncludeDir);
int AstCheckDump(const std::string& DumpPath);

}   // namespace Uasset
