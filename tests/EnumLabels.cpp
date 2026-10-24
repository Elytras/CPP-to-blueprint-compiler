#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/EnumLabels");

/* `__UeEnumNames` gives an enumerator a display name (None_ shows as "None"), a cooked name (Old is cooked as
   NewEnumerator7, as an editor enum's was), or both; Plain keeps its C++ name and shows it. */
enum class ELabel : uint8 { None_, Hit_Override, Plain, Old };
UE_ENUM(ELabel);
static constexpr const char* ELabel__UeEnumNames = "None_=None_=None|Hit_Override=Hit_Override=Hit Override|Old=NewEnumerator7";

class EnumLabels : public AActor {
public:
    ELabel Label = ELabel::Hit_Override;
};
