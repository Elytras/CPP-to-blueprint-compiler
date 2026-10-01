#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/NameNumberTest");

/*
A name that ends in _digits is two things to the engine: FName("Count_7") is the entry "Count" with the number 8,
and a package stores it so - the name map holds "Count", the reference carries 8. The loader never splits an entry
itself, so a name written whole ("Count_7" with no number) would be a different FName from the one the engine makes
of the same text, and every lookup by name would miss it. What counts as a number is FName's rule, not a guess: at
most ten digits, no leading zero unless it is the only one, below MAX_int32. Each member and literal below sits on one
side of that rule.
*/
class NameNumberTest : public AActor {
public:
  int32 Count_7 = 1;           // "Count" #8
  int32 Level_0 = 2;           // "Level" #1: a lone zero is a number
  int32 Rocket_04 = 3;         // a leading zero: kept whole
  int32 Width_123456789 = 4;   // nine digits: "Width" #123456790

  FName Tag() { return FName("Tag_7"); }
  FName Socket() { return FName("Socket_01"); }
  FName Nine() { return FName("Nine_123456789"); }
  FName Cap() { return FName("Cap_2147483647"); }   // MAX_int32 itself is not below MAX_int32: kept whole
  int32 Sum() { return Count_7 + Level_0 + Rocket_04 + Width_123456789; }
};
