/* HotKeys: an actor that reacts to keys.

   Press F5 and a game message counts the presses; Ctrl+F5 sets the count back to zero. Pressing G posts "G down" and
   letting it go posts "G up", while G still does whatever the game binds it to. The game spawns InitSpacerig in the
   Space Rig and InitCave in a mission; both are HotKeys under another name. */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_AssetGenExamples/HotKeys");

class HotKeys : public AActor {
  int32 Presses = 0;

  /* An actor's key events fire only once it receives input. Auto Receive Input in the class defaults does that: the
     actor enables input for the first player as it is spawned. A possessed pawn and a player controller need nothing. */
  UE_DEFAULTS { AutoReceiveInput = EAutoReceiveInput::Player0; }

public:
  /* UE_INPUT_KEY makes a method a key event, the editor's InputKey node: the engine calls Count each time F5 goes
     down. The key is the engine's name of it, as the node's key picker lists it. */
  UE_INPUT_KEY("F5") void Count() {
    Presses += 1;
    Say(FString("F5 pressed ") + Presses + " times");
  }

  /* Ctrl+F5 is a key event of its own: a modifier is an option of the marker, as it is a tick box on the node. */
  UE_INPUT_KEY("F5", Ctrl) void Reset() {
    Presses = 0;
    Say("Count reset");
  }

  /* One method per edge, as the node has a Pressed and a Released pin. A method that takes the key is handed the one
     that fired it. NoConsume leaves G to the game's own controls as well: without it, the actor's input comes first
     and takes the key, so whatever the game does with G would stop while the actor has input. */
  UE_INPUT_KEY("G", NoConsume) void Down(FKey Key) { Say(Key.KeyName.ToString() + " down"); }
  UE_INPUT_KEY("G", Released, NoConsume) void Up(FKey Key) { Say(Key.KeyName.ToString() + " up"); }

private:
  /* Print String shows nothing in the retail game, so the text goes out as a game message. */
  inline void Say(FString Msg) { UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Msg); }
};

/* The game spawns a mod's InitSpacerig in the Space Rig and its InitCave in a mission. Both are empty, so whichever one
   it spawns is a HotKeys, with HotKeys' key events: the engine binds each class's events up the chain. */
class InitSpacerig : public HotKeys {};
class InitCave : public HotKeys {};
