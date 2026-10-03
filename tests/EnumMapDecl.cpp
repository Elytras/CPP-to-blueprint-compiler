// UE_ENUM_MAP(K, V, Name) declares the member itself, `TMap<K, V> Name` filled at build time as `= UE_ENUM_MAP(E)` fills
// it, so the key and enum types are spelled once: the enum on either side, FName or FString on the other, a game enum
// too. The one-argument form beside it still works (test_bytecode.py enum_map_decl).
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/EnumMapDecl");

enum class EEmdMood : uint8 { Calm, Angry, Sleepy };
UE_ENUM(EEmdMood);

class EnumMapDecl : public AActor
{
public:
    UE_ENUM_MAP(FString, EEmdMood, MoodsByName);
    UE_ENUM_MAP(FName, EEmdMood, MoodsByFName);
    UE_ENUM_MAP(EEmdMood, FName, MoodNames);
    UE_ENUM_MAP(EEmdMood, FString, MoodTexts);
    UE_ENUM_MAP(EEndPlayReason, FName, Reasons);
    TMap<FString, EEmdMood> OldByName = UE_ENUM_MAP(EEmdMood);
};
