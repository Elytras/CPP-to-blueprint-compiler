/*
EnumNativeClash.cpp - a mod enum named like one of the game's, sharing its enumerators.

The game's EDialogRestriction (FSD.h) already registers EDialogRestriction::None and ::SinglePlayerOnly in the one global
map of enumerator names (UEnum::AddNamesToMasterList). A UserDefinedEnum of that name brings the same FNames: the load
keeps the game's entry with only a log line, and every global lookup by name (UEnum::LookupEnumName) answers with the
game's enum. The compiler knows the game's enums, so it should refuse this, or at least warn, naming the clash.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/EnumNativeClash");

namespace ClashNs {
enum class EDialogRestriction : uint8 { None, SinglePlayerOnly };
UE_ENUM(EDialogRestriction);
} // namespace ClashNs

class EnumNativeClash : public AActor {
public:
  ClashNs::EDialogRestriction Mode = ClashNs::EDialogRestriction::SinglePlayerOnly;
};
