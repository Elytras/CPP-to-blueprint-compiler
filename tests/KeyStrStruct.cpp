// A set or map key that is a struct holding an FString: the engine compares such a key member by member (Identical), so
// its FString without case, as it does a lone FString key (AssetMapStrCase). {FKsKey{"a", 1}, FKsKey{"A", 1}} is one key,
// the last given, in a class's own value and in a UE_DEFAULTS delta over its parent's (test_bytecode.py key_str_struct).
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/KeyStrStruct");

struct FKsKey
{
    UE_STRUCT;
    FString S;
    int32 N = 0;
};

class UKsDef : public UPrimaryDataAsset
{
public:
    TMap<FKsKey, int32> M = {{FKsKey{"a", 1}, 5}, {FKsKey{"A", 1}, 1}};   // C++: {{"A", 1}: 1}
    TSet<FKsKey> U = {FKsKey{"a", 1}, FKsKey{"A", 1}};                  // C++: {{"A", 1}}
};

class UKsBase : public UPrimaryDataAsset
{
public:
    TMap<FKsKey, int32> M = {{FKsKey{"A", 1}, 1}};
    TSet<FKsKey> U = {FKsKey{"A", 1}};
};

class UKsKid : public UKsBase
{
public:
    UE_DEFAULTS {
        M = {{FKsKey{"a", 1}, 5}, {FKsKey{"A", 1}, 1}};   // C++: {{"A", 1}: 1}, the parent's value as it is
        U = {FKsKey{"a", 1}, FKsKey{"A", 1}};             // C++: {{"A", 1}}
    }
};
