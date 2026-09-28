/* AwaitEvents: methods that wait for a dispatcher to fire, with UE_AWAIT, and carry on with the value it broadcasts.

   The mod finds your dwarf, then posts a game message each time the dwarf goes down ("Down 1", "Down 2", ...) and
   greets each player who joins the team by name. Nothing polls for those two: the code stops at a UE_AWAIT line and
   goes on from there when the game broadcasts. The game spawns InitSpacerig in the Space Rig and InitCave in a
   mission; both are AwaitEvents under another name. */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_AssetGenExamples/AwaitEvents");

/* The mod's actor. FindDwarf announces your dwarf on the mod's own dispatcher, OnDwarfFound; CountDowns and
   GreetJoiners wait on dispatchers. A method that waits returns void and takes its parameters by value: it finishes
   after its caller has moved on. */
class AwaitEvents : public AActor {
  UE_DISPATCHER(OnDwarfFound, APlayerCharacter *Dwarf);
  int32 Downs = 0;

public:
  /* Event BeginPlay. A call to a method that waits returns as soon as that method reaches its first wait. So the two
     waiters below bind their dispatchers and return, and only then does FindDwarf start looking: a dwarf that is
     already there is broadcast to a listener that is ready for it. */
  void ReceiveBeginPlay() {
    CountDowns();
    GreetJoiners();
    FindDwarf();
  }

  /* Polls for your dwarf with Delay, the latent Delay node, since the game can start the mod before the character
     exists. Then it hands the dwarf to whoever waits on OnDwarfFound. */
  void FindDwarf() {
    APlayerCharacter *Dwarf = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    while (Dwarf == nullptr) {
      UKismetSystemLibrary::Delay(0.5f);
      Dwarf = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    }
    OnDwarfFound.Broadcast(Dwarf);
  }

  /* Counts your dwarf's downs. The method stops at each UE_AWAIT and resumes when that dispatcher fires, and the
     await's value is the dispatcher's one parameter: first the dwarf that FindDwarf broadcasts, then the dwarf's new
     state. The locals survive both waits, as they do after a Delay in an event graph.

     The event that AssetGen binds for an await stays bound. Every later state change runs the lines after the second
     await again, which is what makes this count every down and not only the first. */
  void CountDowns() {
    APlayerCharacter *Dwarf = UE_AWAIT(OnDwarfFound);
    ECharacterState State = UE_AWAIT(Dwarf->OnCharacterStateChanged);
    if (State == ECharacterState::Downed) {
      Downs += 1;
      Say(FString("Down ") + Downs);
    }
  }

  /* Greets each player who joins. In callback style you would bind a method of the class instead, as the editor's
     Bind Event node does, and move the code after the wait into it:

       Game->OnPlayerJoined.Add(this, &AwaitEvents::Greet);   // void Greet(AFSDPlayerState *Joined)

     An await continues on the one dispatcher it names: if another one fires, the method never resumes. For a node
     with several outcome pins, such as Play Montage with OnCompleted and OnInterrupted, bind a handler to each pin:
     Proxy->OnCompleted.Add(this, &AwaitEvents::Done). An async action (a UBlueprintAsyncActionBase such as Download
     Image) is awaited from a variable, as in UE_AWAIT(Task->OnSuccess), and AssetGen calls its Activate() right
     after the bind, as the editor's node does. */
  void GreetJoiners() {
    AFSDGameState *Game = UGameFunctionLibrary::GetFSDGameState(this);
    AFSDPlayerState *Joined = UE_AWAIT(Game->OnPlayerJoined);
    Say(Joined->GetPlayerName() + " joined the team");
  }

private:
  /* Posts a game message. Print String shows nothing in the retail game, so the text goes through the game state. */
  inline void Say(FString Msg) { UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Msg); }
};

/* The game spawns a mod's InitSpacerig in the Space Rig and its InitCave in a mission. Both are
   empty, so whichever one the game spawns runs AwaitEvents's ReceiveBeginPlay. */
class InitSpacerig : public AwaitEvents {};
class InitCave : public AwaitEvents {};
