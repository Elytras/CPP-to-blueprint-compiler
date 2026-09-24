#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/StringTest");

class StringTest : public AActor {
  FString Label;
  FName Key;
  FText Caption;
  int32 Count;
  float Ratio;
  int64 Big;

public:
  void ReceiveBeginPlay() {
    Count = 3;
    Ratio = 0.5f;
    Label = FString("Kills: ") + Count + ", ratio " + Ratio;
    Key = Label + "_key";
    Caption = Key;
    Caption = Label + Key + Caption;
    FText Literal = "plain text";
    FName Named = "plain name";
    Label = Named + Literal;
    Big = 1234567890123;
    Caption = Big;
    Label = Label + Big;
    Count = int(Label);
    Ratio = Count;
    Big = Count;
    uint8 Small = 7;
    Count = Small;
    Key = MakeKey(Caption, Count);
    Label = Label + MakeKey(Caption, Count) + GetTickableWhenPaused();
    UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Label + " (" + this + ")");
    /* C++ reads these two as pointer arithmetic; AssetGen reads the concat. */
    Label = "Kills: " + Count;
    Label = Count + " left";
  }

  UE_PURE FName MakeKey(FText Prefix, int32 Index) { return Prefix + "_" + Index; }
};
