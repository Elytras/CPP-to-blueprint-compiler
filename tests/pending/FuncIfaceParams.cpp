#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncIfaceParams");

/*
An interface implementation whose parameters differ from the interface function's. Interface calls find the target by
name (FindFunctionChecked / EX_VirtualFunction) and step or copy the arguments by the interface's layout into this
function's frame: a native Execute_Score passes one int32 where this reads a float and an int32 past it. The compiler
must refuse it, naming Score, or give the implementation the interface function's parameter block.
*/
class IScoredParams {
public:
  UE_INTERFACE;
  int32 Score(int32 Times);
};

class FuncIfaceParams : public AActor, public IScoredParams {
public:
  int32 Score(float Times, int32 Extra) { return Extra; }
};
