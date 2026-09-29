#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncStubIface");

/*
An interface's own functions are stubs: an implementer without a function of that name reaches the interface's through
FindFunctionByName, so they must do nothing - no store, no call - whatever they return or take by reference. The
implementer defines one of the three and gets stubs of its own for the other two.
*/
class IStubbed {
public:
  UE_INTERFACE;
  int32 Count(int32 By);
  void  Touch(class AActor *By);
  float Pair(int32 A, int32 &Out);
};

class FuncStubIface : public AActor, public IStubbed {
public:
  int32 Seen = 0;
  int32 Count(int32 By) {
    Seen = Seen + By;
    return Seen;
  }
};
