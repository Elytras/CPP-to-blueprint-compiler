#pragma once
/*
Cpp.h — the front end: a mod source in, cooked packages out.

The builder API underneath this file takes strings, which means a misspelled function name or a
base class that does not exist produces an asset that is structurally valid and behaviourally
broken. The fix is not a better builder, it is a compiler: the mod is written as real C++
against declarations in ue/UeApi.h, clang type-checks it, and the generator reads clang's AST
rather than trusting the author.

So the parser is clang's (`-Xclang -ast-dump=json`), not ours. Everything here is the walk from
that JSON to the package writer, plus the refusal to guess: any construct outside the supported
subset is reported by its clang node kind instead of being silently skipped.

Supported subset, deliberately: a class deriving from a declared base; methods with
float/int/bool parameters overriding an event on an ancestor; statements that are a call to a
static library function, optionally used as the target of one member call.
*/
#include <string>

namespace Uasset
{
/*
Compiles SourcePath and writes one .uasset/.uexp pair per class it declares into OutDir.

IncludeDir is where UeApi.h lives. Returns false and fills Err on a clang diagnostic, an
unsupported construct, or a write failure — the first two being the whole reason this exists.
*/
bool CompileToAssets(const std::string& SourcePath, const std::string& IncludeDir,
                     const std::string& OutDir, std::string* Err);

}   // namespace Uasset
