/* AssetOtherShared.h: data asset classes AssetOtherOwner.cpp cooks and AssetOtherUser.cpp makes assets of. Their
   defaults are here, so a mod that includes the header knows each CDO's value of S and M: an asset of the class loads
   over it (test_bytecode.py asset_other_class). UAosNamed and AosHand::UAosHand are pinned with UE_CLASS, the others
   with UE_CLASS_IN. UAosHand's S has no initializer, which a game Blueprint's header has neither, so what the CDO
   holds is unknown to a mod that includes the header: an asset of it warns. */
#pragma once
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

class UAosDef : public UPrimaryDataAsset {
public:
  UE_CLASS_IN("/Game/_ElytrasMods/AssetOtherOwner");
  TSet<int32> S = {1, 2};
  TMap<FName, int32> M = {{"a", 1}, {"c", 3}};
};

class UAosKid : public UAosDef {
public:
  UE_CLASS_IN("/Game/_ElytrasMods/AssetOtherOwner");
  UE_DEFAULTS {
    S = {2, 5};   // the kid's CDO holds {2, 5}
  }
};

class UAosNamed : public UPrimaryDataAsset {
public:
  UE_CLASS("/Game/_ElytrasMods/AssetOtherOwner/UAosNamed", "UAosNamed_C");
  TSet<int32> S = {1, 2};
};

namespace AosHand {
class UAosHand : public UPrimaryDataAsset {
public:
  UE_CLASS("/Game/_ElytrasMods/AssetOtherOwner/AosHand/UAosHand", "UAosHand_C");
  TSet<int32> S;
};
} // namespace AosHand
