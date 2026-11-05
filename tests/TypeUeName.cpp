#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TypeUeName");

/* A `__UeName` marker beside a mod struct or enum is the name it cooks under: FTunRow is the asset TunRow, ETunMood the
   asset TunMood, and every property, parameter, RPC and container of them names those. TypeUeNameOther imports both
   from the header the two would share (UE_STRUCT_IN / UE_ENUM_IN). The same marker beside a UE_ASSET_DEFAULTS asset is
   its name: P_Tun_Burst cooks as P_Tun-Burst, which C++ cannot spell, and Burst's default names that. */
struct FTunRow {
  UE_STRUCT;
  int32 N = 2;
};
static constexpr const char *FTunRow__UeName = "TunRow";

enum class ETunMood : uint8 { Calm, Angry };
UE_ENUM(ETunMood);
static constexpr const char *ETunMood__UeName = "TunMood";

UE_ASSET_DEFAULTS(UParticleSystem, P_Tun_Burst) { LODDistances = {0.0f}; }
static constexpr const char *P_Tun_Burst__UeName = "P_Tun-Burst";

class TypeUeName : public AActor {
public:
  UParticleSystem *Burst = &P_Tun_Burst;
  FTunRow Row;
  TArray<FTunRow> Rows;
  TMap<ETunMood, FTunRow> ByMood;
  ETunMood Mood = ETunMood::Angry;
  UE_REPLICATED(FTunRow, Net);
  UE_SERVER UE_RELIABLE void ServerSet(FTunRow In, ETunMood M) {
    Net = In;
    Mood = M;
  }
  int32 Take(FTunRow In) { return In.N; }
  int32 Call() {
    FTunRow L;
    L.N = 5;
    return Take(L);
  }
};
