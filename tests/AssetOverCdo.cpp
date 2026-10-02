// A mod's own asset starts as its class's default object (its archetype: FObjectInitializer::InitProperties copies it
// in, UObjectGlobals.cpp 2999), so its braces lie over the CDO's values, not over a fresh value of each type. A struct
// member the asset's braces leave out keeps the CDO's value; where C++ would make it the engine's and the CDO holds
// another (a member the class's braces give), it cannot be left untagged (test_bytecode.py asset_over_cdo).
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetOverCdo");

struct FAocHeld
{
    UE_STRUCT;
    FHitResult Hit = {.Time = 0.5f};
    int32 N = 0;
};

struct FAocFreshHeld
{
    UE_STRUCT;
    FHitResult Hit;
    int32 N = 0;
};

class UAocDef : public UPrimaryDataAsset
{
public:
    FAocHeld H = {.N = 2};          // H.Hit is its initializer's: Time 0.5, the rest the engine's
    FAocFreshHeld F;                // fresh: F.Hit is the engine's
    int32 K = 4;
};

// C++: H.Hit is Time 1, Distance 5, the rest the engine's, which the CDO's H.Hit holds too; H.N is 0 over the CDO's 2;
// F.Hit is Distance 3 over the engine's; K is 0 over the CDO's 4.
UAocDef AS_AocKeep = {.H = {.Hit = {.Time = 1.0f, .Distance = 5.0f}}, .F = {.Hit = {.Distance = 3.0f}}, .K = 0};
