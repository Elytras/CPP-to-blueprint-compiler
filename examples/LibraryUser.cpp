/* LibraryUser: a mod that calls another mod's function library.

   It includes MathLib.h and calls MathLib as it would an engine library. Once your dwarf exists, it posts a game
   message every ten seconds, such as "Health 85%, 42 m from where you started". The game spawns InitSpacerig in the
   Space Rig and InitCave in a mission; both are LibraryUser under another name. */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

#include "MathLib.h"

/* LibraryUser cooks only its own classes. It imports MathLib_C, so MathLib's assets must be in the game too.
   examples/mods.yaml builds both with these entries:

     mods:
       - name: MathLib
         sources: [MathLib.cpp, MathLib.h]
         generate_api: true
       - name: LibraryUser
         sources: [LibraryUser.cpp, MathLib.h]
         needs: [MathLib]
         embed: true

   needs builds MathLib first, and MathLib.h among the sources rebuilds LibraryUser when the header changes. embed
   packs MathLib's assets into LibraryUser_P.pak, so that pak works alone; without it, install MathLib_P.pak too.
   generate_api writes MathLib's editor stub, so a UE 4.27 project can call Percent and MetresFromPlayer from its own
   Blueprints. */
UE_MOD_PACKAGE("/Game/_AssetGenExamples/LibraryUser");

/* The mod's actor. Start is where your dwarf stood when the mod found it. */
class LibraryUser : public AActor {
  FVector Start;

public:
  void ReceiveBeginPlay() {
    /* First we wait for your dwarf: the game can start the mod before the character exists. Each Delay hands control
       back to the engine, and the loop resumes here half a second later. */
    APlayerCharacter *Dwarf = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    while (!Dwarf) {
      UKismetSystemLibrary::Delay(0.5f);
      Dwarf = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    }

    /* Then we note where it stands and start a looping timer: Set Timer by Event, with Looping ticked. */
    Start = Dwarf->K2_GetActorLocation();
    UKismetSystemLibrary::K2_SetTimerDelegate({this, &LibraryUser::Report}, 10.0f, true, 0.0f, 0.0f);
  }

  /* The looping timer calls this every ten seconds. Each MathLib call runs in the MathLib mod, on the library's class
     default object, as a call to a game library made in Blueprint would. MetresFromPlayer leaves its world context
     out, so it gets this actor. */
  void Report() {
    APlayerCharacter *Dwarf = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    if (!Dwarf)
      return;
    UPlayerHealthComponent *Health = Dwarf->HealthComponent;
    int32 Hp = MathLib::Percent(Health->GetHealth(), Health->MaxHealth);
    FString Msg = FString("Health ") + Hp + "%, " + MathLib::MetresFromPlayer(Start) + " m from where you started";
    UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Msg);
  }
};

/* The classes the game spawns. Both are empty, so whichever one it spawns runs LibraryUser. */
class InitSpacerig : public LibraryUser {};
class InitCave : public LibraryUser {};
