#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PreloadTypes");

/*
User-defined structs and an enum, used as a class variable, an array element, a function parameter, a local and a
struct member. Linking each owner reads the type (a struct's PropertiesSize, an enum's names), so the type is
serialized before the owner is.
*/
enum class EPreloadTier : uint8 { Low, High };
UE_ENUM(EPreloadTier);

struct FPreloadInner {
  UE_STRUCT;
  int32 A;
};

struct FPreloadOuter {
  UE_STRUCT;
  FPreloadInner Inner;
  EPreloadTier Tier;
};

class PreloadTypes : public AActor {
  FPreloadOuter Held;
  TArray<FPreloadInner> Many;
  EPreloadTier Tier;

public:
  int32 Sum(FPreloadOuter O) {
    FPreloadInner Local = O.Inner;
    return Local.A + (O.Tier == EPreloadTier::High ? 10 : 0);
  }
};
