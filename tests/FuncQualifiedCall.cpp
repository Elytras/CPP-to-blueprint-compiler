#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncQualifiedCall");

/*
A qualified call, `QcParent::Plain()`, is QcParent's function whatever the object is: the editor's parent call is a
final call bound to that UFunction (bIsParentContext, KismetCompilerVMBackend.cpp:1222). From a class that does not
override Plain itself, a call by name still reaches the most derived Plain on an object of a subclass - QcKid's here -
so the call must be bound to QcParent's function. AuthOnly is the same with a routing mark: final, not local.
Plain's body is copied in. AuthOnly's, ServerBump's and Kept's are not (authority-only, an RPC, noinline), nor Plain's
into a UE_NO_OPTIMIZE caller: the editor binds a parent's function only from a class that has one of that name, so
FuncQualifiedCall gets an override of each that forwards to QcParent's, and QcKid's versions override those.
*/
class QcParent : public AActor {
public:
  int32 Seen = 0;
  UE_AUTHORITY_ONLY void AuthOnly() { Seen = 3; }
  UE_SERVER void ServerBump(int32 By) { Seen = Seen + By; }
  [[gnu::noinline]] int32 Kept(int32 V) { return V + 1; }
  int32 Plain() { return 5; }
};

class FuncQualifiedCall : public QcParent {
public:
  void  CallParentAuth() { QcParent::AuthOnly(); }
  void  CallParentServer() { QcParent::ServerBump(4); }
  int32 CallParentKept() { return QcParent::Kept(1); }
  int32 CallParentPlain() { return QcParent::Plain(); }
  UE_NO_OPTIMIZE int32 CallParentPlainSlow() { return QcParent::Plain(); }
  void  CallAuth() { AuthOnly(); }      // unqualified: the object's own AuthOnly
};

class QcKid : public FuncQualifiedCall {
public:
  void  AuthOnly() { Seen = 30; }
  void  ServerBump(int32 By) { Seen = By * 100; }
  int32 Kept(int32 V) { return V + 1000; }
  int32 Plain() { return 50; }
};
