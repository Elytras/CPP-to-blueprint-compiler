// A TDelegate parameter of a mod interface's function: the interface makes its signature function, as a class does,
// so the parameter names an export of the interface's own package, which the implementer imports; a caller through
// the interface binds values to it (test_bytecode.py iface_delegate).
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/IfaceDelegate");

class IIfdSchedules
{
public:
    UE_INTERFACE;
    void Schedule(TDelegate<void()> Call);
    int32 Count(TDelegate<void(int32)> Each, int32 N);
};

class AIfdClock : public AActor, public IIfdSchedules
{
public:
    TDelegate<void()> Saved;
    void Schedule(TDelegate<void()> Call) { Saved = Call; }
    int32 Count(TDelegate<void(int32)> Each, int32 N) { return N + 1; }
};

class AIfdCaller : public AActor
{
public:
    TScriptInterface<IIfdSchedules> Target;
    int32 Pings = 0;
    void Ping() { Pings++; }
    void Each(int32 I) { Pings += I; }
    void Go() { Target->Schedule({this, &AIfdCaller::Ping}); Pings = Target->Count({this, &AIfdCaller::Each}, 2); }
};
