#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PropDefaults");

/*
PropDefaults.cpp - what the engine reads off a class's properties and its default object, beyond the values TypesTest
covers:

- FText defaults: a literal is a culture-invariant text in the CDO, an ASCII one and a UTF-16 one; an unset or empty
  text reads back empty.
- The descriptive flags AActor::ResetPropertiesForConstruction reads at run time (instance-editable, settable from a
  Blueprint) on a plain variable, a const one (BlueprintReadOnly) and a component's.
- Set elements and map keys of hashable types only: a name, an int, a vector, and an array (wrapped in a struct).
*/
class PropDefaults : public AActor {
public:
  FText Label = "Ready";
  FText Wide = "Größe";
  FText Empty = "";
  FText Blank;
  const int32 Limit = 3;
  int32 Plain = 4;
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UStaticMeshComponent, Mesh);
  TSet<FName> Names = {"a", "b"};
  TMap<int32, FText> Labels = {{1, "one"}};
  TSet<FVector> Spots;
  TSet<TArray<int32>> Lists;
};
