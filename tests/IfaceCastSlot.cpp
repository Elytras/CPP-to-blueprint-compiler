#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/IfaceCastSlot");

/*
Cast<> to a game interface. For a CLASS_Interface class execDynamicCast writes an FScriptInterface - the object,
then the interface pointer 8 bytes on (ScriptCore.cpp 3605-3645) - through RESULT_PARAM, so its result needs a
16-byte interface slot. The `IHealth*` is the object, so EX_InterfaceToObjCast takes it out of that value before
IsValid's 8-byte object parameter gets it.
*/
class IfaceCastSlot : public AActor {
  AActor *Other;
  int32 Guard = 7;

public:
  int32 Probe() {
    auto H = Cast<IHealth>(Other);
    int32 After = Guard;
    return H ? After : -After;
  }
};
