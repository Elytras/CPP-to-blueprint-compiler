/* WaitForPlayer: finding the local player's dwarf, and finding it again when the game replaces it.

   A mod starts before the player's character exists, so it waits for one in a Delay loop. It greets the dwarf by
   class and health, then counts the seconds the dwarf spends sprinting. When the character is destroyed, for example
   when you change class in the Space Rig, the mod waits for the next one. In game you see "WaitForPlayer found your
   <class> with <health> health", and after a change of class also how long the previous dwarf sprinted. */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_AssetGenExamples/WaitForPlayer");

/* The mod's actor. Char is the dwarf it follows, and it is null while the actor waits for one. */
class WaitForPlayer : public AActor {
  APlayerCharacter *Char = nullptr;
  int32 Found = 0;
  float SprintSeconds = 0.0f;

public:
  /* Event BeginPlay. FindPlayer waits, so this call returns as soon as FindPlayer reaches its first Delay, and the
     rest of FindPlayer runs later. */
  void ReceiveBeginPlay() { FindPlayer(); }

  /* The dwarf's OnDestroyed calls this. A handler takes exactly the dispatcher's parameters, spelled as the header
     spells them: OnDestroyed is void(AActor *DestroyedActor). */
  void OnCharDestroyed(AActor *DestroyedActor) {
    Char = nullptr;
    FindPlayer();
  }

  /* Event Tick: overriding it turns ticking on for the actor. With no dwarf to watch, it returns at once. */
  void ReceiveTick(float DeltaSeconds) {
    if (!Char)
      return;
    if (Char->IsRunning)
      SprintSeconds += DeltaSeconds;
  }

private:
  /* Waits for the local dwarf and starts following it. ReceiveBeginPlay starts it, and OnCharDestroyed runs it again
     for the next character. */
  void FindPlayer() {
    /* First we wait. GetLocalPlayerCharacter returns null until the game has spawned the dwarf, so we ask again every
       half second: each Delay hands control back to the engine, and the loop resumes here. The Delay comes before
       the first look because OnCharDestroyed calls this while the old dwarf is still being destroyed and still
       valid. When the Delay ends the engine has marked it pending kill, and `!C`, the IsValid node, counts it as
       missing. */
    APlayerCharacter *C = nullptr;
    while (!C) {
      UKismetSystemLibrary::Delay(0.5f);
      C = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    }

    /* Then we bind: Bind Event to OnDestroyed, with the dwarf as Target. The game broadcasts it when it destroys the
       character, and each broadcast calls OnCharDestroyed on this actor. */
    C->OnDestroyed.Add(this, &WaitForPlayer::OnCharDestroyed);
    Char = C;
    Found += 1;

    /* GetHeroName is the character's own function and gives its class name. The health lives on its health
       component, which the character holds as a member. */
    FString Msg = FString("WaitForPlayer found your ") + C->GetHeroName() + " with " +
                  UKismetMathLibrary::Round(C->HealthComponent->GetHealth()) + " health";
    if (Found > 1)
      Msg = Msg + ". The previous dwarf sprinted for " + UKismetMathLibrary::Round(SprintSeconds) + " s";
    Say(Msg);
    SprintSeconds = 0.0f;
  }

  /* Print String shows nothing in the retail game, so we post the text through the game state instead. */
  inline void Say(FString Msg) { UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Msg); }
};

/* The game spawns a mod's InitSpacerig in the Space Rig and its InitCave in a mission. Both are
   empty, so whichever one the game spawns runs WaitForPlayer. */
class InitSpacerig : public WaitForPlayer {};
class InitCave : public WaitForPlayer {};
