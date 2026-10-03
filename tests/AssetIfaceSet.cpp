// A mod interface's variable is a property of the class that implements it (UeMeta.h's UE_INTERFACE), so that class's
// default object holds the interface's initializer, or its own UE_DEFAULTS value. A subclass's UE_DEFAULTS of an
// inherited set loads over that value, and so does an asset's braces, which give an implemented interface's variables
// in the interface's own braces, after the base's (test_bytecode.py asset_iface_set).
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/AssetIfaceSet");

class IAisTagged
{
public:
    UE_INTERFACE;
    TSet<int32> Tags = {1, 2};
    int32 Level = 4;
    void Touch();
};

class UAisImpl : public UPrimaryDataAsset, public IAisTagged     // its CDO: Tags {1, 2}, its own property
{
public:
    int32 Own = 0;
    void Touch() {}
};

class UAisKid : public UAisImpl
{
public:
    UE_DEFAULTS {
        Tags = {3};     // C++: {3}
    }
};

class UAisImpl5 : public UPrimaryDataAsset, public IAisTagged
{
public:
    UE_DEFAULTS {
        Tags = {5};     // the implementer's own property: {5}
    }
    void Touch() {}
};

class UAisKid6 : public UAisImpl5
{
public:
    UE_DEFAULTS {
        Tags = {6};     // C++: {6}
    }
};

// C++: Tags {3}, Level 9, Own 1.
UAisImpl AS_AisImpl = {{}, {.Tags = {3}, .Level = 9}, 1};
// C++: Tags {7}, over UAisKid's {3}; Level stays 4.
UAisKid AS_AisKid = {{{}, {.Tags = {7}}, 0}};
