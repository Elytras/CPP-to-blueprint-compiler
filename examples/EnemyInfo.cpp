/* EnemyInfo: a field guide kept as data assets.

   Each entry of the guide is a data asset the mod cooks, written with braces the way you fill in a Data Asset's
   details. An entry points at the game's own descriptor of the enemy and names a Miner's Manual icon by its path.
   When the game spawns the mod, it loads each entry's icon and enemy class in the background and posts one game
   message per entry: the title and tip, the difficulty rating from the game's descriptor, and what it loaded. */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"
#include "UeAssets/UEnemyDescriptor.h"

UE_MOD_PACKAGE("/Game/_AssetGenExamples/EnemyInfo");

/* UE_ASSET_AT names one asset the game already holds, by its path, so &ED_Spider_Tank points at it. AssetGen does not
   check the path, so copy it exactly. The UeAssets header above holds such a line for every enemy descriptor in the
   game, in namespaces that follow its folders. */
UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Tank, "/Game/Enemies/Spider/Tank/ED_Spider_Tank");

/* One entry of the guide: a data asset class of the mod's own, cooked as the Blueprint class UFieldNote_C with
   PrimaryDataAsset as its parent. Its members are public, so an entry can be written with braces. */
class UFieldNote : public UPrimaryDataAsset {
public:
  FString Title;
  FString Tip = "Keep your distance.";
  /* A hard reference: the descriptor loads together with the entry. */
  UEnemyDescriptor *Descriptor = nullptr;
  /* A soft reference: only a path until something loads it, so the texture does not load with the entry. */
  TSoftObjectPtr<UTexture2D> Icon;
};

/* The entries, cooked as Exploder and Praetorian in the mod's folder. Only the members the braces name are written,
   so Praetorian keeps the class's Tip. A soft reference takes the asset's path, where "/Game/Dir/Pkg" means the object
   Pkg.Pkg. */
UFieldNote Exploder = {
    .Title = "Glyphid Exploder",
    .Tip = "Shoot it before it reaches you.",
    .Descriptor = &UeAssets::UEnemyDescriptor::Game::Enemies::Spider::Exploder::ED_Spider_Exploder,
    .Icon = "/Game/UI/Menu_MinersManual/Assets/Icons/Creatures/Icon_Glyphid_Exploder_1024x256px"};
UFieldNote Praetorian = {
    .Title = "Glyphid Praetorian",
    .Descriptor = &ED_Spider_Tank,
    .Icon = "/Game/UI/Menu_MinersManual/Assets/Icons/Creatures/Icon_Glyphid_Praetorian_1024x256px"};

/* The mod's actor: it holds the guide and reads it out when the game spawns it. */
class EnemyInfo : public AActor {
  /* Pointing a variable's default at the entries is what picking them under Class Defaults does: they load together
     with this class. */
  TArray<UFieldNote *> Guide = {&Exploder, &Praetorian};

public:
  /* Event BeginPlay: one message per entry, in the order of Guide. */
  void ReceiveBeginPlay() {
    for (UFieldNote *Note : Guide) {
      UEnemyDescriptor *Desc = Note->Descriptor;

      /* First we load what the entry only names. LoadAsset is the Async Load Asset node: ReceiveBeginPlay waits at
         the call while the engine loads the icon, and the call's value is the texture, or null if nothing loaded.
         The descriptor names its enemy's class the same way, with a soft class reference, and LoadAssetClass (Async
         Load Class Asset) loads that. The loop and its locals carry on after each wait. */
      UObject *Icon = UKismetSystemLibrary::LoadAsset(Note->Icon);
      UClass *Enemy = UKismetSystemLibrary::LoadAssetClass(Desc->EnemyClass);

      FString Line = Note->Title + ": " + Note->Tip + " Difficulty " + Desc->DifficultyRating;
      Say(Line + ", icon " + Icon + ", class " + Enemy);
    }
  }

private:
  /* Print String shows nothing in the retail game, so the messages go through the game state. */
  inline void Say(FString Msg) { UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Msg); }
};

/* The game spawns InitSpacerig in the Space Rig and InitCave in a mission. Both are EnemyInfo under another name. */
class InitSpacerig : public EnemyInfo {};
class InitCave : public EnemyInfo {};
