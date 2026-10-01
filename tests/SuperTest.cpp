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
  /* Expanded where it is called, SuperTest too: its Bump is still a call by name there, reaching SuperTest's. */
  inline int32 TwiceInline(int32 By) { return Bump(By) + Bump(By); }
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
  int32 ViaInline(int32 By) { return TwiceInline(By); }
};

/* `= 0` is an empty function that returns the default: the super of a subclass's version, and what a call by name
   finds on an object without one. A class that declares one, or inherits one with no version of its own, is
   cooked Abstract, as clang calls it abstract. */
class PureBase : public AActor {
public:
  virtual int32 Pure(int32 V) = 0;
  virtual void  Touch() = 0;
  int32         UsePure(int32 V) { return Pure(V) * 10; }
};

class PureMid : public PureBase {
public:
  void Touch() override {}
};

class PureKid : public PureMid {
public:
  int32 Pure(int32 V) override { return V + 3; }
};

/* An interface's `= 0` leaves no class Abstract: one that leaves it out gets the interface stub. */
class IPokable {
public:
  UE_INTERFACE;
  virtual int32 Poke() = 0;
};

class PokeLess : public AActor, public IPokable {};
