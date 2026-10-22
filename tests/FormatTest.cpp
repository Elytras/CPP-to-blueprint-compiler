#include "UeApi/Types.h"

#include "../include/Format.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FormatTest");

/* Format.h: an fmt-style call, each argument an FFormatArgumentData of its type, named by the placeholder it fills. */
class FormatTest : public AActor {
public:
  FString NameValue(FName Key, bool bOn) { return Format("{Name}:{Value}", Key, bOn); }
  FString Numbers(int32 Hits, float Rate, FString Who) { return Format("{Who} hit {Hits} ({Rate}), {Who}!", Who, Hits, Rate); }
  FString Escaped(int32 N) { return Format("`{N`} is {N}", N); }
  // Arguments that are computed: each one is held in a local of its own until the Format node runs.
  FString Computed(FString S, int32 N) { return Format("{A}-{B}-{C}", S + "!", N * 2, FText(S)); }
};
