/*
EnumMaxDup.cpp - an enum that declares its own <Enum>_MAX, the usual UE C++ idiom.

A cooked enum closes with the sentinel <Enum>::<Enum>_MAX, one past the largest value: the engine reads it as the invalid
marker (an unresolved name loads as GetMaxEnumValue, GetValidValue maps a stray value to it). A declared EMaxDupGear_MAX
must not become a second entry of the same FName beside the one the compiler adds: the names must stay unique (one
global name map, AddNamesToMasterList) and exactly one _MAX entry must hold the maximum.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/EnumMaxDup");

enum class EMaxDupGear : uint8 { Low, High, EMaxDupGear_MAX };
UE_ENUM(EMaxDupGear);

class EnumMaxDup : public AActor {
public:
  EMaxDupGear Gear = EMaxDupGear::High;
};
