// A TDelegate value a mod asset's braces give: `{}`, unbound, the one value a default can hold (a bound one is refused),
// in a member, as a TMap's value, and in a mod interface's variable, which the implementing class holds. Its tag is an
// FScriptDelegate, the object and then the function's name (ScriptDelegates.h 138-142), 12 bytes. AssetGen wrote none
// of it, so the loader read the bytes after it as the delegate; and the asset's property took the signature function
// of the class built last, an export of that class's package, which the asset's preload table then listed as its own
// (test_bytecode.py asset_delegate_value).
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetDelegateValue");

class IAdvHas
{
public:
    UE_INTERFACE;
    TDelegate<void()> OnIt;
    void Touch();
};

class UAdvDef : public UPrimaryDataAsset
{
public:
    TDelegate<void()> D;
    TSet<int32> S = {1, 2};
    TMap<FString, TDelegate<void()>> M = {{"A", {}}};
};

class UAdvIfaceData : public UPrimaryDataAsset, public IAdvHas      // generated last: its OnIt's signature was the stale one
{
public:
    int32 Own = 0;
    void Touch() {}
};

// C++: D unbound, S {3}, M {"a": unbound, "B": unbound}: "a" is "A" to FString's ==, but the braces are M's whole value.
UAdvDef AS_AdvOver = {.D = {}, .S = {3}, .M = {{"a", {}}, {"B", {}}}};
// C++: OnIt unbound, Own 1.
UAdvIfaceData AS_AdvIface = {{}, {.OnIt = {}}, 1};
