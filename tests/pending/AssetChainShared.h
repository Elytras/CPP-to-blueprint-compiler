/* AssetChainShared.h: a class AssetChainOwner.cpp cooks and AssetChainUser.cpp makes assets and subclasses of. Its
   UE_DEFAULTS sets a map it inherits from an engine class, whose default object's value no header says: the owner
   writes that statement whole over the engine's value, with a warning, so the CDO holds the engine's pairs as well as
   {o: x}. An asset of it, a class below it and that class's UE_DEFAULTS load over that CDO, so each warns too; the
   pairs the header does say are still listed as removed (test_bytecode.py asset_chain_unsaid). */
#pragma once
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

class UAcsNodes : public UNodeMappingContainer {
public:
  UE_CLASS_IN("/Game/_ElytrasMods/AssetChainOwner");
  UE_DEFAULTS {
    SourceToTarget = {{"o", "x"}};
  }
};
