/* HelloWorld: the smallest mod that runs on its own.

   One actor class greets you when the game spawns it, then counts to three, one step every two seconds. In game
   you see "Hello from AssetGen" posted as a game message, followed by "HelloWorld counted to 1", 2 and 3. The game
   spawns InitSpacerig in the Space Rig and InitCave in a mission; both are HelloWorld under another name. */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_AssetGenExamples/HelloWorld");

/* The mod's actor, cooked as the Blueprint class HelloWorld_C whose parent is Actor. The game never spawns it
   under this name: it spawns the two empty subclasses at the bottom, which inherit all of it. */
class HelloWorld : public AActor {
  /* Blueprint variables: each initializer is the variable's default under Class Defaults. */
  int32 Count = 0;
  int32 Goal = 3;

public:
  /* The engine calls this when the actor starts play: it is Event BeginPlay in the class's event graph. */
  void ReceiveBeginPlay() {
    Say("Hello from AssetGen");

    /* Then we count. Delay is a latent call, like the Delay node in an event graph: ReceiveBeginPlay hands control
       back to the engine at the first Delay, and each round of the loop resumes here two seconds later. */
    while (Count < Goal) {
      UKismetSystemLibrary::Delay(2.0f);
      Count += 1;
      Say(FString("HelloWorld counted to ") + Count);
    }
  }

private:
  /* ReceiveBeginPlay's way to show text. Print String shows nothing in the retail game, so we post the text
     through the game state instead. Say is inline, so it is no Blueprint function: each call pastes this body in. */
  inline void Say(FString Msg) { UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Msg); }
};

/* DRG's mod support spawns every mounted mod's InitSpacerig in the Space Rig and its InitCave in a mission. Both
   are empty, so whichever one the game spawns runs HelloWorld's ReceiveBeginPlay. */
class InitSpacerig : public HelloWorld {};
class InitCave : public HelloWorld {};
