#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TypeUeNameOther");

/* The other mod of TypeUeName: FTunRow and ETunMood come from TypeUeName (UE_STRUCT_IN / UE_ENUM_IN), and their
   `__UeName` markers make the imports name the assets TunRow and TunMood that TypeUeName cooks. */
struct FTunRow {
  UE_STRUCT_IN("/Game/_ElytrasMods/TypeUeName");
  int32 N = 2;
};
static constexpr const char *FTunRow__UeName = "TunRow";

enum class ETunMood : uint8 { Calm, Angry };
UE_ENUM_IN(ETunMood, "/Game/_ElytrasMods/TypeUeName");
static constexpr const char *ETunMood__UeName = "TunMood";

class TypeUeNameOther : public AActor {
public:
  FTunRow Row;
  TArray<FTunRow> Rows;
  ETunMood Mood = ETunMood::Angry;
  void Pass(FTunRow In, ETunMood M) {}
};
