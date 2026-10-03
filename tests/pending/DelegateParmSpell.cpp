/* DelegateParmSpell: one delegate type spelled two ways, and delegate values set through another object. clang keeps
   each spelling - `TDelegate<void(int32)>` in DelegateParmSpellTop, `TDelegate<void(int)>` in the overrides of Use, in
   Take's out-of-line definition and in the operator= it declares for `Held = D` - but they are one C++ type, so one
   signature function of DelegateParmSpellTop's: the overrides' parameters and the values handed to Take import it,
   whichever class is generated first (DelegateParmSpell sorts before its parent, DelegateParmSpellZKid after). A value
   bound on another object and assigned to a variable through another object (`Peer->Held = {H, ...}`) is typed with
   the variable's signature, as a Create Event wired to a Set node with its Target wired is (K2Node_CreateDelegate.cpp
   317-331); C++ binds it before `GetPeer()->Held` is located, so on the H that GetPeer replaces. */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DelegateParmSpell");

class DelegateParmSpellHelper : public AActor {
public:
  int32 Got = 0;
  void PingI(int32 X) { Got += X; }
};

class DelegateParmSpellSib : public AActor {
public:
  TDelegate<void(int)> SibVar;
};

class DelegateParmSpellTop : public AActor {
public:
  DelegateParmSpellHelper *H = nullptr;
  DelegateParmSpellTop *Peer = nullptr;
  TDelegate<void(int32)> Held;
  virtual void Use(TDelegate<void(int32)> D) { Held = D; }
  void Take(TDelegate<void(int32)> T);
};
void DelegateParmSpellTop::Take(TDelegate<void(int)> T) { Held = T; }

class DelegateParmSpell : public DelegateParmSpellTop {
public:
  DelegateParmSpellHelper *H2 = nullptr;
  DelegateParmSpellSib *S = nullptr;
  void Use(TDelegate<void(int)> D) override { Held = D; }
  DelegateParmSpellTop *GetPeer() { H = H2; return Peer; }
  void CallTake() { Take({H, &DelegateParmSpellHelper::PingI}); }
  void SetPeer() { Peer->Held = {H, &DelegateParmSpellHelper::PingI}; }
  void SetGotPeer() { GetPeer()->Held = {H, &DelegateParmSpellHelper::PingI}; }
  void SetSib() { S->SibVar = {H, &DelegateParmSpellHelper::PingI}; }
};

class DelegateParmSpellZKid : public DelegateParmSpellTop {
public:
  void Use(TDelegate<void(int)> D) override { Held = D; }
  void CallTake() { Peer->Take({H, &DelegateParmSpellHelper::PingI}); }
  void SetPeer() { Peer->Held = {H, &DelegateParmSpellHelper::PingI}; }
};
