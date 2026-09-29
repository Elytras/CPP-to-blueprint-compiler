#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CompAttachInherited");

/*
An own component attached to one the class inherits, written as UE's C++ writes it in a constructor. The SCS says so
on the root node: ParentComponentOrVariableName, with ParentComponentOwnerClassName the ancestor Blueprint whose SCS
has that node, or bIsParentComponentNative and the native default subobject's name (ACharacter's Mesh is the
subobject CharacterMesh0).
*/
class AttachBase : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UPointLightComponent, Lamp);
};

class CompAttachInherited : public AttachBase {
public:
  UE_COMPONENT(UPointLightComponent, Glow);

  UE_DEFAULTS { Glow->SetupAttachment(Lamp); }
};

class AttachChar : public ACharacter {
public:
  UE_COMPONENT(UPointLightComponent, Glow);

  UE_DEFAULTS { Glow->SetupAttachment(Mesh); }
};
