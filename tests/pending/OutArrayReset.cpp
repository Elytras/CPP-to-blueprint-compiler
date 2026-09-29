#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/OutArrayReset");

/*
A native's output array. Before calling a FUNC_Native function the editor empties every TArray parameter that is an
out parameter and not a reference or const one, with an `EX_SetArray <arg> EX_EndArray` statement of its own
(KismetCompilerVMBackend.cpp 1152-1174: "in case the native function doesn't clear them before filling"). A native
written against that contract appends: Found must hold what the call found, not also what Run added before it.
*/
class OutArrayReset : public AActor {
public:
  TArray<AActor *> Found;

  void Run() {
    Found.Add(this);
    UGameplayStatics::GetAllActorsOfClass(this, AActor::StaticClass(), Found);
  }
};
