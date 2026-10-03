// A mod's own asset loads over its class's default object (the CDO is its archetype: SerializeScriptProperties diffs
// against it, Obj.cpp 1450), and a TSet / TMap tag loads as a delta there: the archetype's value copied in, the listed
// removals taken out, the rest added (PropertySet.cpp 285-358, PropertyMap.cpp 316-400). So the asset's braces must list
// the CDO's elements they drop as removed, as the cook writes a game object over its archetype; written whole they load
// as the union of both (test_bytecode.py asset_set_over_cdo). A TArray is replaced whole (PropertyArray.cpp 199).
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetSetOverCdo");

struct FAsocHeld
{
    UE_STRUCT;
    TSet<int32> Ids;
    TMap<FName, int32> Score;
    int32 N = 0;
};

class UAsocDef : public UPrimaryDataAsset
{
public:
    TSet<int32> S = {1, 2};
    TMap<FName, int32> M = {{"a", 1}, {"c", 3}};
    FAsocHeld H = {.Ids = {1, 2}, .Score = {{"a", 1}, {"c", 3}}, .N = 3};
    TArray<int32> A = {1, 2};
};

class UAsocKid : public UAsocDef
{
public:
    UE_DEFAULTS {
        S = {2, 5};             // the kid's CDO holds {2, 5}: its asset's braces lie over that
    }
};

// C++: S {3}, M {a: 5, b: 2}, H.Ids {2, 3}, H.Score {a: 5, b: 2}, A [3].
UAsocDef AS_AsocOver = {.S = {3}, .M = {{"a", 5}, {"b", 2}}, .H = {.Ids = {2, 3}, .Score = {{"a", 5}, {"b", 2}}, .N = 3}, .A = {3}};
// C++: S and M empty.
UAsocDef AS_AsocEmpty = {.S = {}, .M = {}};
// C++: H.Ids and H.Score empty, as FAsocHeld makes them, over the CDO's {1, 2} and {a: 1, c: 3}.
UAsocDef AS_AsocLeft = {.H = {.N = 5}};
// C++: S {5, 6}.
UAsocKid AS_AsocKid = {{.S = {5, 6}}};
