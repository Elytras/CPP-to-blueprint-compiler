// A delegate value stored into a mod interface's variable through an implementer, `Impl->OnIt = {H, &F}`: the
// implementing class holds OnIt as its own property, so the value is typed with that property's signature function,
// imported from the implementer, as the editor's Set node on it would be (test_bytecode.py iface_delegate_store).
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/IfaceDelegateStore");

class IIdsHas
{
public:
    UE_INTERFACE;
    TDelegate<void()> OnIt;
    void Touch();
};

class IdsHelper : public AActor
{
public:
    void Ping() {}
};

class IdsImpl : public AActor, public IIdsHas
{
public:
    void Touch() {}
};

class IfaceDelegateStore : public AActor
{
public:
    IdsImpl* Impl = nullptr;
    IdsHelper* H = nullptr;
    void Store() { Impl->OnIt = {H, &IdsHelper::Ping}; }
};
