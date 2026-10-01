/*
UdsInitTest.cpp - a UE_STRUCT's member initializers are its default instance.

A UserDefinedStruct has no C++ constructor: every new value of it - a class member, a local, an array element, a
member of another struct - is zeroed, each member initialized, and then the struct's default instance copied over it
(UUserDefinedStruct::InitializeStruct). That instance is the Data stream after the struct's properties, so every member
initializer must be written there; a member left out takes its type's own default, which for a struct member is that
struct's defaults. A designated default of a class member leaves the members it does not name at their initializers.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/UdsInitTest");

enum class EUdsDial : uint8 { Off, Low, High };
UE_ENUM(EUdsDial);

struct FUdsGauge {
  UE_STRUCT;
  int32 Kills = 9;
  float Time;
};

struct FUdsTuned {
  UE_STRUCT;
  int32 Hp = 100;
  float Rate = 0.25f;
  bool bOn = true;
  EUdsDial Dial = EUdsDial::Low;
  FName Tag = "Hot";
  FString Label = "x";
  FVector Pos = {1, 2, 3};
  TArray<int32> Seq = {4, 5};
  FUdsGauge Inner = {.Time = 2.5f};
  FUdsGauge Plain;
  int32 Zero;
};

class UdsInitTest : public AActor {
public:
  FUdsTuned Tuned;
  FUdsTuned Braced = {.Hp = 7, .Plain = {.Time = 0.5f}};
};
