/* GameBlueprintChild: a mod class that extends one of the game's own Blueprints.

   BP_LightPost01 is a lamp post from the Space Rig. BP_LightPost01_Alarm derives from it, changes three of the lamp's
   variables and overrides its BeginPlay. In game, when you enter the Space Rig or start a mission, a red lamp post
   appears three metres in front of you and flickers, and "The alarm lamp is on" is posted as a game message. */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"
#include "UeApi/Game/BP_LightPost01_C.h"

#include "../include/Objects.h" // AssetGen's include/ folder, by its path from this file

UE_MOD_PACKAGE("/Game/_AssetGenExamples/GameBlueprintChild");

/* The SDK declares each game Blueprint in a namespace that spells its /Game folder. A mod class declared in that
   namespace is cooked into that folder, so this one becomes /Game/Art/Environments/SpaceRig/BP_LightPost01_Alarm,
   beside the class it extends. Outside a Game:: namespace it would be cooked into the mod's own folder, and would need
   no alias for StaticClass (below). Either works; this example shows the Game:: form. */
namespace Game::Art::Environments::SpaceRig {

/* The child class. Its parent is the game's BP_LightPost01_C, imported from the game's package, so it keeps the
   lamp's components, variables, construction script and events, and changes only what it names here. */
class BP_LightPost01_Alarm : public BP_LightPost01_C {
  /* First we change the lamp's own variables, as you would under Class Defaults in the editor. The lamp's
     construction script sets its PointLight's colour and intensity from Light_Color and Light_Intensity, so we set
     these rather than the PointLight's own values, which the script would overwrite. Flicker is read by the lamp's
     BeginPlay. The SDK spells the editor's "Light Color" as Light_Color; the compiler writes the real name. */
  UE_DEFAULTS {
    Light_Color = FLinearColor(1.0f, 0.05f, 0.0f, 1.0f);
    Light_Intensity = 40.0f;
    Flicker = true;
  }

public:
  /* Event BeginPlay, overridden. An override replaces the parent's event, and it is the lamp's own BeginPlay that
     starts the flicker, so we call it first: BP_LightPost01_C::ReceiveBeginPlay() is the editor's "Add call to
     parent function". Without it the lamp would still be red, but it would never flicker. */
  void ReceiveBeginPlay() {
    BP_LightPost01_C::ReceiveBeginPlay();
    UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage("The alarm lamp is on");
  }
};

}   // namespace Game::Art::Environments::SpaceRig

/* A short name for the lamp outside its namespace. Written with the namespace path,
   Game::Art::Environments::SpaceRig::BP_LightPost01_Alarm::StaticClass() silently names the parent, BP_LightPost01_C,
   except where the value goes straight into a TSubclassOf slot such as SpawnActor's class argument. Through the alias,
   StaticClass() names this class everywhere. */
using AlarmLamp = Game::Art::Environments::SpaceRig::BP_LightPost01_Alarm;

/* The mod's own actor, cooked into the mod's folder. The game starts it as InitSpacerig or InitCave (below), and it
   stands an alarm lamp in front of your dwarf. */
class GameBlueprintChild : public AActor {
public:
  void ReceiveBeginPlay() {
    /* First we wait for the local dwarf. Your character may not exist yet when the game starts the mod, so we ask
       again every half second until GetLocalPlayerCharacter returns one. */
    APlayerCharacter *Player = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    while (Player == nullptr) {
      UKismetSystemLibrary::Delay(0.5f);
      Player = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    }

    /* Then we put the lamp on the floor three metres ahead. A character's location is the middle of its capsule,
       so the floor is half the capsule's height below it. The lamp's construction script and its BeginPlay (the
       override above) run inside SpawnActor. */
    float Down = Player->CapsuleComponent->GetScaledCapsuleHalfHeight();
    FVector Feet = Player->K2_GetActorLocation() - FVector(0.0f, 0.0f, Down);
    FTransform Where = Feet + Player->GetActorForwardVector() * 300.0f;
    SpawnActor<AlarmLamp>(AlarmLamp::StaticClass(), Where);
  }
};

/* DRG's mod support spawns InitSpacerig in the Space Rig and InitCave in a mission. Both are empty, so whichever
   one the game spawns runs GameBlueprintChild's ReceiveBeginPlay. */
class InitSpacerig : public GameBlueprintChild {};
class InitCave : public GameBlueprintChild {};
