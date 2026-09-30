#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TypingArrNull");

/*
A container method on a container another object holds, with that object null. KismetArrayLibrary's Array_* are
CustomThunks that find their container through Stack.MostRecentProperty; on a null holder there is none, and the
thunk sets bArrayContextFailed and returns with its other arguments unread (KismetArrayLibrary.h 278-286). Only
ProcessContextOpcode rewinds and skips the call (ScriptCore.cpp 2896-2902), which is why the editor calls these inside
an EX_Context on the library's default object and never with EX_CallMath (KismetCompilerVMBackend.cpp 1222-1231).
Called with EX_CallMath, the VM goes on at the unread argument list as statements, and EX_EndFunctionParms, whose
handler steps the code back onto itself (ScriptCore.cpp 2366-2370), never lets it leave: the game hangs. Poke must log
Accessed None and still set Done.
*/
class TypingArrNull : public AActor {
  TypingArrNull *Other;
  TArray<int32>  Items;
  int32          Done;

public:
  void Poke() {
    Other->Items.Add(1);
    Done = 1;
  }
};
