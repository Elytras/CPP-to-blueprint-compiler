#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SetToArrayAppend");

/*
A native that appends to its output array. GenericSet_ToArray adds each element with GenericArray_Add and never
empties Result first (BlueprintSetLibrary.cpp 53-70): it relies on the editor, which empties every non-reference,
non-const TArray out parameter of a native call with its own `EX_SetArray <arg> EX_EndArray` statement just ahead of
the call (KismetCompilerVMBackend.cpp 1152-1174). So ToArray must leave Listed holding exactly the set, and a
range-for over a set - a walk over a ToArray() copy (Types.h) - must see the set once per walk, also the second time
round an outer loop that reuses the copy.
*/
class SetToArrayAppend : public AActor {
public:
  TSet<int32> Picks;
  TArray<int32> Listed;

  void List() { Picks.ToArray(Listed); }

  int32 SumTwice() {
    int32 Sum = 0;
    for (int32 Round = 0; Round < 2; ++Round) {
      for (int32 P : Picks) {
        Sum += P;
      }
    }
    return Sum;
  }
};
