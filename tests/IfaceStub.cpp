#include "UeApi/Types.h"

#include "UeApi/Engine.h"

#include "../include/Objects.h" // NewObject, by its path from this file

UE_MOD_PACKAGE("/Game/_ElytrasMods/IfaceStub");

/*
The editor API stub of a mod interface (--api): a Blueprint Interface asset whose functions an editor Blueprint can
implement. IStubPing has a function with a result and one without, a tooltip, and a variable, which the stub leaves out;
IStubLoud extends it. StubSelf implements it in C++; StubRelay calls Ping through the interface on an object of a class
named at run time, which in the game is EdPinger, a Blueprint the UE 4.27 editor made against the stub.
*/
class IStubPing {
public:
  UE_INTERFACE;
  UE_TOOLTIP("Answers a ping with a number.") UE_KEYWORDS("echo reply")
  int32 Ping(int32 X);
  void Note(FString Text);
  int32 Marks = 2;
};

class IStubLoud : public IStubPing {
public:
  UE_INTERFACE;
  int32 Shout();
};

class StubSelf : public UObject, public IStubPing {
public:
  int32 Ping(int32 X) { return X + 100; }
};

class StubRelay : public UObject {
public:
  TSoftClassPtr<UObject> Pinger = "/Game/EdStub/EdPinger.EdPinger_C";
  UE_TOOLTIP("Pings the editor Blueprint.") UE_KEYWORDS("relay forward")
  int32 Relay(int32 X) {
    UClass *C = UKismetSystemLibrary::LoadClassAsset_Blocking(Pinger);
    if (C == nullptr) return -1;
    TScriptInterface<IStubPing> P = NewObject<UObject>(this, C);
    if (P == nullptr) return -2;
    return P->Ping(X);
  }
};
