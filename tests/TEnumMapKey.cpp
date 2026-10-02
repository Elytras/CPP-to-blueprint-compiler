#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TEnumMapKey");

/*
A TMap keyed by a TEnum<E> whose value is a template too: the key is the enum, the value its own type, as a variable,
a local and a parameter.
*/
enum class ETmPick : uint8 { Zero, One, Two, Three };
UE_ENUM(ETmPick);

class TEnumMapKey : public AActor {
public:
  TMap<TEnum<ETmPick>, TSubclassOf<AActor>> Classes;
  TMap<TEnum<ETmPick>, TEnum<EAttachmentRule>> Rules = {{ETmPick::One, EAttachmentRule::KeepWorld}};
  TMap<TEnum<ETmPick>, TArray<int32>> Lists;

  int32 Rule(int32 M) { return (int32)(EAttachmentRule)Rules[ETmPick::One] + M; }
  int32 Local(int32 M) {
    TMap<TEnum<ETmPick>, TSoftObjectPtr<AActor>> L;
    TMap<TEnum<ETmPick>, TEnum<ETmPick>> Next = {{ETmPick::One, ETmPick::Two}};
    return (int32)(ETmPick)Next[ETmPick::One] * 10 + M;
  }
  int32 Pass(TMap<TEnum<ETmPick>, TEnum<EAttachmentRule>> R, int32 M) { return (int32)(EAttachmentRule)R[ETmPick::Two] + M; }
};
