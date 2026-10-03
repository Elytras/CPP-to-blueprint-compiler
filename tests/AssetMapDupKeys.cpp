// A key the braces give twice holds the last value given: TMap's initializer-list constructor Adds each pair in order
// (Map.h 1166-1173). An asset's map is written as a delta over its class's CDO's, and so is a child's UE_DEFAULTS over
// its parent's: the delta must be of that last value, not of each pair on its own (test_bytecode.py asset_map_dup_keys).
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetMapDupKeys");

class UAmdDef : public UPrimaryDataAsset
{
public:
    TMap<FName, int32> M = {{"a", 2}, {"c", 3}};
    TSet<int32> S = {1, 2};
};

class UAmdKid : public UAmdDef
{
public:
    UE_DEFAULTS {
        M = {{"a", 5}, {"a", 2}};   // C++: the kid's CDO holds {a: 2}
    }
};

// C++: M {a: 2}, S {3}.
UAmdDef AS_AmdOver = {.M = {{"a", 1}, {"a", 2}}, .S = {3, 3}};
// C++: M {c: 3}.
UAmdDef AS_AmdLast = {.M = {{"c", 7}, {"c", 3}}};
