/*
UdsLocalInit.cpp - a local of a UE_STRUCT starts at the struct's defaults.

The VM zeroes a function's frame and runs InitializeValue only on the locals from Function->FirstPropertyToInit on
(ScriptCore.cpp ProcessScriptFunction), and UFunction::Link finds that property only when the function is
FUNC_HasDefaults (Class.cpp). The editor sets the flag for any local that is not zero-constructible - a struct whose
default instance is not all zero is not. Without it every local below is zeros: Hp 0, bOn false, Inner.Kills 0.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/UdsLocalInit");

struct FUdsLocalGauge {
  UE_STRUCT;
  int32 Kills = 9;
  float Time;
};

struct FUdsLocalTuned {
  UE_STRUCT;
  int32 Hp = 100;
  float Rate = 0.25f;
  bool bOn = true;
  FName Tag = "Hot";
  TArray<int32> Seq = {4, 5};
  FUdsLocalGauge Inner = {.Time = 2.5f};
  FUdsLocalGauge Plain;
  int32 Zero;
};

class UdsLocalInit : public AActor {
public:
  /* A declared local: 100 + 1000 + 9. */
  int32 LocalHp() {
    FUdsLocalTuned L;
    return L.Hp + (L.bOn ? 1000 : 0) + L.Inner.Kills;
  }
  /* A braced one keeps what the braces leave out: 100 + 9 + 9, and the Rate given. */
  float BracedHp() {
    FUdsLocalTuned L = {.Rate = 2.0f};
    return L.Hp + L.Plain.Kills + L.Inner.Kills + L.Rate;
  }
  /* Declared in a loop, it is made again each pass: 100 every time, never the 1 the pass before stored. */
  int32 LoopHp(int32 N) {
    int32 S = 0;
    for (int32 I = 0; I < N; I++) {
      FUdsLocalTuned L;
      S += L.Hp;
      L.Hp = 1;
    }
    return S;
  }
  /* A struct whose only initializer is on a member: 9. */
  int32 GaugeKills() {
    FUdsLocalGauge G;
    return G.Kills;
  }
  /* Its name and array members: "Hot", 2 elements. */
  int32 TagAndSeq() {
    FUdsLocalTuned L;
    return (L.Tag == FName("Hot") ? 10 : 0) + L.Seq.Num();
  }
};
