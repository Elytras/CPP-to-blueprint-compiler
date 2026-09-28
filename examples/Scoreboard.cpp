/* Scoreboard: counting what the player does, and telling whoever listens.

   The mod binds four of the local player's dispatchers (jump, salute, shout, flare), counts each feat in a map keyed
   by name, and after every feat broadcasts an event dispatcher of its own. Its announcer listens on that dispatcher.
   In game it posts a game message on every salute and on every tenth of anything else, such as
   "Jumps 10. Board: Jumps 10 Salutes 2". */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_AssetGenExamples/Scoreboard");

/* The feats the board counts. UE_ENUM cooks the enum as a Blueprint enum asset, so a struct can hold one. */
enum class EFeat : uint8 { Jump, Salute, Shout, Flare };
UE_ENUM(EFeat);

/* What OnScored hands its listeners, as one value: a Blueprint Structure asset. */
struct FScoreChange {
  UE_STRUCT;
  EFeat Feat;
  FName Name;
  int32 Total;
};

/* The board as other code sees it. A listener that holds the board only as an actor asks it for scores through this
   interface, so it does not need the Scoreboard class. Code in another mod can do the same once IScoreKeeper moves into
   a shared header with UE_CLASS("/Game/_AssetGenExamples/Scoreboard/IScoreKeeper", "IScoreKeeper_C"), so that this
   mod cooks it and the other imports it (GUIDE.md, Several mods and shared code). A copy declared in another mod
   would be a separate interface that the board does not implement. */
class IScoreKeeper {
public:
  UE_INTERFACE;
  int32 GetScore(FName Name);
  FString Summary();
};

/* The mod's actor. It waits for the local player, binds the player's dispatchers and keeps the counts. The game
   spawns it as InitSpacerig or InitCave, the empty subclasses at the bottom. */
class Scoreboard : public AActor, public IScoreKeeper {
  TMap<FName, int32> Scores;
  APlayerCharacter *Player = nullptr;

public:
  /* The editor's Event Dispatcher. Score calls it after every feat, and whatever is bound to it runs: Announce
     below, or a handler in another mod that found the board, once Scoreboard is shared the same way. */
  UE_DISPATCHER(OnScored, FScoreChange Change, AActor *Board);

  void ReceiveBeginPlay() {
    OnScored.Add(this, &Scoreboard::Announce);

    /* GetLocalPlayerCharacter returns null until the character exists, so first we wait for it, a second at a time.
       When the game replaces the character, these bindings go with it; WaitForPlayer.cpp shows how to bind again. */
    Player = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    while (Player == nullptr) {
      UKismetSystemLibrary::Delay(1.0f);
      Player = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    }
    Player->OnJumpPressed.Add(this, &Scoreboard::Jumped);
    Player->OnSaluteEvent.Add(this, &Scoreboard::Saluted);
    Player->OnPlayerShout.Add(this, &Scoreboard::Shouted);
    Player->OnFlareThrown.Add(this, &Scoreboard::ThrewFlare);
  }

  /* The player's dispatchers call these. Each handler takes exactly the parameters its dispatcher declares. */
  void Jumped() { Score(EFeat::Jump); }
  void Saluted() { Score(EFeat::Salute); }
  void Shouted(APlayerCharacter *ShoutingPlayer) { Score(EFeat::Shout); }
  void ThrewFlare() { Score(EFeat::Flare); }

  /* IScoreKeeper. Reading Scores[Name] is the Find node: a name not in the map reads as 0 and is not added. */
  int32 GetScore(FName Name) { return Scores[Name]; }

  FString Summary() {
    FString Text = "Board:";
    for (auto &[Name, Count] : Scores) Text = Text + " " + Name + " " + Count;
    return Text;
  }

  /* Bound to OnScored in ReceiveBeginPlay. It is written as a listener in another mod would be: it gets the board as
     an actor and reads it through IScoreKeeper. An actor that does not implement the interface gives an empty one. */
  void Announce(FScoreChange Change, AActor *Board) {
    if (Change.Feat != EFeat::Salute && Change.Total % 10 != 0) return;
    TScriptInterface<IScoreKeeper> Keeper = Board;
    if (!Keeper) return;
    Say(Change.Name + " " + Change.Total + ". " + Keeper->Summary());
  }

private:
  /* The handlers' common body. `Scores[Name] += 1` finds the count (0 for a new name) and adds it back one higher. */
  void Score(EFeat Feat) {
    FName Name = NameOf(Feat);
    Scores[Name] += 1;
    FScoreChange Change = {.Feat = Feat, .Name = Name, .Total = Scores[Name]};
    OnScored.Broadcast(Change, this);
  }

  FName NameOf(EFeat Feat) {
    switch (Feat) {
    case EFeat::Jump: return "Jumps";
    case EFeat::Salute: return "Salutes";
    case EFeat::Shout: return "Shouts";
    case EFeat::Flare: return "Flares";
    }
    return FName();
  }

  /* Print String shows nothing in the retail game, so the text goes out as a game message. */
  inline void Say(FString Msg) { UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Msg); }
};

/* The game spawns a mod's InitSpacerig in the Space Rig and its InitCave in a mission. Both are the Scoreboard. */
class InitSpacerig : public Scoreboard {};
class InitCave : public Scoreboard {};
