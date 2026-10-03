/* AssetOtherShared.h: data asset classes AssetOtherOwner.cpp cooks and AssetOtherUser.cpp makes assets of. Their
   defaults are here, so a mod that includes the header knows each CDO's value of S and M: an asset of the class loads
   over it (test_bytecode.py asset_other_class). UAosNamed is pinned with UE_CLASS, the others with UE_CLASS_IN. */
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
