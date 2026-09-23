#include "../include/Objects.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SpawnTest");

/* GetTypedOuter reads OuterPrivate; a mod reading memory declares the compiler's scratch (Intrin.h). */
struct FDeref {
  UE_STRUCT;
  int64 Data;
  int32 Num;
  int32 Max;
};

/* Objects.h: every helper compiles to the engine calls the editor's nodes use. */
/* A mod UObject: NewObject<T> must construct THIS class, not the UObject whose StaticClass it inherits. */
class USpawnProbe : public UObject {
public:
  int32 Value;
};

class SpawnTest : public AActor {
  AActor *Spawned;
  UObject *Made;
  UUserWidget *Widget;
  USceneComponent *Part;
  int32 Tag;

public:
  void ReceiveBeginPlay() {
    FTransform Where = FVector(0.0f, 0.0f, 100.0f);
    Spawned = SpawnActor<AActor>(AActor::StaticClass(), Where, this);
    SpawnTest *Twin = SpawnActorDeferred<SpawnTest>(SpawnTest::StaticClass(), Where);
    Twin->Tag = 7;
    FinishSpawning(Twin, Where);
    Made = NewObject<UObject>(this);
    Widget = CreateWidget<UUserWidget>(nullptr, UUserWidget::StaticClass());
    Part = AddComponentByType<USceneComponent>(this);
    AttachToComponent(Part, K2_GetRootComponent());
    USceneComponent *Late = AddComponentDeferred<USceneComponent>(this);
    Late->bHiddenInGame = true;         // seen by the component's first registration, not after it
    FinishComponent(this, Late);
    AttachToActor(Spawned, this, FName(), EAttachmentRule::KeepWorld);
  }

  /* StaticClass is declared once, on the native class; the one meant is the qualifier / NewObject's T. */
  UClass *OwnClass() { return SpawnTest::StaticClass(); }
  UObject *MakeProbe() { return NewObject<USpawnProbe>(this); }

  /* The outer walks: an actor's nearest and farthest outer of a kind. */
  AActor *OwningActor(UObject *Obj) { return GetTypedOuter<AActor>(Obj); }
  ULevel *LevelOf(UObject *Obj) { return GetOutermostTypedOuter<ULevel>(Obj); }
};
