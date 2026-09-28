/* Beacon: an actor with components and component defaults of its own, spawned by the mod, with a looping timer.

   The mod waits for your dwarf, spawns a Beacon three metres in front of it and posts "Beacon placed" as a game
   message. The beacon is a thin post with an orange light above it. The light blinks once a second for twenty
   seconds, then stays on. The game spawns InitSpacerig in the Space Rig and InitCave in a mission; both are BeaconMod
   under another name. */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"
#include "UeAssets/UStaticMesh.h"

#include "../include/Objects.h" // AssetGen's include/ folder, by its path from this file

UE_MOD_PACKAGE("/Game/_AssetGenExamples/Beacon");

/* The beacon, cooked as the Blueprint class Beacon_C. Each UE_COMPONENT is a row in the Components panel: the first
   scene component, Root, becomes the actor's root, and Mesh and Lamp attach to it. */
class Beacon : public AActor {
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UStaticMeshComponent, Mesh);
  UE_COMPONENT(UPointLightComponent, Lamp);

  /* What you would set in each component's details panel. The block never runs: AssetGen reads the assignments and
     writes them into the component templates, so every beacon starts out with these values. The mesh is the engine's
     basic cylinder, named through the UeAssets headers, which name the game's assets by their paths. */
  UE_DEFAULTS {
    Mesh->StaticMesh = &UeAssets::UStaticMesh::Engine::BasicShapes::Cylinder;
    Mesh->RelativeScale3D = FVector(0.2f, 0.2f, 1.5f);
    Lamp->RelativeLocation = FVector(0.0f, 0.0f, 100.0f);
    Lamp->LightColor = FColor(255, 140, 0);
    Lamp->Intensity = 8000.0f;
  }

  FTimerHandle BlinkTimer;
  int32 Blinks = 0;

public:
  /* Runs inside the SpawnActor call that places the beacon. It starts a looping timer, the Set Timer by Event node
     with Looping ticked, and keeps its handle so that Blink can stop it. */
  void ReceiveBeginPlay() {
    BlinkTimer = UKismetSystemLibrary::K2_SetTimerDelegate({this, &Beacon::Blink}, 0.5f, true, 0.0f, 0.0f);
  }

  /* The timer calls this every half second. It is an ordinary method, not inline, because a timer needs a function
     of the class to call. After 40 toggles the light is on again, and clearing the handle stops the timer. */
  void Blink() {
    Lamp->ToggleVisibility(false);
    Blinks += 1;
    if (Blinks == 40)
      UKismetSystemLibrary::K2_ClearAndInvalidateTimerHandle(BlinkTimer);
  }
};

/* The mod's own actor. The game starts it through InitSpacerig and InitCave below; it places one beacon and is done. */
class BeaconMod : public AActor {
public:
  void ReceiveBeginPlay() {
    /* First we wait for your dwarf: the game can start the mod before the character exists. Delay is the latent
       Delay node, so each round of the loop hands control back to the engine and resumes here half a second later. */
    APlayerCharacter *Dwarf = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    while (Dwarf == nullptr) {
      UKismetSystemLibrary::Delay(0.5f);
      Dwarf = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    }

    /* Then we spawn the beacon 300 units (three metres) in front of the dwarf. A location converts to a transform
       that only moves. SpawnActor, from Objects.h, is the Spawn Actor from Class node: the beacon's ReceiveBeginPlay
       has run by the time it returns. */
    FTransform Where = Dwarf->K2_GetActorLocation() + Dwarf->GetActorForwardVector() * 300.0f;
    Beacon *Placed = SpawnActor<Beacon>(Beacon::StaticClass(), Where);
    if (Placed)
      UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage("Beacon placed");
  }
};

/* The game spawns a mod's InitSpacerig in the Space Rig and its InitCave in a mission. Both are
   empty, so whichever one the game spawns runs BeaconMod's ReceiveBeginPlay. */
class InitSpacerig : public BeaconMod {};
class InitCave : public BeaconMod {};
