// An asset's braces lie over its class's default object, and a set or map element the CDO already holds is left out of
// the delta (only an addition or a change is written, a drop is listed as removed). Lowering the braces imported every
// asset they name for the compare, so an element left out still had its package and object in the import table, which no
// tag names and the loader resolves for nothing (test_bytecode.py asset_import_dropped). An asset that names the object
// elsewhere keeps the import.
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetImportDropped");

UE_ASSET_AT(UEnemyDescriptor, ED_A, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");
UE_ASSET_AT(UEnemyDescriptor, ED_B, "/Game/Enemies/Spider/Exploder/ED_Spider_Exploder");

class UAidDef : public UPrimaryDataAsset
{
public:
    TSet<UObject *> Peers = {&ED_A, &ED_B};
    TMap<FName, UObject *> Named = {{"a", &ED_A}, {"b", &ED_B}};
    UObject *Best = nullptr;
};

class UAidKid : public UAidDef
{
public:
    UE_DEFAULTS {
        Peers = {&ED_B};    // C++: this CDO's Peers is {B}: A is listed removed, B is its parent's, so written nowhere
    }
};

// C++: Peers {B}. A is listed removed; B is the CDO's, so it is written nowhere.
UAidDef AS_AidSet = {.Peers = {&ED_B}, .Named = {{"a", &ED_A}, {"b", &ED_B}}};
// C++: Named {"b": B}: "a" is listed removed (a name), B's pair is the CDO's. Nothing names A or B.
UAidDef AS_AidMap = {.Peers = {&ED_A, &ED_B}, .Named = {{"b", &ED_B}}};
// C++: Peers {B} as above, and Best = B: the import stays, a tag names it.
UAidDef AS_AidKeep = {.Peers = {&ED_B}, .Named = {{"a", &ED_A}, {"b", &ED_B}}, .Best = &ED_B};
