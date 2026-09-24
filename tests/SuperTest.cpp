#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SuperTest");

/*
An override that still wants its parent's implementation says `SuperBase::Bump(...)`, the class it derives from
spelled out - C++ has no `Super`. It is the editor's "Add call to parent function": a final call on the parent's
own function. Called by name it would find the most derived Bump, which is the one making the call.
*/
class SuperBase : public AActor {
public:
  int32 Count;
  void  ReceiveBeginPlay() { Count = 1; }
  int32 Bump(int32 By) {
    Count = Count + By;
    return Count;
  }
  int32 Twice(int32 By) { return Bump(By) + Bump(By); }
};

class SuperTest : public SuperBase {
public:
  void ReceiveBeginPlay() {
    SuperBase::ReceiveBeginPlay();
    Count = Count + 10;
  }
  int32 Bump(int32 By) { return SuperBase::Bump(By * 2); }
  /* Twice is inherited and not redeclared here, so this is an ordinary call by name: a subclass's would run. */
  int32 Thrice(int32 By) { return Twice(By) + Bump(By); }
};
