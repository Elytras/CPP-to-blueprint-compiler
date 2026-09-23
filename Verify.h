#pragma once
#include <string>

namespace Uasset
{
/* Rebuilds Autosprint/InitCave by hand and byte-diffs it against the cooked original in RefDir: the package
   writer's regression check. 0 when both .uasset and .uexp match. */
int VerifyInitCave(const std::string& OutDir, const std::string& RefDir);

}   // namespace Uasset
