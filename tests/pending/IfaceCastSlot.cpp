#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/IfaceCastSlot");

/*
Cast<> to a game interface. For a CLASS_Interface class execDynamicCast writes an FScriptInterface - the object,
then the interface pointer 8 bytes on (ScriptCore.cpp 3605-3645) - through RESULT_PARAM, so its result needs a
16-byte interface slot. Here it lands in IsValid's 8-byte object parameter, and the second half past it
(REFERENCE.md: "whether it overruns has not been checked").
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
