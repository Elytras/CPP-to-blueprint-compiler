// A qualified call to an ancestor further up than the direct parent runs that ancestor's own function, past every
// override between (test_bytecode.py engine_parent_call; REFERENCE.md#calling-the-parent.aactor-receivebeginplay
// and #calling-the-parent.ancestor-skip):
// - `AActor::ReceiveBeginPlay()`: a final call on /Script/Engine.Actor:ReceiveBeginPlay, which as an implementable
//   event has no body, so EpcBase's override is skipped;
// - `EpcBase::X()` from EpcSkip, past EpcMid: EpcBase's body copied in, or a final call on EpcBase_C's function where
//   it cannot be (an event override, `noinline`);
// - `ABP_Damage_Cloud_Base::ReceiveBeginPlay()` from a child of BP_Spider_RadiationCloud, a cooked game Blueprint
//   whose own override is skipped: a final call on BP_Damage_Cloud_Base_C's function.
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"
#include "UeApi/Game/ABP_Spider_RadiationCloud.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/EngineParentCall");

class EpcBase : public AActor {
public:
  int32 Count;
  void  ReceiveBeginPlay() { Count = 1; }
  int32 Bump(int32 By) {
    Count = Count + By;
    return Count;
  }
  [[gnu::noinline]] int32 Slow(int32 By) {
    Count = Count + By;
    return Count;
  }
};

class EngineParentCall : public EpcBase {
public:
  void ReceiveBeginPlay() {
    AActor::ReceiveBeginPlay();
    Count = Count + 10;
  }
};

class EpcMid : public EpcBase {
public:
  void  ReceiveBeginPlay() { Count = Count + 100; }
  int32 Bump(int32 By) {
    Count = Count + 100 * By;
    return Count;
  }
  [[gnu::noinline]] int32 Slow(int32 By) {
    Count = Count + 100 * By;
    return Count;
  }
};

class EpcSkip : public EpcMid {
public:
  void ReceiveBeginPlay() {
    EpcBase::ReceiveBeginPlay();
    Count = Count + 10;
  }
  int32 Bump(int32 By) { return EpcBase::Bump(By) + 1000; }
  int32 Slow(int32 By) { return EpcBase::Slow(By) + 1000; }
};

class EpcGame : public ABP_Spider_RadiationCloud {
public:
  int32 Count;
  void  ReceiveBeginPlay() {
    ABP_Damage_Cloud_Base::ReceiveBeginPlay();
    Count = Count + 10;
  }
};
