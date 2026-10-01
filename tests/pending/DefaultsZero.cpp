#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DefaultsZero");

/*
UE_DEFAULTS over a parent's non-zero defaults, each set to its type's zero by value-initialisation: `{}`, `T()` and
`nullptr`. A subclass's default object deltas against its parent's, so the zero is written as a tag, as `Count = 0;`
is: a number, a float, a bool, an enum, an object pointer, a name, a string, an array, an engine struct and a
UE_STRUCT, whose `{}` and `T()` are its own defaults.
*/
struct FZeroIn {
  UE_STRUCT;
  int32 P = 1;
  int32 Q = 2;
};

class DefaultsZeroParent : public AActor {
public:
  int32 Count = 5;
  float Rate = 2.5f;
  bool bOn = true;
  EAttachmentRule Rule = EAttachmentRule::KeepWorld;
  AActor* Who;
  FName Tag = "Keep";
  FString Text = "Keep";
  TArray<int32> Ids = {1, 2};
  FVector2D V = {1.0f, 2.0f};
  FZeroIn In = {5, 6};
  TArray<int32> More = {3};
  FVector2D W = {3.0f, 4.0f};
  FZeroIn Kept = {7, 8};
};

class DefaultsZero : public DefaultsZeroParent {
public:
  UE_DEFAULTS {
    Count = {};
    Rate = float();
    bOn = {};
    Rule = EAttachmentRule();
    Who = nullptr;
    Tag = {};
    Text = {};
    Ids = {};
    V = {};
    In = {};
    More = TArray<int32>();
    W = FVector2D();
    Kept = FZeroIn();
  }
};
