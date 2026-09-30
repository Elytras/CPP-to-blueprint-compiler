#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/NameSuffix");

/*
FName splits a _digits suffix of up to ten digits when the value is below MAX_int32 (UnrealNames.cpp ParseNumber):
FName("Tag_1234567890") is "Tag" #1234567891. A package that stores the name whole makes a different FName, so the
literal below would compare unequal to the same text converted at run time, and the property would not be found by
its name. Pending: the compiler splits only up to nine digits.
*/
class NameSuffix : public AActor {
public:
  int32 Count_1234567890 = 3;

  FName Tag() { return FName("Tag_1234567890"); }
  bool Same() { return FName("Tag_1234567890") == UKismetStringLibrary::Conv_StringToName(FString("Tag_1234567890")); }
};
