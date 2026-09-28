/* NetworkedSwitch: one lamp for the whole team, switched on and off with F5 by any player.

   When a level starts, the host hangs a lamp three metres above its dwarf. A player who presses F5 asks the host to
   flip it. The light then goes on or off on every screen, and every player reads "<name> switched the lamp on". Each
   player needs the mod installed: every machine builds its copy of the lamp from this mod's class. */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"
#include "../include/Objects.h" // AssetGen's include/ folder, by its path from this file

UE_MOD_PACKAGE("/Game/_AssetGenExamples/NetworkedSwitch");

/* The shared lamp. Only the host changes bOn, and the engine copies each change to every client. The replicated
   variable is what makes the class replicate: AssetGen sets Replicates (bReplicates) under its Class Defaults.
   bAlwaysRelevant keeps the lamp and its messages reaching players however far away they are. */
class SwitchLamp : public AActor {
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UPointLightComponent, Light);
  UE_REPLICATED_USING(bool, bOn, OnRep_On);

  UE_DEFAULTS {
    bAlwaysRelevant = true;
    Light->bVisible = false;
    Light->Intensity = 8000.0f;
    Light->LightColor = FColor(255, 180, 90);
  }

public:
  /* The RepNotify. The engine calls it on a client when a new bOn arrives. On the host it runs right after Flip's
     write, as the editor's Set w/ Notify node does. */
  void OnRep_On() { Light->SetVisibility(bOn, false); }

  /* Called on the host by a player's SwitchRemote. */
  void Flip(FString Who) {
    /* First we check authority: a client that wrote bOn would change its own copy and nobody else's. */
    if (!HasAuthority())
      return;
    bOn = !bOn;
    MulticastFlipped(Who, bOn);
  }

  /* Multicast: called on the host, it runs there and on every client. It is told the new state rather than reading
     bOn, because the call can reach a client before the new value of bOn does. */
  UE_MULTICAST UE_RELIABLE void MulticastFlipped(FString Who, bool bNowOn) {
    FString State = bNowOn ? FString("on") : FString("off");
    UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Who + " switched the lamp " + State);
  }
};

/* One per player, owned by that player's controller. The engine delivers a client's Server RPC only for an actor
   that client owns, and no player owns the shared lamp, so each player's request goes through a remote of its own.
   bOnlyRelevantToOwner sends each remote to its owner and to no other client. Its Server RPC makes the class replicate,
   as bOn does for the lamp, so the owner's machine has a copy whose ReceiveTick polls the key. */
class SwitchRemote : public AActor {
  FKey FlipKey = FKey{"F5"};

  UE_DEFAULTS { bOnlyRelevantToOwner = true; }

public:
  SwitchLamp *Lamp = nullptr;

  /* Every frame, on the machine of the player who owns this remote, we look for the key. */
  void ReceiveTick(float DeltaSeconds) {
    APlayerController *PC = Cast<APlayerController>(GetOwner());
    if (!PC || !PC->IsLocalController())
      return;
    if (PC->WasInputKeyJustPressed(FlipKey))
      ServerFlip();
  }

  /* Run on Server: called on a client, it runs on the host. Called on the host, it runs right there. */
  UE_SERVER UE_RELIABLE void ServerFlip() {
    APlayerController *PC = Cast<APlayerController>(GetOwner());
    if (Lamp && PC && PC->PlayerState)
      Lamp->Flip(PC->PlayerState->GetPlayerName());
  }
};

/* The mod's starter, spawned by the game as InitSpacerig or InitCave. It works on the host only: an actor that a
   client spawns exists on that client alone, so the lamp and the remotes have to come from the host. IsServer is
   true on the host and in a solo game, and false on a client. */
class NetworkedSwitch : public AActor {
  SwitchLamp *Lamp = nullptr;
  TArray<APlayerController *> Served;

public:
  void ReceiveBeginPlay() {
    if (!UKismetSystemLibrary::IsServer(this))
      return;

    /* First we wait for the host's dwarf, then hang the lamp above it. */
    APlayerCharacter *Me = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    while (Me == nullptr) {
      UKismetSystemLibrary::Delay(0.5f);
      Me = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    }
    FTransform Above = Me->K2_GetActorLocation() + FVector(0.0f, 0.0f, 300.0f);
    Lamp = SpawnActor<SwitchLamp>(SwitchLamp::StaticClass(), Above);

    /* Then, every two seconds, each player without a remote gets one, so a player who joins later can switch too.
       On the host, GetPlayerController walks every player's controller, not only the host's own. */
    while (true) {
      for (int32 I = 0; APlayerController *PC = UGameplayStatics::GetPlayerController(I); ++I) {
        if (Served.Contains(PC))
          continue;
        SwitchRemote *Remote = SpawnActor<SwitchRemote>(SwitchRemote::StaticClass(), FTransform(), PC);
        Remote->Lamp = Lamp;
        Served.Add(PC);
      }
      UKismetSystemLibrary::Delay(2.0f);
    }
  }
};

/* The game spawns a mod's InitSpacerig in the Space Rig and its InitCave in a mission. Both are NetworkedSwitch. */
class InitSpacerig : public NetworkedSwitch {};
class InitCave : public NetworkedSwitch {};
