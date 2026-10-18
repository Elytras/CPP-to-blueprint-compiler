/* FadingLamp: an actor that plays a timeline.

   As it spawns, a point light fades up over a second while it rises 100 units, then fades back down, three times over.
   Each time it reaches full brightness a game message says which run it is in. The game spawns InitSpacerig in the
   Space Rig and InitCave in a mission; both are FadingLamp under another name. */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_AssetGenExamples/FadingLamp");

class FadingLamp : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UPointLightComponent, Lamp);

  /* UE_TIMELINE is the editor's Timeline node: Fade, two seconds long, is a component the engine makes as the actor
     spawns. Each track declares the variable the timeline sets every frame it plays, from its keys, (time, value): a
     Cubic key eases in and out of it, the rest run in straight lines. */
  UE_TIMELINE(Fade, 2.0f);
  UE_FLOAT_TRACK(Fade, Brightness, (0, 0), (1, 5000, Cubic), (2, 0));
  UE_VECTOR_TRACK(Fade, Lift, (0, 0, 0, 0), (1, 0, 0, 100), (2, 0, 0, 0));

  int32 Runs = 0;

  /* The node's Update pin: the timeline calls OnFade each frame it plays, after it has set Brightness and Lift. */
  UE_TIMELINE_UPDATE(Fade) void OnFade() {
    FHitResult Hit;
    Lamp->SetIntensity(Brightness);
    Lamp->K2_SetRelativeLocation(Lift, false, Hit, false);
  }

  /* The Finished pin, at the end of the two seconds: play it again, three runs in all. */
  UE_TIMELINE_FINISHED(Fade) void OnFaded() {
    Runs += 1;
    if (Runs < 3) Fade->PlayFromStart();
  }

  /* An event track: the timeline calls Peak as it passes one second, the top of the fade. */
  UE_TIMELINE_EVENT(Fade, 1.0) void Peak() {
    UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(FString("Lamp at its brightest, run ") + (Runs + 1));
  }

  void ReceiveBeginPlay() {
    Lamp->SetIntensity(0.0f);
    Fade->PlayFromStart();
  }
};

/* The game spawns a mod's InitSpacerig in the Space Rig and its InitCave in a mission. Both are empty, so whichever one
   it spawns is a FadingLamp, with FadingLamp's timeline: the engine makes each class's timelines up the chain. */
class InitSpacerig : public FadingLamp {};
class InitCave : public FadingLamp {};
