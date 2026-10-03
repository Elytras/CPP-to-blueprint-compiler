// A mod asset's TSet / TMap inside a struct member whose class declares it with no initializer, or as `T()`: the CDO
// has no tag for it, so the CDO holds the UE_STRUCT's default instance there (InitNonNativeProperty initialises the
// property, BlueprintSupport.cpp 2609; UUserDefinedStruct::InitializeStruct copies the default instance in,
// UserDefinedStruct.cpp 254), and the asset's braces load over that: they must list the default instance's elements
// they drop as removed (test_bytecode.py asset_set_fresh). So must a child class's UE_DEFAULTS over its parent's CDO.
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetSetFresh");

struct FAsfHeld
{
    UE_STRUCT;
    TSet<int32> Ids = {7};
    TMap<FName, int32> Score = {{"z", 9}};
    int32 N = 0;
};

class UAsfDef : public UPrimaryDataAsset
{
public:
    FAsfHeld H;                 // the default instance: Ids {7}, Score {z: 9}
    FAsfHeld HV = FAsfHeld();   // the same
};

class UAsfKid : public UAsfDef
{
public:
    UE_DEFAULTS {
        H = {.Ids = {5}, .Score = {{"k", 2}}};    // the kid's CDO holds Ids {5}, Score {k: 2}
    }
};

// C++: H.Ids {3}, H.Score {a: 1}; HV.Ids {8}, HV.Score {a: 1}.
UAsfDef AS_AsfOver = {.H = {.Ids = {3}, .Score = {{"a", 1}}}, .HV = {.Ids = {8}, .Score = {{"a", 1}}}};
// C++: H.Ids and H.Score empty.
UAsfDef AS_AsfEmpty = {.H = {.Ids = {}, .Score = {}}};
