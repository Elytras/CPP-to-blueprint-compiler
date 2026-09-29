/*
PropEnumClass.cpp - a native `enum class : uint8` (UENUM, ECppForm::EnumClass) as a variable, a parameter, a return
value and an inherited default.

The editor makes such a pin an EnumProperty over a ByteProperty named UnderlyingType, and a plain or namespaced enum a
ByteProperty with its Enum (Editor/KismetCompiler/Private/KismetCompilerMisc.cpp 1071-1094); the game's cooks carry an
EnumProperty for every enum class. Tags follow the property: an EnumProperty tag naming the enum. EAttachmentRule and
EActorUpdateOverlapsMethod are both `enum class : uint8` in UE 4.27 (EngineTypes.h 57, Actor.h 38).
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PropEnumClass");

class PropEnumClass : public AActor {
public:
  EAttachmentRule Rule = EAttachmentRule::KeepWorld;

  EAttachmentRule Pick(EAttachmentRule In) { return In; }

  UE_DEFAULTS { UpdateOverlapsMethodDuringLevelStreaming = EActorUpdateOverlapsMethod::AlwaysUpdate; }
};
