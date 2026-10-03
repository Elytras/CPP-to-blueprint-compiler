/* DelegateParmKeys: which delegate types are one and which are two. A UE_ENUM in a namespace is one type spelled short
   inside it, qualified outside it and from the global namespace (`::DelegateParmKeysNs::EDpkMode`): one signature
   function of DelegateParmKeysTop's, which the overrides of Use (DelegateParmKeys is generated before Top,
   DelegateParmKeysZKid after), the values handed to Take - declared short, defined qualified -, the value set through
   Peer and those stored into an element of Top's TArray and TMap, on this object and on Peer, all name, and
   `Held = D` across two spellings compiles. The comma value handed to Take is held in a local of that signature too,
   and DelegateParmKeys makes none of its own. DelegateParmKeysKinds keeps apart what C++
   keeps apart - int and int64, uint8 and a uint8 enum, a value and a reference, a reference and a const one, a return
   value and none - and makes one signature function for int32 and int, TArray<int32> and TArray<int>, and an enum
   spelled short and qualified. */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DelegateParmKeys");

enum class EDpkKey : uint8 { A, B };
UE_ENUM(EDpkKey);

namespace DelegateParmKeysNs {
enum class EDpkMode : uint8 { A, B };
UE_ENUM(EDpkMode);

class DelegateParmKeysHelper : public AActor {
public:
  int32 Got = 0;
  void Pick(EDpkMode M) { Got += 1; }
};

class DelegateParmKeysTop : public AActor {
public:
  DelegateParmKeysHelper *H = nullptr;
  DelegateParmKeysTop *Peer = nullptr;
  TDelegate<void(EDpkMode)> Held;
  TArray<TDelegate<void(EDpkMode)>> Picks;
  TMap<int32, TDelegate<void(EDpkMode)>> ByKey;
  virtual void Use(TDelegate<void(EDpkMode)> D) { Held = D; }
  void Take(TDelegate<void(EDpkMode)> T);
};

class DelegateParmKeysKinds : public AActor {
public:
  TDelegate<void(int32)> I32;
  TDelegate<void(int)> I;
  TDelegate<void(int64)> I64;
  TDelegate<void(uint8)> U8;
  TDelegate<void(EDpkKey)> En;
  TDelegate<void(EDpkMode)> Mode;
  TDelegate<void(DelegateParmKeysNs::EDpkMode)> ModeQ;
  TDelegate<void(FVector)> VecVal;
  TDelegate<void(const FVector &)> VecCRef;
  TDelegate<void(FVector &)> VecRef;
  TDelegate<int32()> RetI;
  TDelegate<void()> RetV;
  TDelegate<void(TArray<int32>)> ArrA;
  TDelegate<void(TArray<int>)> ArrB;
};
} // namespace DelegateParmKeysNs

void DelegateParmKeysNs::DelegateParmKeysTop::Take(TDelegate<void(DelegateParmKeysNs::EDpkMode)> T) { Held = T; }

class DelegateParmKeys : public DelegateParmKeysNs::DelegateParmKeysTop {
public:
  int32 Bumps = 0;
  void Use(TDelegate<void(DelegateParmKeysNs::EDpkMode)> D) override { Held = D; }
  int32 Bump() { Bumps += 1; return Bumps; }
  void CallTake() { Peer->Take({H, &DelegateParmKeysNs::DelegateParmKeysHelper::Pick}); }
  void SetPeer() { Peer->Held = {H, &DelegateParmKeysNs::DelegateParmKeysHelper::Pick}; }
  void CommaTake() { Peer->Take((Bump(), Held)); }
  void SetPicks() {
    Picks[0] = {H, &DelegateParmKeysNs::DelegateParmKeysHelper::Pick};
    Peer->Picks[0] = {H, &DelegateParmKeysNs::DelegateParmKeysHelper::Pick};
  }
  void SetByKey() {
    ByKey[Bump()] = {H, &DelegateParmKeysNs::DelegateParmKeysHelper::Pick};
    Peer->ByKey[1] = {H, &DelegateParmKeysNs::DelegateParmKeysHelper::Pick};
  }
};

class DelegateParmKeysZKid : public DelegateParmKeysNs::DelegateParmKeysTop {
public:
  void Use(TDelegate<void(::DelegateParmKeysNs::EDpkMode)> D) override { Held = D; }
  void CallTake() { Peer->Take({H, &::DelegateParmKeysNs::DelegateParmKeysHelper::Pick}); }
};
