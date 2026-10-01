#include "UeApi/Types.h"

#include "UeApi/AIModule.h"
#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ClassTailMeta");

/*
Children of native parents whose class tail is not the default one. The engine's compiler copies the
parent's ScriptInherit flags, its ClassWithin and its ClassConfigName into every Blueprint class, and nothing
at load time re-derives them: a HUD child that is not Transient saves its instances, a cheat manager child
not within PlayerController can be created anywhere, a settings child reads the wrong ini. Each class here
names what its parent passes on, from UE 4.27's UCLASS specifiers or from the game's own Blueprints.
*/

/* StatusEffect: every one of the game's 242 STE_ Blueprints carries EditInlineNew. */
class ClassTailMeta : public UStatusEffect {
public:
  int32 Stacks;
  int32 More() { return Stacks + 1; }
};

/* HUD.h:35 - config=Game, notplaceable, transient; globalconfig members make it CLASS_Config. */
class MetaHud : public AHUD {};

/* PlayerController.h:222 config=Game; NotPlaceable from Controller.h:39. */
class MetaController : public APlayerController {};

/* GameModeBase.h:45 - config=Game, notplaceable, Transient. */
class MetaMode : public AGameModeBase {};

/* CheatManager.h:87 - Within=PlayerController. */
class MetaCheats : public UCheatManager {
public:
  int32 N;
  void Bump() { N = N + 1; }
};

/* GameUserSettings.h:37 - config=GameUserSettings, configdonotcheckdefaults. */
class MetaSettings : public UGameUserSettings {};

/* Character.h:213 - config=Game. */
class MetaWalker : public ACharacter {};

/* BTNode.h:36 - config=Game, which every game BTTask_BlueprintBase child has. */
class MetaTask : public UBTTask_BlueprintBase {
public:
  int32 Runs;
};

/* DamageType.h:19 - const. */
class MetaDamage : public UDamageType {};

/* AnimInstance.h:361 - transient, Within=SkeletalMeshComponent. */
class MetaAnim : public UAnimInstance {};

/* PlayerInput.h:333 - Within=PlayerController, config=Input, transient. */
class MetaInput : public UPlayerInput {};
