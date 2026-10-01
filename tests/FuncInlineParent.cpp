#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncInlineParent");

/*
`IpBase::AuthOnly()` in an inline method runs IpBase's AuthOnly in C++, whatever the object. The body is copied into
each caller, so the call is made from the class compiled: FuncInlineParent's Use, IpKid's UseKid. Bound to IpBase's
function, it is the editor's call to a parent function, which only a class with its own AuthOnly makes; so each class
the body is copied into gets an override of AuthOnly forwarding to its parent's, as a qualified call written in the
class itself does (FuncQualifiedCall). IpMid, between, has an AuthOnly of its own; IpDirect has none between, and
IpDirectKid overrides AuthOnly, which a call by name would run.
*/
class IpBase : public AActor {
public:
  int32 Seen = 0;
  UE_AUTHORITY_ONLY void AuthOnly() { Seen = 3; }
};

class IpMid : public IpBase {
public:
  void AuthOnly() { Seen = 7; }
};

class FuncInlineParent : public IpMid {
public:
  inline void Helper() { IpBase::AuthOnly(); }
  void Use() { Helper(); }
};

class IpKid : public FuncInlineParent {
public:
  void UseKid() { Helper(); }
};

class IpDirect : public IpBase {
public:
  inline void Helper2() { IpBase::AuthOnly(); }
  void Use2() { Helper2(); }
};

class IpDirectKid : public IpDirect {
public:
  void AuthOnly() { Seen = 70; }
  void UseKid2() { Helper2(); }
};
