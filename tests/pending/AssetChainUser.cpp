// Assets and a subclass of another mod's class (AssetChainShared.h, cooked by AssetChainOwner.cpp) whose UE_DEFAULTS
// sets an engine class's map: its CDO holds the engine's pairs too, which no header says, so each here warns, naming
// the member; o, the pair the header says, is listed as removed (test_bytecode.py asset_chain_unsaid).
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

#include "AssetChainShared.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetChainUser");

class UAcuLocal : public UAcsNodes {
public:
  int32 X = 0;
};

class UAcuKid : public UAcsNodes {
public:
  UE_DEFAULTS {
    SourceToTarget = {{"k", "z"}};   // C++: {k: z}, over UAcsNodes's CDO
  }
};

// C++: {u: y} each.
UAcsNodes AS_AcuNodes = {{.SourceToTarget = {{"u", "y"}}}};
UAcuLocal AS_AcuLocal = {{{.SourceToTarget = {{"u", "y"}}}}};
