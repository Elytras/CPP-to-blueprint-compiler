/*
ModComponent.cpp - a component of a class the mod declares itself, and defaults that set part of a struct. UE_COMPONENT
takes the mod's own component Blueprint as it takes an engine one: an SCS node whose template is of that class, made
from its default object. `Field.Member = value;` in UE_DEFAULTS sets that member alone, as the editor saves it: a struct
written as tags holds only the members that differ, and the loader reads them over the parent's value.
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ModComponent");

struct FMcRange {
  UE_STRUCT;
  float Min = 1.0f;
  float Max = 2.0f;
  int32 Steps = 3;
};

class UMcPart : public UActorComponent {
public:
  float Power = 1.0f;
  FMcRange Range;
};

class McBase : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UMcPart, Part); // a component class of the mod's own
  FMcRange Reach;
  FHitResult Hit;

  UE_DEFAULTS {
    Part->Power = 5.0f;
    Part->Range.Max = 9.0f; // part of the template's struct
  }
};

class ModComponent : public McBase {
  UE_DEFAULTS {
    Part->Range.Steps = 7; // part of an inherited component's struct
    Reach.Min = 4.0f;      // part of an inherited mod struct
    Hit.Distance = 6.0f;   // part of an inherited engine struct written as tags
    Hit.Time = 0.5f;
    Hit.Distance = 8.0f;   // a later statement wins
  }
};
