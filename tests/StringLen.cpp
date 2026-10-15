#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/StringLen");

/*
`S.Len()` is UKismetStringLibrary::Len(S), the editor's Len node, like every other FString method genueapi declares.
It used to be a hand-written method of Types.h's FString, which compiled into a call the game has no function for.
*/
class StringLen : public AActor {
public:
  FString Name;

  int32 Param(FString S) { return S.Len(); }
  int32 Member(FString S) { Name = S; return Name.Len(); }
  int32 Concat(FString A, FString B) { return (A + B).Len(); }
};
