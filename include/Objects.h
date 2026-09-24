#pragma once
/*
Objects.h - typed object creation for mods: the C++ spelling of the editor's Spawn Actor, Construct
Object, Create Widget, Add Component and Attach nodes.

Every function here is `inline`, so AssetGen copies its body into the caller instead of calling
anything of its own: a call costs exactly the engine calls in the body, the ones the editor's
nodes expand to (K2Node_SpawnActorFromClass, K2Node_GenericCreateObject, K2Node_CreateWidget,
K2Node_AddComponentByClass). The world context the engine calls leave out is the caller's self,
or a static caller's own world context parameter. Write more helpers the same way.

  AMyActor* A = SpawnActor<AMyActor>(AMyActor::StaticClass(), Where);
  AMyActor* B = SpawnActorDeferred<AMyActor>(Class, Where);   // set properties, then:
  FinishSpawning(B, Where);
  UMyObject* O = NewObject<UMyObject>(this);
  UMyWidget* W = CreateWidget<UMyWidget>(PlayerController, WidgetClass);
  UStaticMeshComponent* C = AddComponentByType<UStaticMeshComponent>(this);
  UMyComp* D = AddComponentDeferred<UMyComp>(this);           // set properties, then:
  FinishComponent(this, D);
  AttachToComponent(C, GetRootComponent());
  AMyActor* Owner = GetTypedOuter<AMyActor>(this);           // the nearest outer that is one, or null
  UMyGame* Game = GetOutermostTypedOuter<UMyGame>(this);     // the farthest
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/UMG.h"

/* What a helper's T has to be, refused at the call that got it wrong rather than somewhere in the body:
   `SpawnActor<UObject>(...)` is "constraints not satisfied" on that line. A compiler builtin, so no <concepts>. */
template <class T, class Base> concept Derives = __is_base_of(Base, T);

template <Derives<AActor> T>
inline T *SpawnActorDeferred(TSubclassOf<T> Class, const FTransform &Transform, AActor *Owner = nullptr,
                             ESpawnActorCollisionHandlingMethod Collision = ESpawnActorCollisionHandlingMethod::Undefined) {
  return (T *)UGameplayStatics::BeginDeferredActorSpawnFromClass((UClass *)Class, Transform, Collision, Owner);
}

inline AActor *FinishSpawning(AActor *Actor, const FTransform &Transform) {
  return UGameplayStatics::FinishSpawningActor(Actor, Transform);
}

template <Derives<AActor> T>
inline T *SpawnActor(TSubclassOf<T> Class, const FTransform &Transform, AActor *Owner = nullptr,
                     ESpawnActorCollisionHandlingMethod Collision = ESpawnActorCollisionHandlingMethod::Undefined) {
  return (T *)UGameplayStatics::FinishSpawningActor(
      UGameplayStatics::BeginDeferredActorSpawnFromClass((UClass *)Class, Transform, Collision, Owner), Transform);
}

/* Neither an actor nor a component, as the editor's Construct Object node has it (K2Node_GenericCreateObject.cpp,
   IsClassAllowedLambda). SpawnObject itself checks nothing of the kind: it would hand back an actor no world
   spawned, or a component nothing registered. Those are SpawnActor and AddComponentByType. */
template <Derives<UObject> T>
  requires(!Derives<T, AActor> && !Derives<T, UActorComponent>)
inline T *NewObject(UObject *Outer, TSubclassOf<T> Class = T::StaticClass()) {
  return (T *)UGameplayStatics::SpawnObject((UClass *)Class, Outer);
}

template <Derives<UUserWidget> T>
inline T *CreateWidget(APlayerController *OwningPlayer, TSubclassOf<T> Class = T::StaticClass()) {
  return (T *)UWidgetBlueprintLibrary::Create((UClass *)Class, OwningPlayer);
}

template <Derives<UActorComponent> T>
inline T *AddComponentByType(AActor *Owner, TSubclassOf<T> Class = T::StaticClass(), bool bManualAttachment = false) {
  return (T *)Owner->AddComponentByClass((UClass *)Class, bManualAttachment, FVector(0.0f, 0.0f, 0.0f), false);
}

/* The component twin of SpawnActorDeferred: bDeferredFinish holds registration (and so the component's BeginPlay)
   back until FinishComponent, which is when the values set in between are first seen - what the editor's
   Add Component node does with its exposed pins.
     UMyComp* C = AddComponentDeferred<UMyComp>(this);\
     C->Charges = 3;
     FinishComponent(this, C);    
*/

template <Derives<UActorComponent> T>
inline T *AddComponentDeferred(AActor *Owner, TSubclassOf<T> Class = T::StaticClass(), bool bManualAttachment = false) {
  return (T *)Owner->AddComponentByClass((UClass *)Class, bManualAttachment, FVector(0.0f, 0.0f, 0.0f), true);
}

inline void FinishComponent(AActor *Owner, UActorComponent *Component, bool bManualAttachment = false) {
  Owner->FinishAddComponent(Component, bManualAttachment, FVector(0.0f, 0.0f, 0.0f));
}

/* UObject::GetTypedOuter, as a walk up GetOuter(): the nearest outer that is a T, or null. Each step is one
   GetOuterObject call and one Cast<T> (EX_DynamicCast), as the editor would spell it. Obj itself is not a candidate. */
template <Derives<UObject> T>
inline T *GetTypedOuter(UObject *Obj) {
  UObject *O = Obj;
  while (O) {
    O = O->GetOuter();
    T *Typed = Cast<T>(O);
    if (Typed) return Typed;
  }
  return nullptr;
}

/* The same walk keeping the last hit: the farthest outer that is a T, or null. */
template <Derives<UObject> T>
inline T *GetOutermostTypedOuter(UObject *Obj) {
  T *Last = nullptr;
  UObject *O = Obj;
  while (O) {
    O = O->GetOuter();
    T *Typed = Cast<T>(O);
    if (Typed) Last = Typed;
  }
  return Last;
}

inline bool AttachToComponent(USceneComponent *Child, USceneComponent *Parent, FName Socket = FName(),
                              EAttachmentRule Rule = EAttachmentRule::SnapToTarget, bool bWeld = true) {
  return Child->K2_AttachToComponent(Parent, Socket, Rule, Rule, Rule, bWeld);
}

inline void AttachToActor(AActor *Child, AActor *Parent, FName Socket = FName(),
                          EAttachmentRule Rule = EAttachmentRule::SnapToTarget, bool bWeld = true) {
  Child->K2_AttachToActor(Parent, Socket, Rule, Rule, Rule, bWeld);
}
