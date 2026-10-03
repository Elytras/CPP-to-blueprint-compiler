// An FString key compares without case, as FString's == and GetTypeHash do (UnrealString.h 1123, Strihash), so
// {{"a", 5}, {"A", 1}} is one key: TMap's initializer-list constructor Adds each pair in order, and Add replaces the
// whole element, key spelling and value (TSet::Emplace, Set.h 625). The loader finds a key that way too (FindOrAdd,
// PropertyMap.cpp 392; a set's FindElementIndex, PropertySet.cpp 348), keeping the spelling it holds. A delta over a
// default object must be of that one element (test_bytecode.py asset_map_str_case). An FName key ignores case as well.
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetMapStrCase");

class UAmsDef : public UPrimaryDataAsset
{
public:
    TMap<FString, int32> T = {{"A", 1}};
    TSet<FString> U;
    TMap<FName, int32> N = {{"A", 1}};
};

class UAmsKid : public UAmsDef
{
public:
    UE_DEFAULTS {
        T = {{"a", 5}, {"A", 1}};   // C++: the kid's CDO holds {"A": 1}
    }
};

// C++: T {"A": 1}, U {"A"}, N {A: 1}.
UAmsDef AS_AmsDup = {.T = {{"a", 5}, {"A", 1}}, .U = {"a", "A"}, .N = {{"a", 5}, {"A", 1}}};
// C++: T {"a": 1}: the key spelled another way is another element.
UAmsDef AS_AmsSpell = {.T = {{"a", 1}}};
