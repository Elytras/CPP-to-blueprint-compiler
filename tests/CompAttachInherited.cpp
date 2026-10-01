#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompAttachInherited");

/*
An own component attached to one the class inherits, written as UE's C++ writes it in a constructor. The SCS says so
on the root node: ParentComponentOrVariableName, with ParentComponentOwnerClassName the ancestor Blueprint whose SCS
has that node, or bIsParentComponentNative and the native default subobject's name (ACharacter's Mesh is the
subobject CharacterMesh0). Attached to one of the class's own, a component is one of that node's ChildNodes instead,
at the socket if one is named (AttachToName). Each keeps its RelativeLocation under its parent: Glow sits 10 in
front of Lamp, which is 100 up, and Tip 5 beside Glow. Pivot, left alone, attaches to the actor's root.
*/
class AttachBase : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UPointLightComponent, Lamp);

  UE_DEFAULTS { Lamp->RelativeLocation = FVector(0.0f, 0.0f, 100.0f); }
};

class CompAttachInherited : public AttachBase {
public:
  UE_COMPONENT(UPointLightComponent, Glow);
  UE_COMPONENT(USceneComponent, Pivot);
  UE_COMPONENT(UPointLightComponent, Tip);

  UE_DEFAULTS {
    Glow->SetupAttachment(Lamp);
    Glow->RelativeLocation = FVector(10.0f, 0.0f, 0.0f);
    Pivot->RelativeLocation = FVector(0.0f, 0.0f, 20.0f);
    Tip->SetupAttachment(Glow, FName("Bulb"));
    Tip->RelativeLocation = FVector(0.0f, 5.0f, 0.0f);
  }

  /* Anywhere else, SetupAttachment attaches at once and keeps the relative transform, as the engine's does when the
     component registers: AttachToComponent with KeepRelative, no welding. */
  void ReceiveBeginPlay() { Pivot->SetupAttachment(Lamp); }
};

class AttachChar : public ACharacter {
public:
  UE_COMPONENT(UPointLightComponent, Glow);

  UE_DEFAULTS { Glow->SetupAttachment(Mesh); }
};

/* Further down, Mesh is still ACharacter's CharacterMesh0: APlayerCharacter's FPMesh is a skeletal mesh as well, but a
   subclass cannot rename a subobject its parent made. The game's BP_PlayerCharacter hangs FilmFaceLight there, at
   S_Lamp. The same holds for a default set through Mesh: it overrides CharacterMesh0. */
class AttachPlayer : public APlayerCharacter {
public:
  UE_COMPONENT(UPointLightComponent, Glow);

  UE_DEFAULTS {
    Glow->SetupAttachment(Mesh, FName("S_Lamp"));
    Mesh->bVisible = false;
  }
};

/* With no inherited root, the root is the first scene component SetupAttachment leaves alone, Root, though Bulb is
   declared first. Root's offset passes to Arm, attached to it, and through Arm to Bulb: Arm at (100, 0, 50), Bulb 10
   above it. */
class AttachOwn : public AActor {
public:
  UE_COMPONENT(UPointLightComponent, Bulb);
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(USceneComponent, Arm);

  UE_DEFAULTS {
    Bulb->SetupAttachment(Arm);
    Bulb->RelativeLocation = FVector(0.0f, 0.0f, 10.0f);
    Root->RelativeLocation = FVector(0.0f, 0.0f, 50.0f);
    Arm->RelativeLocation = FVector(100.0f, 0.0f, 0.0f);
  }
};
