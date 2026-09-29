#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TypingIface");

/*
Casts between objects and interfaces, each stepping its operand at the width the VM reads it: an object into a
UObject* (EX_ObjToInterfaceCast, CST_ObjectToBool), an interface into an FScriptInterface (EX_CrossInterfaceCast,
EX_InterfaceToObjCast, CST_InterfaceToBool) - ScriptCore.cpp 3677-3767. An object that does not implement the
interface casts to an empty one; `(bool)I` is true when an object is behind the interface (GetObject() != null).
*/
class ITypingMark {
public:
  UE_INTERFACE;
  int32 Mark();
};

class ITypingMore : public ITypingMark {
public:
  UE_INTERFACE;
  int32 More();
};

class TypingIface : public AActor, public ITypingMore {
  TScriptInterface<ITypingMark> Held;

public:
  int32 Mark() { return 3; }
  int32 More() { return 4; }

  bool Has(AActor *A) {
    TScriptInterface<ITypingMark> I = A;
    return (bool)I;
  }

  AActor *Back(AActor *A) {
    TScriptInterface<ITypingMark> I = A;
    return Cast<AActor>(I.GetObject());
  }

  bool Cross(AActor *A) {
    TScriptInterface<ITypingMark> I = A;
    TScriptInterface<ITypingMore> M = I;
    return (bool)M;
  }

  void Keep(AActor *A) { Held = A; }

  int32 MarkOf(AActor *A) {
    TScriptInterface<ITypingMark> I = A;
    return I ? I->Mark() : -1;
  }
};
