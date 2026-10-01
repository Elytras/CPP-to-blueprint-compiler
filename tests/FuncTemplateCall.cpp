#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncTemplateCall");

/*
A member template's body is copied into each caller, as an inline method's is, and what it names is read in the class
it is written in. `AuthOnly()` in Helper is unqualified, a call by name from wherever it is copied: on an FtKid it runs
FtMid's override (7), as C++ does, and FtKid, which declares no AuthOnly, needs no function of that name.
`FuncTemplateCall::AuthOnly()` in HelperQ names FuncTemplateCall's (3), on an FtQKid too, below FtMid's override.
*/
class FuncTemplateCall : public AActor {
public:
  int32 Seen = 0;
  UE_AUTHORITY_ONLY void AuthOnly() { Seen = 3; }
  template <typename T> void Helper(T V) { AuthOnly(); }
  template <typename T> void HelperQ(T V) { FuncTemplateCall::AuthOnly(); }
  inline void Plain() { AuthOnly(); }
};

class FtMid : public FuncTemplateCall {
public:
  void AuthOnly() { Seen = 7; }
};

class FtKid : public FtMid {
public:
  void Use() { Helper(1); }
  void UsePlain() { Plain(); }
};

class FtQKid : public FtMid {
public:
  void UseQ() { HelperQ(1); }
};
